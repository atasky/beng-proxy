/*
 * Copyright 2007-2022 CM4all GmbH
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "AcmeClient.hxx"
#include "AcmeAccount.hxx"
#include "AcmeOrder.hxx"
#include "AcmeAuthorization.hxx"
#include "AcmeChallenge.hxx"
#include "AcmeError.hxx"
#include "AcmeConfig.hxx"
#include "JWS.hxx"
#include "json/String.hxx"
#include "json/ForwardList.hxx"
#include "jwt/RS256.hxx"
#include "lib/openssl/Buffer.hxx"
#include "lib/openssl/UniqueBIO.hxx"
#include "lib/sodium/Base64.hxx"
#include "util/AllocatedString.hxx"
#include "util/Exception.hxx"
#include "util/RuntimeError.hxx"

#include <boost/json.hpp>

#include <memory>

using std::string_view_literals::operator""sv;

[[gnu::pure]]
static bool
IsJson(const GlueHttpResponse &response) noexcept
{
	auto i = response.headers.find("content-type");
	if (i == response.headers.end())
		return false;

	const char *content_type = i->second.c_str();
	return strcmp(content_type, "application/json") == 0 ||
		strcmp(content_type, "application/jose+json") == 0 ||
		strcmp(content_type, "application/problem+json") == 0;
}

[[gnu::pure]]
static boost::json::value
ParseJson(GlueHttpResponse &&response)
{
	if (!IsJson(response))
		throw std::runtime_error("JSON expected");

	return boost::json::parse(response.body);
}

/**
 * Throw an exception if the given JSON document contains an "error"
 * element.
 */
static void
CheckThrowError(const boost::json::object &root)
{
	const auto *error = root.if_contains("error");
	if (error == nullptr)
		return;

	const auto *error_object = error->if_object();
	if (error_object != nullptr)
		throw AcmeError(*error_object);
}

/**
 * Throw an exception if the given JSON document contains an "error"
 * element.
 */
static void
CheckThrowError(const boost::json::object &root, const char *msg)
{
	try {
		CheckThrowError(root);
	} catch (...) {
		std::throw_with_nested(std::runtime_error(msg));
	}
}

/**
 * Throw an exception, adding "detail" from the JSON document (if the
 * response is JSON).
 */
[[noreturn]]
static void
ThrowError(GlueHttpResponse &&response, const char *msg)
{
	if (IsJson(response)) {
		const auto root = boost::json::parse(response.body);
		std::rethrow_exception(NestException(std::make_exception_ptr(AcmeError(root.as_object())),
						     std::runtime_error(msg)));
	}

	throw std::runtime_error(msg);
}

/**
 * Throw an exception due to unexpected status.
 */
[[noreturn]]
static void
ThrowStatusError(GlueHttpResponse &&response, const char *msg)
{
	std::string what(msg);
	what += " (";
	what += http_status_to_string(response.status);
	what += ")";

	ThrowError(std::move(response), what.c_str());
}

AcmeClient::AcmeClient(const AcmeConfig &config) noexcept
	:glue_http_client(event_loop),
	 server(config.staging
		? "https://acme-staging-v02.api.letsencrypt.org"
		: "https://acme-v02.api.letsencrypt.org"),
	 account_key_id(config.account_key_id),
	 fake(config.fake)
{
	if (config.debug)
		glue_http_client.EnableVerbose();
}

AcmeClient::~AcmeClient() noexcept = default;

static auto
tag_invoke(boost::json::value_to_tag<AcmeDirectory>,
	   const boost::json::value &jv)
{
	const auto &json = jv.as_object();
	AcmeDirectory directory;
	directory.new_nonce = Json::GetString(json, "newNonce");
	directory.new_account = Json::GetString(json, "newAccount");
	directory.new_order = Json::GetString(json, "newOrder");
	directory.new_authz = Json::GetString(json, "new-authz");
	directory.new_cert = Json::GetString(json, "new-cert");
	return directory;
}

void
AcmeClient::RequestDirectory()
{
	if (fake)
		return;

	unsigned remaining_tries = 3;
	while (true) {
		auto response = glue_http_client.Request(event_loop,
							 HTTP_METHOD_GET,
							 (server + "/directory").c_str(),
							 {});
		if (response.status != HTTP_STATUS_OK) {
			if (http_status_is_server_error(response.status) &&
			    --remaining_tries > 0)
				/* try again, just in case it's a
				   temporary Let's Encrypt hiccup */
				continue;

			throw FormatRuntimeError("Unexpected response status %d",
						 response.status);
		}

		if (!IsJson(response))
			throw std::runtime_error("JSON expected");

		directory = boost::json::value_to<AcmeDirectory>(boost::json::parse(response.body));
		break;
	}
}

