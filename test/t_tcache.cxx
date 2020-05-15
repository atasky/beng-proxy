/*
 * Copyright 2007-2020 CM4all GmbH
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

#include "tconstruct.hxx"
#include "translation/Cache.hxx"
#include "translation/Stock.hxx"
#include "translation/Handler.hxx"
#include "translation/Request.hxx"
#include "translation/Response.hxx"
#include "translation/Transformation.hxx"
#include "translation/Protocol.hxx"
#include "widget/View.hxx"
#include "http/Address.hxx"
#include "file_address.hxx"
#include "delegate/Address.hxx"
#include "cgi/Address.hxx"
#include "spawn/MountList.hxx"
#include "spawn/NamespaceOptions.hxx"
#include "pool/pool.hxx"
#include "PInstance.hxx"
#include "util/Cancellable.hxx"
#include "stopwatch.hxx"

#include <gtest/gtest.h>

#include <string.h>

class MyTranslationService final : public TranslationService {
public:
	/* virtual methods from class TranslationService */
	void SendRequest(struct pool &pool,
			 const TranslateRequest &request,
			 const StopwatchPtr &parent_stopwatch,
			 TranslateHandler &handler,
			 CancellablePointer &cancel_ptr) noexcept override;
};

struct Instance : PInstance {
	MyTranslationService ts;
	TranslationCache cache;

	Instance()
		:cache(root_pool, event_loop, ts, 1024) {}
};

const TranslateResponse *next_response, *expected_response;

void
MyTranslationService::SendRequest(struct pool &pool,
				  gcc_unused const TranslateRequest &request,
				  const StopwatchPtr &,
				  TranslateHandler &handler,
				  gcc_unused CancellablePointer &cancel_ptr) noexcept
{
	if (next_response != nullptr) {
		auto response = NewFromPool<MakeResponse>(pool, pool, *next_response);
		handler.OnTranslateResponse(*response);
	} else
		handler.OnTranslateError(std::make_exception_ptr(std::runtime_error("Error")));
}

static bool
string_equals(const char *a, const char *b)
{
	if (a == nullptr || b == nullptr)
		return a == nullptr && b == nullptr;

	return strcmp(a, b) == 0;
}

template<typename T>
static bool
buffer_equals(ConstBuffer<T> a, ConstBuffer<T> b)
{
	if (a.IsNull() || b.IsNull())
		return a.IsNull() == b.IsNull();

	return a.size == b.size && memcmp(a.data, b.data, a.ToVoid().size) == 0;
}

static bool
operator==(const MountList &a, const MountList &b) noexcept
{
	return strcmp(a.source, b.source) == 0 &&
		strcmp(a.target, b.target) == 0 &&
		a.expand_source == b.expand_source;
}

static bool
Equals(const MountList *a, const MountList *b)
{
	for (; a != nullptr; a = a->next, b = b->next)
		if (b == nullptr || !(*a == *b))
			return false;

	return b == nullptr;
}

static bool
operator==(const MountNamespaceOptions &a,
	   const MountNamespaceOptions &b) noexcept
{
	return Equals(a.mounts, b.mounts);
}

static bool
operator==(const NamespaceOptions &a, const NamespaceOptions &b) noexcept
{
	return a.mount == b.mount;
}

static bool
operator==(const ChildOptions &a, const ChildOptions &b) noexcept
{
	return a.ns == b.ns;
}

static bool
operator==(const DelegateAddress &a, const DelegateAddress &b) noexcept
{
	return string_equals(a.delegate, b.delegate) &&
		a.child_options == b.child_options;
}

static bool
operator==(const HttpAddress &a, const HttpAddress &b) noexcept
{
	return string_equals(a.host_and_port, b.host_and_port) &&
		string_equals(a.path, b.path);
}

