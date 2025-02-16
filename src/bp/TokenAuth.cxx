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

/*
 * #TranslationCommand::TOKEN_AUTH implementation.
 */

#include "Request.hxx"
#include "Connection.hxx"
#include "Instance.hxx"
#include "session/Lease.hxx"
#include "session/Session.hxx"
#include "http/IncomingRequest.hxx"
#include "translation/Handler.hxx"
#include "translation/Service.hxx"
#include "pool/pool.hxx"
#include "uri/PEscape.hxx"
#include "uri/Recompose.hxx"
#include "util/IterableSplitString.hxx"
#include "AllocatorPtr.hxx"

#include <stdexcept>

using std::string_view_literals::operator""sv;

static const char *
GetTokenAuthRedirectUri(AllocatorPtr alloc,
			const char *scheme, const char *host,
			DissectedUri dissected_uri,
			const TranslateResponse &response) noexcept
{
	/* TODO: deduplicate code from GetBounceUri() */

	if (response.uri != nullptr) {
		dissected_uri.base = response.uri;
		dissected_uri.path_info = {};
	}

	const char *uri_path = RecomposeUri(alloc, dissected_uri);

	return alloc.Concat(scheme, "://", host, uri_path);
}

inline void
Request::OnTokenAuthTranslateResponse(const TranslateResponse &response) noexcept
{
	assert(translate.previous != nullptr);

	if (response.discard_session)
		DiscardSession();
	else if (response.discard_realm_session)
		DiscardRealmSession();

	bool is_authenticated = false;
	{
		auto session = ApplyTranslateSession(response);
		if (session)
			is_authenticated = session->user != nullptr;
	}

	if (CheckHandleRedirectBounceStatus(response))
		return;

	if (!is_authenticated) {
		/* for some reason, the translation server did not send
		   REDIRECT/BOUNCE/STATUS, but we still don't have a user -
		   this should not happen; bail out, don't dare to accept the
		   client */
		DispatchError(HTTP_STATUS_FORBIDDEN, "Forbidden");
		return;
	}

	translate.user_modified = response.user != nullptr;

	if (!had_auth_token) {
		OnTranslateResponseAfterAuth(*translate.previous);
		return;
	}

	/* using the original translation response, because it may
	   have information about the original request */
	const auto &tr = *translate.previous;

	/* promote the "previous" response to the final response, so
	   GenerateSetCookie() uses its settings */
	translate.response = &tr;

	/* don't call OnTranslateResponseAfterAuth() here, instead
	   redirect to the URI with auth_token removed */

	const char *redirect_uri =
		GetTokenAuthRedirectUri(pool,
					GetExternalUriScheme(tr),
					GetExternalUriHost(tr),
					dissected_uri,
					tr);

	DispatchRedirect(HTTP_STATUS_SEE_OTHER, redirect_uri, nullptr);
}

inline void
Request::OnTokenAuthTranslateError(std::exception_ptr ep) noexcept
{
	LogDispatchError(HTTP_STATUS_BAD_GATEWAY,
			 "Configuration server failed", ep, 1);
}

class TokenAuthTranslateHandler final : public TranslateHandler {
	Request &request;

public:
	explicit TokenAuthTranslateHandler(Request &_request) noexcept
		:request(_request) {}

	/* virtual methods from TranslateHandler */
	void OnTranslateResponse(TranslateResponse &response) noexcept override {
		request.OnTokenAuthTranslateResponse(response);
	}

	void OnTranslateError(std::exception_ptr error) noexcept override {
		request.OnTokenAuthTranslateError(std::move(error));
	}
};

static std::string_view
ConcatQueryStrings(AllocatorPtr alloc, std::string_view a, std::string_view b) noexcept
{
	/* strip redundant ampersands */
	if (a.ends_with('&') && (b.empty() || b.starts_with('&')))
		a = a.substr(0, a.size() - 1);

	if (a.empty() && b.starts_with('&'))
		b = b.substr(1);

	/* shortcut: if both are empty, the query string is gone
	   completely */
	if (a.empty() && b.empty())
		return {};

	/* concatenate both parts */
	return alloc.Concat(a, b);
}

static std::string_view
RemoveFromQueryString(AllocatorPtr alloc, std::string_view q,
		      std::string_view name, std::string_view value) noexcept
{
	const auto a = Partition(q, name.data()).first;
	const auto b = Partition(q, value.data() + value.size()).second;

	return ConcatQueryStrings(alloc, a, b);
}

static const char *
ExtractAuthToken(AllocatorPtr alloc, DissectedUri &dissected_uri)
{
	char *auth_token = nullptr;

	for (const auto i : IterableSplitString(dissected_uri.query, '&')) {
		const auto [name, escaped_value] = Split(i, '=');
		if (name != "access_token"sv)
			continue;

		auth_token = uri_unescape_dup(alloc, escaped_value);
		if (auth_token == nullptr)
			throw std::invalid_argument("Malformed auth token");

		/* remove the "access_token" parameter from the query
		   string */
		dissected_uri.query =
			RemoveFromQueryString(alloc, dissected_uri.query,
					      name, escaped_value);

		break;
	}

	return auth_token;
}

void
Request::HandleTokenAuth(const TranslateResponse &response) noexcept
{
	assert(response.token_auth.data() != nullptr);

	/* we need to validate the session realm early */
	ApplyTranslateRealm(response, {});

	const AllocatorPtr alloc{pool};

	const char *auth_token;

	try {
		auth_token = ExtractAuthToken(alloc, dissected_uri);
	} catch (const std::invalid_argument &e) {
		DispatchError(HTTP_STATUS_BAD_REQUEST, e.what());
		return;
	}

	had_auth_token = auth_token != nullptr;

	bool is_authenticated = false;
	std::span<const std::byte> translate_realm_session{};

	if (auto session = GetRealmSession()) {
		is_authenticated = session->user != nullptr;
		translate_realm_session = alloc.Dup(std::span(session->translate));
	}

	if (auth_token == nullptr) {
		if (is_authenticated) {
			/* already authenticated; we can skip the
			   TOKEN_AUTH request */
			OnTranslateResponseAfterAuth(response);
			return;
		}
	}

	auto t = alloc.New<TranslateRequest>();
	t->token_auth = response.token_auth;
	t->auth_token = auth_token;
	if (auth_token == nullptr)
		t->recover_session = recover_session_from_cookie;
	t->uri = auth_token != nullptr
		? RecomposeUri(alloc, dissected_uri)
		: request.uri;
	t->listener_tag = translate.request.listener_tag;
	t->host = translate.request.host;
	t->session = translate.request.session;
	t->realm_session = translate_realm_session;

	translate.previous = &response;

	auto *http_auth_translate_handler =
		alloc.New<TokenAuthTranslateHandler>(*this);

	GetTranslationService().SendRequest(alloc, *t,
					    stopwatch,
					    *http_auth_translate_handler,
					    cancel_ptr);
}