void
AcmeClient::EnsureDirectory()
{
	if (fake)
		return;

	if (directory.new_nonce.empty())
		RequestDirectory();
}

std::string
AcmeClient::RequestNonce()
{
	if (fake)
		return "foo";

	EnsureDirectory();
	if (directory.new_nonce.empty())
		throw std::runtime_error("No newNonce in directory");

	unsigned remaining_tries = 3;
	while (true) {
		auto response = glue_http_client.Request(event_loop,
							 HTTP_METHOD_HEAD,
							 directory.new_nonce.c_str(),
							 {});
		if (response.status != HTTP_STATUS_OK) {
			if (http_status_is_server_error(response.status) &&
			    --remaining_tries > 0)
				/* try again, just in case it's a temporary Let's
				   Encrypt hiccup */
				continue;

			throw FormatRuntimeError("Unexpected response status %d",
						 response.status);
		}

		if (IsJson(response))
			directory = boost::json::value_to<AcmeDirectory>(boost::json::parse(response.body));

		auto nonce = response.headers.find("replay-nonce");
		if (nonce == response.headers.end())
			throw std::runtime_error("No Replay-Nonce response header");
		return nonce->second.c_str();
	}
}

std::string
AcmeClient::NextNonce()
{
	if (next_nonce.empty())
		next_nonce = RequestNonce();

	std::string result;
	std::swap(result, next_nonce);
	return result;
}

static boost::json::object
MakeHeader(EVP_PKEY &key, const char *url, const char *kid,
	   std::string_view nonce)
{
	boost::json::object root{
		{"alg", "RS256"},
		{"url", url},
		{"nonce", nonce},
	};
	if (kid != nullptr)
		root.emplace("kid", kid);
	else
		root.emplace("jwk", MakeJwk(key));
	return root;
}

GlueHttpResponse
AcmeClient::Request(http_method_t method, const char *uri,
		    std::span<const std::byte> body)
{
	auto response = fake
		? FakeRequest(method, uri, body)
		: glue_http_client.Request(event_loop,
					   method, uri,
					   body);

	auto new_nonce = response.headers.find("replay-nonce");
	if (new_nonce != response.headers.end())
		next_nonce = std::move(new_nonce->second);

	return response;
}

GlueHttpResponse
AcmeClient::Request(http_method_t method, const char *uri,
		    const boost::json::value &body)
{
	return Request(method, uri, boost::json::serialize(body));
}

GlueHttpResponse
AcmeClient::SignedRequest(EVP_PKEY &key,
			  http_method_t method, const char *uri,
			  std::span<const std::byte> payload)
{
	const auto payload_b64 = UrlSafeBase64(payload);

	const auto protected_header =
		boost::json::serialize(MakeHeader(key, uri,
						  account_key_id.empty()
						  ? nullptr
						  : account_key_id.c_str(),
						  NextNonce()));

	const auto protected_header_b64 = UrlSafeBase64(protected_header);

	const boost::json::object root{
		{"payload", payload_b64.c_str()},
		{"signature",
		 JWT::SignRS256(key, protected_header_b64.c_str(),
				payload_b64.c_str()).c_str()},
		{"protected", protected_header_b64.c_str()},
	};

	return Request(method, uri, root);
}

GlueHttpResponse
AcmeClient::SignedRequest(EVP_PKEY &key,
			  http_method_t method, const char *uri,
			  const boost::json::value &payload)
{
	return SignedRequest(key, method, uri,
			     boost::json::serialize(payload));
}

template<typename T>
static auto
WithLocation(T &&t, const GlueHttpResponse &response) noexcept
{
	auto location = response.headers.find("location");
	if (location != response.headers.end())
		t.location = std::move(location->second);

	return std::move(t);
}

static boost::json::string
MakeMailToString(const char *email) noexcept
{
	boost::json::string s("mailto:");
	s.append(email);
	return s;
}

static boost::json::array
MakeMailToArray(const char *email) noexcept
{
	return {MakeMailToString(email)};
}