static bool
operator==(const ResourceAddress &a, const ResourceAddress &b) noexcept
{
	if (a.type != b.type)
		return false;

	switch (a.type) {
	case ResourceAddress::Type::NONE:
		return true;

	case ResourceAddress::Type::LOCAL:
		EXPECT_NE(a.GetFile().path, nullptr);
		EXPECT_NE(b.GetFile().path, nullptr);

		return string_equals(a.GetFile().path, b.GetFile().path) &&
			string_equals(a.GetFile().deflated, b.GetFile().deflated) &&
			string_equals(a.GetFile().gzipped, b.GetFile().gzipped) &&
			string_equals(a.GetFile().base, b.GetFile().base) &&
			string_equals(a.GetFile().content_type, b.GetFile().content_type) &&
			string_equals(a.GetFile().document_root, b.GetFile().document_root) &&
			(a.GetFile().delegate == nullptr) == (b.GetFile().delegate == nullptr) &&
			(a.GetFile().delegate == nullptr ||
			 *a.GetFile().delegate == *b.GetFile().delegate);

	case ResourceAddress::Type::CGI:
		EXPECT_NE(a.GetCgi().path, nullptr);
		EXPECT_NE(b.GetCgi().path, nullptr);

		return a.GetCgi().options == b.GetCgi().options &&
			string_equals(a.GetCgi().path, b.GetCgi().path) &&
			string_equals(a.GetCgi().interpreter, b.GetCgi().interpreter) &&
			string_equals(a.GetCgi().action, b.GetCgi().action) &&
			string_equals(a.GetCgi().uri, b.GetCgi().uri) &&
			string_equals(a.GetCgi().script_name, b.GetCgi().script_name) &&
			string_equals(a.GetCgi().path_info, b.GetCgi().path_info) &&
			string_equals(a.GetCgi().query_string, b.GetCgi().query_string) &&
			string_equals(a.GetCgi().document_root, b.GetCgi().document_root);

	case ResourceAddress::Type::HTTP:
		return a.GetHttp() == b.GetHttp();

	default:
		/* not implemented */
		EXPECT_TRUE(false);
		return false;
	}
}

static bool
operator==(const Transformation &a, const Transformation &b) noexcept
{
	if (a.type != b.type)
		return false;

	switch (a.type) {
	case Transformation::Type::PROCESS:
		return a.u.processor.options == b.u.processor.options;

	case Transformation::Type::PROCESS_CSS:
		return a.u.css_processor.options == b.u.css_processor.options;

	case Transformation::Type::PROCESS_TEXT:
		return true;

	case Transformation::Type::FILTER:
		return a.u.filter.address == b.u.filter.address;

	case Transformation::Type::SUBST:
		return string_equals(a.u.subst.yaml_file, b.u.subst.yaml_file);
	}

	/* unreachable */
	EXPECT_TRUE(false);
	return false;
}

static bool
transformation_chain_equals(const Transformation *a,
			    const Transformation *b)
{
	while (a != nullptr && b != nullptr) {
		if (!(*a == *b))
			return false;

		a = a->next;
		b = b->next;
	}

	return a == nullptr && b == nullptr;
}

static bool
operator==(const WidgetView &a, const WidgetView &b) noexcept
{
	return string_equals(a.name, b.name) &&
		a.address == b.address &&
		a.filter_4xx == b.filter_4xx &&
		transformation_chain_equals(a.transformation, b.transformation);
}

static bool
view_chain_equals(const WidgetView *a, const WidgetView *b)
{
	while (a != nullptr && b != nullptr) {
		if (!(*a == *b))
			return false;

		a = a->next;
		b = b->next;
	}

	return a == nullptr && b == nullptr;
}

static bool
operator==(const TranslateResponse &a, const TranslateResponse &b) noexcept
{
	return string_equals(a.base, b.base) &&
		a.regex_tail == b.regex_tail &&
		string_equals(a.regex, b.regex) &&
		string_equals(a.inverse_regex, b.inverse_regex) &&
		a.easy_base == b.easy_base &&
		a.unsafe_base == b.unsafe_base &&
		string_equals(a.uri, b.uri) &&
		string_equals(a.redirect, b.redirect) &&
		string_equals(a.test_path, b.test_path) &&
		buffer_equals(a.check, b.check) &&
		buffer_equals(a.want_full_uri, b.want_full_uri) &&
		a.address == b.address &&
		view_chain_equals(a.views, b.views);
}

class MyTranslateHandler final : public TranslateHandler {
public:
	/* virtual methods from TranslateHandler */
	void OnTranslateResponse(TranslateResponse &response) noexcept override;
	void OnTranslateError(std::exception_ptr error) noexcept override;
};

void
MyTranslateHandler::OnTranslateResponse(TranslateResponse &response) noexcept
{
	ASSERT_NE(expected_response, nullptr);
	EXPECT_EQ(response, *expected_response);
}

void
MyTranslateHandler::OnTranslateError(std::exception_ptr) noexcept
{
	EXPECT_EQ(expected_response, nullptr);
}