static auto
MakeNewAccountRequest(const char *email, bool only_return_existing) noexcept
{
	boost::json::object root{
		{"termsOfServiceAgreed", true},
	};

	if (email != nullptr)
		root.emplace("contact", MakeMailToArray(email));

	if (only_return_existing)
		root.emplace("onlyReturnExisting", true);

	return root;
}

static auto
tag_invoke(boost::json::value_to_tag<AcmeAccount>,
	   const boost::json::value &jv)
{
	const auto &root = jv.as_object();

	AcmeAccount account;
	account.status = AcmeAccount::ParseStatus(root.at("status").as_string());

	const auto *contact = root.if_contains("contact");
	if (contact != nullptr)
		account.contact = boost::json::value_to<std::forward_list<std::string>>(*contact);

	return account;
}

AcmeAccount
AcmeClient::NewAccount(EVP_PKEY &key, const char *email,
		       bool only_return_existing)
{
	EnsureDirectory();
	if (directory.new_account.empty())
		throw std::runtime_error("No newAccount in directory");

	const auto payload = MakeNewAccountRequest(email, only_return_existing);

	auto response = SignedRequestRetry(key,
					   HTTP_METHOD_POST,
					   directory.new_account.c_str(),
					   payload);
	if (only_return_existing) {
		if (response.status != HTTP_STATUS_OK)
			ThrowStatusError(std::move(response),
					 "Failed to look up account");
	} else {
		if (response.status == HTTP_STATUS_OK) {
			const auto location = response.headers.find("location");
			if (location != response.headers.end())
				throw FormatRuntimeError("This key is already registered: %s",
							 location->second.c_str());
			else
				throw std::runtime_error("This key is already registered");
		}

		if (response.status != HTTP_STATUS_CREATED)
			ThrowStatusError(std::move(response),
					 "Failed to register account");
	}

	const auto root = ParseJson(std::move(response));
	CheckThrowError(root.as_object(), "Failed to create account");
	return WithLocation(boost::json::value_to<AcmeAccount>(root), response);
}

static boost::json::object
DnsIdentifierToJson(const std::string &value) noexcept
{
	return {
		{"type", "dns"},
		{"value", value},
	};
}

static boost::json::array
DnsIdentifiersToJson(const std::forward_list<std::string> &identifiers) noexcept
{
	boost::json::array root;
	for (const auto &i : identifiers)
		root.emplace_back(DnsIdentifierToJson(i));
	return root;
}

static void
tag_invoke(boost::json::value_from_tag, boost::json::value &jv,
	   const AcmeClient::OrderRequest &request) noexcept
{
	jv = {{"identifiers", DnsIdentifiersToJson(request.identifiers)}};
}

static auto
tag_invoke(boost::json::value_to_tag<AcmeOrder>,
	   const boost::json::value &jv)
{
	const auto &root = jv.as_object();

	AcmeOrder order;
	order.status = root.at("status").as_string();

	const auto *authorizations = root.if_contains("authorizations");
	if (authorizations != nullptr)
		order.authorizations = boost::json::value_to<std::forward_list<std::string>>(*authorizations);

	order.finalize = root.at("finalize").as_string();

	const auto *certificate = root.if_contains("certificate");
	if (certificate != nullptr)
		order.certificate = certificate->as_string();

	return order;
}

AcmeOrder
AcmeClient::NewOrder(EVP_PKEY &key, OrderRequest &&request)
{
	EnsureDirectory();
	if (directory.new_order.empty())
		throw std::runtime_error("No newOrder in directory");

	auto response = SignedRequestRetry(key,
					   HTTP_METHOD_POST,
					   directory.new_order.c_str(),
					   boost::json::value_from(request));
	if (response.status != HTTP_STATUS_CREATED)
		ThrowStatusError(std::move(response),
				 "Failed to create order");

	const auto root = ParseJson(std::move(response));
	CheckThrowError(root.as_object(), "Failed to create order");
	return WithLocation(boost::json::value_to<AcmeOrder>(root), response);
}

static boost::json::object
ToJson(X509_REQ &req) noexcept
{
	return {
		{"csr", UrlSafeBase64(SslBuffer(req).get()).c_str()},
	};
}