TEST(TranslationCache, Basic)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/");
	const auto response1 = MakeResponse(*pool).File("/var/www/index.html");
	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	next_response = nullptr;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/foo/bar.html");
	const auto response2 = MakeResponse(*pool).Base("/foo/")
		.File("bar.html", "/srv/foo/");
	next_response = expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request3 = MakeRequest("/foo/index.html");
	const auto response3 = MakeResponse(*pool).Base("/foo/")
		.File("index.html", "/srv/foo/");
	next_response = nullptr;
	expected_response = &response3;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request4 = MakeRequest("/foo/");
	const auto response4 = MakeResponse(*pool).Base("/foo/")
		.File(".", "/srv/foo/");
	expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request5 = MakeRequest("/foo");
	expected_response = nullptr;
	cache.SendRequest(*pool, request5, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request10 = MakeRequest("/foo//bar");
	const auto response10 = MakeResponse(*pool).Base("/foo/")
		.File("bar", "/srv/foo/");
	expected_response = &response10;
	cache.SendRequest(*pool, request10, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request6 = MakeRequest("/cgi1/foo");
	const auto response6 = MakeResponse(*pool).Base("/cgi1/")
		.Cgi("/usr/lib/cgi-bin/cgi.pl", "/cgi1/foo", "x/foo");

	next_response = expected_response = &response6;
	cache.SendRequest(*pool, request6, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request7 = MakeRequest("/cgi1/a/b/c");
	const auto response7 = MakeResponse(*pool).Base("/cgi1/")
		.Cgi("/usr/lib/cgi-bin/cgi.pl", "/cgi1/a/b/c", "x/a/b/c");

	next_response = nullptr;
	expected_response = &response7;
	cache.SendRequest(*pool, request7, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request8 = MakeRequest("/cgi2/foo");
	const auto response8 = MakeResponse(*pool).Base("/cgi2/")
		.Cgi("/usr/lib/cgi-bin/cgi.pl", "/cgi2/foo", "foo");

	next_response = expected_response = &response8;
	cache.SendRequest(*pool, request8, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request9 = MakeRequest("/cgi2/a/b/c");
	const auto response9 = MakeResponse(*pool).Base("/cgi2/")
		.Cgi("/usr/lib/cgi-bin/cgi.pl", "/cgi2/a/b/c", "a/b/c");

	next_response = nullptr;
	expected_response = &response9;
	cache.SendRequest(*pool, request9, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Feed the cache with a request to the BASE.  This was buggy until
 * 4.0.30.
 */
TEST(TranslationCache, BaseRoot)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/base_root/");
	const auto response1 = MakeResponse(*pool).Base("/base_root/")
		.File(".", "/var/www/");
	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/base_root/hansi");
	const auto response2 = MakeResponse(*pool).Base("/base_root/")
		.File("hansi", "/var/www/");
	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, BaseMismatch)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/base_mismatch/hansi");
	const auto response1 = MakeResponse(*pool).Base("/different_base/").File("/var/www/");

	next_response = &response1;
	expected_response = nullptr;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Test BASE+URI.
 */
TEST(TranslationCache, BaseUri)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/base_uri/foo");
	const auto response1 = MakeResponse(*pool).Base("/base_uri/")
		.File("foo", "/var/www/")
		.Uri("/modified/foo");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/base_uri/hansi");
	const auto response2 = MakeResponse(*pool).Base("/base_uri/")
		.File("hansi", "/var/www/")
		.Uri("/modified/hansi");

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Test BASE+REDIRECT.
 */
TEST(TranslationCache, BaseRedirect)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ref;

	const auto request1 = MakeRequest("/base_redirect/foo");
	const auto response1 = MakeResponse(*pool).Base("/base_redirect/")
		.File("foo", "/var/www/")
		.Redirect("http://modified/foo");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ref);

	const auto request2 = MakeRequest("/base_redirect/hansi");
	const auto response2 = MakeResponse(*pool).Base("/base_redirect/")
		.File("hansi", "/var/www/")
		.Redirect("http://modified/hansi");

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ref);
}

/**
 * Test BASE+TEST_PATH.
 */
TEST(TranslationCache, BaseTestPath)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/base_test_path/foo");
	const auto response1 = MakeResponse(*pool).Base("/base_test_path/")
		.File("foo", "/var/www/")
		.TestPath("/modified/foo");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/base_test_path/hansi");
	const auto response2 = MakeResponse(*pool).Base("/base_test_path/")
		.File("hansi", "/var/www/")
		.TestPath("/modified/hansi");

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, EasyBase)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	const auto request1 = MakeRequest("/easy/bar.html");

	const auto response1 = MakeResponse(*pool).EasyBase("/easy/")
		.File(".", "/var/www/");
	const auto response1b = MakeResponse(*pool).EasyBase("/easy/")
		.File("bar.html", "/var/www/");

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	next_response = &response1;
	expected_response = &response1b;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	next_response = nullptr;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/easy/index.html");
	const auto response2 = MakeResponse(*pool).EasyBase("/easy/")
		.File("index.html", "/var/www/");
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Test EASY_BASE+URI.
 */
TEST(TranslationCache, EasyBaseUri)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/easy_base_uri/foo");
	const auto response1 = MakeResponse(*pool).EasyBase("/easy_base_uri/")
		.File(".", "/var/www/")
		.Uri("/modified/");
	const auto response1b = MakeResponse(*pool).EasyBase("/easy_base_uri/")
		.File("foo", "/var/www/")
		.Uri("/modified/foo");

	next_response = &response1;
	expected_response = &response1b;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/easy_base_uri/hansi");
	const auto response2 = MakeResponse(*pool).EasyBase("/easy_base_uri/")
		.File("hansi", "/var/www/")
		.Uri("/modified/hansi");

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Test EASY_BASE + TEST_PATH.
 */
TEST(TranslationCache, EasyBaseTestPath)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/easy_base_test_path/foo");
	const auto response1 = MakeResponse(*pool).EasyBase("/easy_base_test_path/")
		.File(".", "/var/www/")
		.TestPath("/modified/");
	const auto response1b = MakeResponse(*pool).EasyBase("/easy_base_test_path/")
		.File("foo", "/var/www/")
		.TestPath("/modified/foo");

	next_response = &response1;
	expected_response = &response1b;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/easy_base_test_path/hansi");
	const auto response2 = MakeResponse(*pool).EasyBase("/easy_base_test_path/")
		.File("hansi", "/var/www/")
		.TestPath("/modified/hansi");

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, VaryInvalidate)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	static const TranslationCommand response5_vary[] = {
		TranslationCommand::QUERY_STRING,
	};

	static const TranslationCommand response5_invalidate[] = {
		TranslationCommand::QUERY_STRING,
	};

	const auto response5c = MakeResponse(*pool).File("/srv/qs3")
		.Vary(response5_vary).Invalidate(response5_invalidate);

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request6 = MakeRequest("/qs").QueryString("abc");
	const auto response5a = MakeResponse(*pool).File("/srv/qs1")
		.Vary(response5_vary);
	next_response = expected_response = &response5a;
	cache.SendRequest(*pool, request6, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request7 = MakeRequest("/qs").QueryString("xyz");
	const auto response5b = MakeResponse(*pool).File("/srv/qs2")
		.Vary(response5_vary);
	next_response = expected_response = &response5b;
	cache.SendRequest(*pool, request7, nullptr,
			  my_translate_handler, cancel_ptr);

	next_response = nullptr;
	expected_response = &response5a;
	cache.SendRequest(*pool, request6, nullptr,
			  my_translate_handler, cancel_ptr);

	next_response = nullptr;
	expected_response = &response5b;
	cache.SendRequest(*pool, request7, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request8 = MakeRequest("/qs/").QueryString("xyz");
	next_response = expected_response = &response5c;
	cache.SendRequest(*pool, request8, nullptr,
			  my_translate_handler, cancel_ptr);

	next_response = nullptr;
	expected_response = &response5a;
	cache.SendRequest(*pool, request6, nullptr,
			  my_translate_handler, cancel_ptr);

	next_response = expected_response = &response5c;
	cache.SendRequest(*pool, request7, nullptr,
			  my_translate_handler, cancel_ptr);

	next_response = expected_response = &response5c;
	cache.SendRequest(*pool, request8, nullptr,
			  my_translate_handler, cancel_ptr);

	expected_response = &response5c;
	cache.SendRequest(*pool, request7, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, InvalidateUri)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* feed the cache */

	const auto request1 = MakeRequest("/invalidate/uri");
	const auto response1 = MakeResponse(*pool).File("/var/www/invalidate/uri");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/invalidate/uri").Check("x");
	const auto response2 = MakeResponse(*pool).File("/var/www/invalidate/uri");
	next_response = expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request3 = MakeRequest("/invalidate/uri")
		.ErrorDocumentStatus(HTTP_STATUS_INTERNAL_SERVER_ERROR);
	const auto response3 = MakeResponse(*pool).File("/var/www/500/invalidate/uri");
	next_response = expected_response = &response3;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request4 = MakeRequest("/invalidate/uri")
		.ErrorDocumentStatus(HTTP_STATUS_INTERNAL_SERVER_ERROR)
		.Check("x");
	const auto response4 = MakeResponse(*pool).File("/var/www/500/check/invalidate/uri");

	next_response = expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request4b = MakeRequest("/invalidate/uri")
		.ErrorDocumentStatus(HTTP_STATUS_INTERNAL_SERVER_ERROR)
		.Check("x")
		.WantFullUri({ "a\0/b", 4 });
	const auto response4b = MakeResponse(*pool).File("/var/www/500/check/wfu/invalidate/uri");
	next_response = expected_response = &response4b;
	cache.SendRequest(*pool, request4b, nullptr,
			  my_translate_handler, cancel_ptr);

	/* verify the cache items */

	next_response = nullptr;

	expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	expected_response = &response3;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);

	expected_response = &response4b;
	cache.SendRequest(*pool, request4b, nullptr,
			  my_translate_handler, cancel_ptr);

	/* invalidate all cache items */

	const auto request5 = MakeRequest("/invalidate/uri")
		.ErrorDocumentStatus(HTTP_STATUS_NOT_FOUND);
	static const TranslationCommand response5_invalidate[] = {
		TranslationCommand::URI,
	};
	const auto response5 = MakeResponse(*pool).File("/var/www/404/invalidate/uri")
		.Invalidate(response5_invalidate);

	next_response = expected_response = &response5;
	cache.SendRequest(*pool, request5, nullptr,
			  my_translate_handler, cancel_ptr);

	/* check if all cache items have really been deleted */

	next_response = expected_response = nullptr;

	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);
	cache.SendRequest(*pool, request4b, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, Regex)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* add the "inverse_regex" test to the cache first */
	const auto request_i1 = MakeRequest("/regex/foo");
	const auto response_i1 = MakeResponse(*pool)
		.File("foo", "/var/www/regex/other/")
		.Base("/regex/").InverseRegex("\\.(jpg|html)$");
	next_response = expected_response = &response_i1;
	cache.SendRequest(*pool, request_i1, nullptr,
			  my_translate_handler, cancel_ptr);

	/* fill the cache */
	const auto request1 = MakeRequest("/regex/a/foo.jpg");
	const auto response1 = MakeResponse(*pool)
		.File("a/foo.jpg", "/var/www/regex/images/")
		.Base("/regex/").Regex("\\.jpg$");
	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	/* regex mismatch */
	const auto request2 = MakeRequest("/regex/b/foo.html");
	const auto response2 = MakeResponse(*pool)
		.File("b/foo.html", "/var/www/regex/html/")
		.Base("/regex/").Regex("\\.html$");
	next_response = expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	/* regex match */
	const auto request3 = MakeRequest("/regex/c/bar.jpg");
	const auto response3 = MakeResponse(*pool)
		.File("c/bar.jpg", "/var/www/regex/images/")
		.Base("/regex/").Regex("\\.jpg$");
	next_response = nullptr;
	expected_response = &response3;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	/* second regex match */
	const auto request4 = MakeRequest("/regex/d/bar.html");
	const auto response4 = MakeResponse(*pool)
		.File("d/bar.html", "/var/www/regex/html/")
		.Base("/regex/").Regex("\\.html$");
	next_response = nullptr;
	expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);

	/* see if the "inverse_regex" cache item is still there */
	const auto request_i2 = MakeRequest("/regex/bar");
	const auto response_i2 = MakeResponse(*pool)
		.File("bar", "/var/www/regex/other/")
		.Base("/regex/").InverseRegex("\\.(jpg|html)$");
	next_response = nullptr;
	expected_response = &response_i2;
	cache.SendRequest(*pool, request_i2, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, RegexError)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	const auto request = MakeRequest("/regex-error");
	const auto response = MakeResponse(*pool).File("/error")
		.Base("/regex/").Regex("(");

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* this must fail */
	next_response = &response;
	expected_response = nullptr;
	cache.SendRequest(*pool, request, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, RegexTail)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/regex_tail/a/foo.jpg");
	const auto response1 = MakeResponse(*pool)
		.File("a/foo.jpg", "/var/www/regex/images/")
		.Base("/regex_tail/").RegexTail("^a/");
	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/regex_tail/b/foo.html");
	next_response = expected_response = nullptr;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request3 = MakeRequest("/regex_tail/a/bar.jpg");
	const auto response3 = MakeResponse(*pool)
		.File("a/bar.jpg", "/var/www/regex/images/")
		.Base("/regex_tail/").RegexTail("^a/");
	next_response = nullptr;
	expected_response = &response3;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request4 = MakeRequest("/regex_tail/%61/escaped.html");

	next_response = expected_response = nullptr;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, RegexTailUnescape)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	const auto request1 = MakeRequest("/regex_unescape/a/foo.jpg");
	const auto response1 = MakeResponse(*pool)
		.File("a/foo.jpg", "/var/www/regex/images/")
		.Base("/regex_unescape/").RegexTailUnescape("^a/");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/regex_unescape/b/foo.html");

	next_response = expected_response = nullptr;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request3 = MakeRequest("/regex_unescape/a/bar.jpg");
	const auto response3 = MakeResponse(*pool)
		.File("a/bar.jpg", "/var/www/regex/images/")
		.Base("/regex_unescape/").RegexTailUnescape("^a/");

	next_response = nullptr;
	expected_response = &response3;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request4 = MakeRequest("/regex_unescape/%61/escaped.html");
	const auto response4 = MakeResponse(*pool)
		.File("a/escaped.html", "/var/www/regex/images/")
		.Base("/regex_unescape/").RegexTailUnescape("^a/");
	next_response = nullptr;
	expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, Expand)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* add to cache */

	const auto request1 = MakeRequest("/regex-expand/b=c");
	const auto response1n = MakeResponse(*pool)
		.Base("/regex-expand/").Regex("^/regex-expand/(.+=.+)$")
		.Cgi(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/foo.cgi").ExpandPathInfo("/a/\\1"));

	const auto response1e = MakeResponse(*pool)
		.Base("/regex-expand/").Regex("^/regex-expand/(.+=.+)$")
		.Cgi(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/foo.cgi", nullptr,
				    "/a/b=c"));

	next_response = &response1n;
	expected_response = &response1e;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	/* check match */

	const auto request2 = MakeRequest("/regex-expand/d=e");
	const auto response2 = MakeResponse(*pool)
		.Base("/regex-expand/").Regex("^/regex-expand/(.+=.+)$")
		.Cgi(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/foo.cgi", nullptr,
				    "/a/d=e"));

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, ExpandLocal)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* add to cache */

	const auto request1 = MakeRequest("/regex-expand2/foo/bar.jpg/b=c");
	const auto response1n = MakeResponse(*pool)
		.Base("/regex-expand2/")
		.Regex("^/regex-expand2/(.+\\.jpg)/([^/]+=[^/]+)$")
		.File(MakeFileAddress("/dummy").ExpandPath("/var/www/\\1"));

	const auto response1e = MakeResponse(*pool)
		.Base("/regex-expand2/")
		.Regex("^/regex-expand2/(.+\\.jpg)/([^/]+=[^/]+)$")
		.File(MakeFileAddress("/var/www/foo/bar.jpg"));

	next_response = &response1n;
	expected_response = &response1e;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	/* check match */

	const auto request2 = MakeRequest("/regex-expand2/x/y/z.jpg/d=e");
	const auto response2 = MakeResponse(*pool)
		.Base("/regex-expand2/")
		.Regex("^/regex-expand2/(.+\\.jpg)/([^/]+=[^/]+)$")
		.File("/var/www/x/y/z.jpg");

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, ExpandLocalFilter)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* add to cache */

	const auto request1 = MakeRequest("/regex-expand3/foo/bar.jpg/b=c");

	const auto response1n = MakeResponse(*pool)
		.Base("/regex-expand3/")
		.Regex("^/regex-expand3/(.+\\.jpg)/([^/]+=[^/]+)$")
		.Filter(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/image-processor.cgi").ExpandPathInfo("/\\2"))
		.File(MakeFileAddress("/dummy").ExpandPath("/var/www/\\1"));

	const auto response1e = MakeResponse(*pool)
		.Base("/regex-expand3/")
		.Regex("^/regex-expand3/(.+\\.jpg)/([^/]+=[^/]+)$")
		.Filter(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/image-processor.cgi", nullptr,
				       "/b=c"))
		.File(MakeFileAddress("/var/www/foo/bar.jpg"));

	next_response = &response1n;
	expected_response = &response1e;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	/* check match */

	const auto request2 = MakeRequest("/regex-expand3/x/y/z.jpg/d=e");
	const auto response2 = MakeResponse(*pool)
		.Base("/regex-expand3/")
		.Regex("^/regex-expand3/(.+\\.jpg)/([^/]+=[^/]+)$")
		.Filter(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/image-processor.cgi", nullptr,
				       "/d=e"))
		.File("/var/www/x/y/z.jpg");

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, ExpandUri)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* add to cache */

	const auto request1 = MakeRequest("/regex-expand4/foo/bar.jpg/b=c");
	const auto response1n = MakeResponse(*pool)
		.Base("/regex-expand4/")
		.Regex("^/regex-expand4/(.+\\.jpg)/([^/]+=[^/]+)$")
		.Http(MakeHttpAddress("/foo/bar.jpg").ExpandPath("/\\1"));
	const auto response1e = MakeResponse(*pool)
		.Base("/regex-expand4/")
		.Regex("^/regex-expand4/(.+\\.jpg)/([^/]+=[^/]+)$")
		.Http(MakeHttpAddress("/foo/bar.jpg"));

	next_response = &response1n;
	expected_response = &response1e;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	/* check match */

	const auto request2 = MakeRequest("/regex-expand4/x/y/z.jpg/d=e");
	const auto response2 = MakeResponse(*pool)
		.Base("/regex-expand4/")
		.Regex("^/regex-expand4/(.+\\.jpg)/([^/]+=[^/]+)$")
		.Http(MakeHttpAddress("/x/y/z.jpg"));

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, AutoBase)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* store response */

	const auto request1 = MakeRequest("/auto-base/foo.cgi/bar");
	const auto response1 = MakeResponse(*pool)
		.AutoBase()
		.Cgi("/usr/lib/cgi-bin/foo.cgi", "/auto-base/foo.cgi/bar", "/bar");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	/* check if BASE was auto-detected */

	const auto request2 = MakeRequest("/auto-base/foo.cgi/check");
	const auto response2 = MakeResponse(*pool)
		.AutoBase().Base("/auto-base/foo.cgi/")
		.Cgi("/usr/lib/cgi-bin/foo.cgi", "/auto-base/foo.cgi/check", "/check");

	next_response = nullptr;
	expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Test CHECK + BASE.
 */