AcmeOrder
AcmeClient::FinalizeOrder(EVP_PKEY &key, const AcmeOrder &order,
			  X509_REQ &csr)
{
	auto response = SignedRequestRetry(key,
					   HTTP_METHOD_POST,
					   order.finalize.c_str(),
					   ToJson(csr));
	if (response.status != HTTP_STATUS_OK)
		ThrowStatusError(std::move(response),
				 "Failed to finalize order");

	const auto root = ParseJson(std::move(response));
	CheckThrowError(root.as_object(), "Failed to finalize order");
	return WithLocation(boost::json::value_to<AcmeOrder>(root), response);
}

UniqueX509
AcmeClient::DownloadCertificate(EVP_PKEY &key, const AcmeOrder &order)
{
	auto response = SignedRequestRetry(key,
					   HTTP_METHOD_POST,
					   order.certificate.c_str(),
					   ""sv);
	if (response.status != HTTP_STATUS_OK)
		ThrowStatusError(std::move(response),
				 "Failed to download certificate");

	auto ct = response.headers.find("content-type");
	if (ct == response.headers.end() ||
	    ct->second != "application/pem-certificate-chain")
		throw std::runtime_error("Wrong Content-Type in certificate download");

	UniqueBIO in(BIO_new_mem_buf(response.body.data(), response.body.length()));
	return UniqueX509((X509 *)PEM_ASN1_read_bio((d2i_of_void *)d2i_X509,
						    PEM_STRING_X509, in.get(),
						    nullptr, nullptr, nullptr));
}

static auto
tag_invoke(boost::json::value_to_tag<AcmeChallenge>,
	   const boost::json::value &jv)
{
	const auto &root = jv.as_object();

	AcmeChallenge challenge;
	challenge.type = root.at("type").as_string();
	challenge.uri = root.at("url").as_string();
	challenge.status = AcmeChallenge::ParseStatus(root.at("status").as_string());
	challenge.token = root.at("token").as_string();

	try {
		CheckThrowError(root);
	} catch (...) {
		challenge.error = std::current_exception();
	}

	return challenge;
}

static auto
tag_invoke(boost::json::value_to_tag<AcmeAuthorization>,
	   const boost::json::value &jv)
{
	const auto &root = jv.as_object();

	AcmeAuthorization response;
	response.status = AcmeAuthorization::ParseStatus(root.at("status").as_string());
	response.identifier = root.at("identifier").at("value").as_string();
	response.challenges = boost::json::value_to<std::forward_list<AcmeChallenge>>(root.at("challenges"));
	if (response.challenges.empty())
		throw std::runtime_error("No challenges");

	const auto *wildcard = root.if_contains("wildcard");
	response.wildcard = wildcard != nullptr && wildcard->as_bool();

	return response;
}

AcmeAuthorization
AcmeClient::Authorize(EVP_PKEY &key, const char *url)
{
	auto response = SignedRequestRetry(key,
					   HTTP_METHOD_POST,
					   url,
					   ""sv);
	if (response.status != HTTP_STATUS_OK)
		ThrowStatusError(std::move(response),
				 "Failed to request authorization");

	const auto root = ParseJson(std::move(response));
	CheckThrowError(root.as_object(), "Failed to request authorization");
	return boost::json::value_to<AcmeAuthorization>(root);
}

AcmeAuthorization
AcmeClient::PollAuthorization(EVP_PKEY &key, const char *url)
{
	auto response = SignedRequestRetry(key,
					   HTTP_METHOD_POST,
					   url,
					   ""sv);
	if (response.status != HTTP_STATUS_OK)
		ThrowStatusError(std::move(response),
				 "Failed to poll authorization");

	const auto root = ParseJson(std::move(response));
	CheckThrowError(root.as_object(), "Failed to poll authorization");
	return boost::json::value_to<AcmeAuthorization>(root);
}

AcmeChallenge
AcmeClient::UpdateChallenge(EVP_PKEY &key, const AcmeChallenge &challenge)
{
	auto response = SignedRequestRetry(key,
					   HTTP_METHOD_POST,
					   challenge.uri.c_str(),
					   boost::json::object{});
	if (response.status != HTTP_STATUS_OK)
		ThrowStatusError(std::move(response),
				 "Failed to update challenge");

	const auto root_value = ParseJson(std::move(response));
	const auto &root = root_value.as_object();
	CheckThrowError(root, "Failed to update challenge");
	return boost::json::value_to<AcmeChallenge>(root_value);
}