TEST(TranslationCache, BaseCheck)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* feed the cache */

	const auto request1 = MakeRequest("/a/b/c.html");
	const auto response1 = MakeResponse(*pool).Base("/a/").Check("x");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/a/b/c.html").Check("x");
	const auto response2 = MakeResponse(*pool).Base("/a/b/")
		.File("c.html", "/var/www/vol0/a/b/");

	next_response = expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request3 = MakeRequest("/a/d/e.html").Check("x");
	const auto response3 = MakeResponse(*pool).Base("/a/d/")
		.File("e.html", "/var/www/vol1/a/d/");

	next_response = expected_response = &response3;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	/* now check whether the translate cache matches the BASE
	   correctly */

	next_response = nullptr;

	const auto request4 = MakeRequest("/a/f/g.html");
	const auto response4 = MakeResponse(*pool).Base("/a/").Check("x");

	expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request5 = MakeRequest("/a/b/0/1.html");

	cache.SendRequest(*pool, request5, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request6 = MakeRequest("/a/b/0/1.html").Check("x");
	const auto response6 = MakeResponse(*pool).Base("/a/b/")
		.File("0/1.html", "/var/www/vol0/a/b/");

	expected_response = &response6;
	cache.SendRequest(*pool, request6, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request7 = MakeRequest("/a/d/2/3.html").Check("x");
	const auto response7 = MakeResponse(*pool).Base("/a/d/")
		.File("2/3.html", "/var/www/vol1/a/d/");

	expected_response = &response7;
	cache.SendRequest(*pool, request7, nullptr,
			  my_translate_handler, cancel_ptr);

	/* expect cache misses */

	expected_response = nullptr;

	const auto miss1 = MakeRequest("/a/f/g.html").Check("y");
	cache.SendRequest(*pool, miss1, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Test WANT_FULL_URI + BASE.
 */
TEST(TranslationCache, BaseWantFullUri)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* feed the cache */

	const auto request1 = MakeRequest("/wfu/a/b/c.html");
	const auto response1 = MakeResponse(*pool).Base("/wfu/a/").WantFullUri("x");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/wfu/a/b/c.html").WantFullUri("x");
	const auto response2 = MakeResponse(*pool).Base("/wfu/a/b/")
		.File("c.html", "/var/www/vol0/a/b/");

	next_response = expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request3 = MakeRequest("/wfu/a/d/e.html").WantFullUri("x");
	const auto response3 = MakeResponse(*pool).Base("/wfu/a/d/")
		.File("e.html", "/var/www/vol1/a/d/");

	next_response = expected_response = &response3;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	/* now check whether the translate cache matches the BASE
	   correctly */

	next_response = nullptr;

	const auto request4 = MakeRequest("/wfu/a/f/g.html");
	const auto response4 = MakeResponse(*pool).Base("/wfu/a/").WantFullUri("x");

	expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request5 = MakeRequest("/wfu/a/b/0/1.html");

	cache.SendRequest(*pool, request5, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request6 = MakeRequest("/wfu/a/b/0/1.html").WantFullUri("x");
	const auto response6 = MakeResponse(*pool).Base("/wfu/a/b/")
		.File("0/1.html", "/var/www/vol0/a/b/");

	expected_response = &response6;
	cache.SendRequest(*pool, request6, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request7 = MakeRequest("/wfu/a/d/2/3.html").WantFullUri("x");
	const auto response7 = MakeResponse(*pool).Base("/wfu/a/d/")
		.File("2/3.html", "/var/www/vol1/a/d/");

	expected_response = &response7;
	cache.SendRequest(*pool, request7, nullptr,
			  my_translate_handler, cancel_ptr);

	/* expect cache misses */

	const auto miss1 = MakeRequest("/wfu/a/f/g.html").WantFullUri("y");
	expected_response = nullptr;
	cache.SendRequest(*pool, miss1, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Test UNSAFE_BASE.
 */
TEST(TranslationCache, UnsafeBase)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* feed */
	const auto request1 = MakeRequest("/unsafe_base1/foo");
	const auto response1 = MakeResponse(*pool).Base("/unsafe_base1/")
		.File("foo", "/var/www/");

	next_response = expected_response = &response1;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/unsafe_base2/foo");
	const auto response2 = MakeResponse(*pool).UnsafeBase("/unsafe_base2/")
		.File("foo", "/var/www/");

	next_response = expected_response = &response2;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	/* fail (no UNSAFE_BASE) */

	const auto request3 = MakeRequest("/unsafe_base1/../x");

	next_response = expected_response = nullptr;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	/* success (with UNSAFE_BASE) */

	const auto request4 = MakeRequest("/unsafe_base2/../x");
	const auto response4 = MakeResponse(*pool).UnsafeBase("/unsafe_base2/")
		.File("../x", "/var/www/");

	next_response = nullptr;
	expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);
}

/**
 * Test UNSAFE_BASE + EXPAND_PATH.
 */
TEST(TranslationCache, ExpandUnsafeBase)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* feed */

	const auto request1 = MakeRequest("/expand_unsafe_base1/foo");
	const auto response1 = MakeResponse(*pool).Base("/expand_unsafe_base1/")
		.Regex("^/expand_unsafe_base1/(.*)$")
		.File(MakeFileAddress("/var/www/foo.html").ExpandPath("/var/www/\\1.html"));
	const auto response1e = MakeResponse(*pool).Base("/expand_unsafe_base1/")
		.Regex("^/expand_unsafe_base1/(.*)$")
		.File(MakeFileAddress("/var/www/foo.html"));

	next_response = &response1;
	expected_response = &response1e;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/expand_unsafe_base2/foo");
	const auto response2 = MakeResponse(*pool).UnsafeBase("/expand_unsafe_base2/")
		.Regex("^/expand_unsafe_base2/(.*)$")
		.File(MakeFileAddress("/var/www/foo.html").ExpandPath("/var/www/\\1.html"));
	const auto response2e = MakeResponse(*pool).UnsafeBase("/expand_unsafe_base2/")
		.Regex("^/expand_unsafe_base2/(.*)$")
		.File(MakeFileAddress("/var/www/foo.html"));

	next_response = &response2;
	expected_response = &response2e;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);

	/* fail (no UNSAFE_BASE) */

	const auto request3 = MakeRequest("/expand_unsafe_base1/../x");

	next_response = expected_response = nullptr;
	cache.SendRequest(*pool, request3, nullptr,
			  my_translate_handler, cancel_ptr);

	/* success (with UNSAFE_BASE) */

	const auto request4 = MakeRequest("/expand_unsafe_base2/../x");
	const auto response4 = MakeResponse(*pool).UnsafeBase("/expand_unsafe_base2/")
		.Regex("^/expand_unsafe_base2/(.*)$")
		.File(MakeFileAddress("/var/www/../x.html"));

	next_response = nullptr;
	expected_response = &response4;
	cache.SendRequest(*pool, request4, nullptr,
			  my_translate_handler, cancel_ptr);
}

TEST(TranslationCache, ExpandBindMount)
{
	Instance instance;
	struct pool *pool = instance.root_pool;
	auto &cache = instance.cache;

	MyTranslateHandler my_translate_handler;
	CancellablePointer cancel_ptr;

	/* add to cache */

	const auto request1 = MakeRequest("/expand_bind_mount/foo");

	const auto response1n = MakeResponse(*pool).Base("/expand_bind_mount/")
		.Regex("^/expand_bind_mount/(.+)$")
		.Cgi(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/foo.cgi")
		     .BindMount("/home/\\1", "/mnt", true)
		     .BindMount("/etc", "/etc"));

	const auto response1e = MakeResponse(*pool).Base("/expand_bind_mount/")
		.Regex("^/expand_bind_mount/(.+)$")
		.Cgi(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/foo.cgi")
		     .BindMount("/home/foo", "/mnt")
		     .BindMount("/etc", "/etc"));

	next_response = &response1n;
	expected_response = &response1e;
	cache.SendRequest(*pool, request1, nullptr,
			  my_translate_handler, cancel_ptr);

	const auto request2 = MakeRequest("/expand_bind_mount/bar");
	const auto response2e = MakeResponse(*pool).Base("/expand_bind_mount/")
		.Regex("^/expand_bind_mount/(.+)$")
		.Cgi(MakeCgiAddress(*pool, "/usr/lib/cgi-bin/foo.cgi")
		     .BindMount("/home/bar", "/mnt")
		     .BindMount("/etc", "/etc"));

	next_response = nullptr;
	expected_response = &response2e;
	cache.SendRequest(*pool, request2, nullptr,
			  my_translate_handler, cancel_ptr);
}
