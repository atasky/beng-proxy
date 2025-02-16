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

#include "CookieClient.hxx"
#include "CookieJar.hxx"
#include "PCookieString.hxx"
#include "Quote.hxx"
#include "PTokenizer.hxx"
#include "strmap.hxx"
#include "pool/tpool.hxx"
#include "pool/pool.hxx"
#include "util/DeleteDisposer.hxx"
#include "util/StringCompare.hxx"
#include "util/StringStrip.hxx"
#include "util/StringView.hxx"
#include "AllocatorPtr.hxx"

#include <iterator>
#include <memory>

#include <stdlib.h>
#include <string.h>

using std::string_view_literals::operator""sv;

[[gnu::pure]]
static bool
domain_matches(const char *domain, const char *match) noexcept
{
	size_t domain_length = strlen(domain);
	size_t match_length = strlen(match);

	return domain_length >= match_length &&
		strcasecmp(domain + domain_length - match_length, match) == 0 &&
		(domain_length == match_length || /* "a.b" matches "a.b" */
		 match[0] == '.' || /* "a.b" matches ".b" */
		 /* "a.b" matches "b" (implicit dot according to RFC 2965
		    3.2.2): */
		 (domain_length > match_length &&
		  domain[domain_length - match_length - 1] == '.'));
}

[[gnu::pure]]
static bool
path_matches(const char *path, const char *match) noexcept
{
	return match == nullptr || StringStartsWith(path, match);
}

template<typename L>
static void
cookie_list_delete_match(L &list,
			 const char *domain, const char *path,
			 StringView name) noexcept
{
	assert(domain != nullptr);

	list.remove_and_dispose_if([=](const Cookie &cookie){
		return domain_matches(domain, cookie.domain.c_str()) &&
			(cookie.path == nullptr
			 ? path == nullptr
			 : path_matches(cookie.path.c_str(), path)) &&
			name.Equals(cookie.name.c_str());
	},
		DeleteDisposer{});
}

static std::unique_ptr<Cookie>
parse_next_cookie(struct pool &tpool,
		  std::string_view &input) noexcept
{
	auto [name, value] = cookie_next_name_value(tpool, input, false);
	if (name.empty())
		return nullptr;

	auto cookie = std::make_unique<Cookie>(name, value);

	input = StripLeft(input);
	while (!input.empty() && input.front() == ';') {
		input = input.substr(1);

		const auto nv = http_next_name_value(tpool, input);
		name = nv.first;
		value = nv.second;
		if (StringIsEqualIgnoreCase(name, "domain"sv))
			cookie->domain = value;
		else if (StringIsEqualIgnoreCase(name, "path"sv))
			cookie->path = value;
		else if (StringIsEqualIgnoreCase(name, "max-age"sv)) {
			unsigned long seconds;
			char *endptr;

			seconds = strtoul(p_strdup(tpool, value), &endptr, 10);
			if (*endptr == 0) {
				if (seconds == 0)
					cookie->expires = Expiry::AlreadyExpired();
				else
					cookie->expires.Touch(std::chrono::seconds(seconds));
			}
		}

		input = StripLeft(input);
	}

	return cookie;
}

static bool
apply_next_cookie(CookieJar &jar, struct pool &tpool, std::string_view &input,
		  const char *domain, const char *path) noexcept
{
	assert(domain != nullptr);

	auto cookie = parse_next_cookie(tpool, input);
	if (cookie == nullptr)
		return false;

	if (cookie->domain == nullptr) {
		cookie->domain = domain;
	} else if (!domain_matches(domain, cookie->domain.c_str())) {
		/* discard if domain mismatch */
		return false;
	}

	if (path != nullptr && cookie->path != nullptr &&
	    !path_matches(path, cookie->path.c_str())) {
		/* discard if path mismatch */
		return false;
	}

	/* delete the old cookie */
	cookie_list_delete_match(jar.cookies, cookie->domain.c_str(),
				 cookie->path.c_str(),
				 (std::string_view)cookie->name);

	/* add the new one */

	if (!cookie->value.empty() && cookie->expires != Expiry::AlreadyExpired())
		jar.Add(*cookie.release());

	return true;
}

void
cookie_jar_set_cookie2(CookieJar &jar, const char *value,
		       const char *domain, const char *path) noexcept
{
	const TempPoolLease tpool;

	std::string_view input = value;
	while (1) {
		if (!apply_next_cookie(jar, tpool, input, domain, path))
			break;

		if (input.empty())
			return;

		if (input.front() != ',')
			break;

		input = StripLeft(input.substr(1));
	}
}

const char *
cookie_jar_http_header_value(const CookieJar &jar,
			     const char *domain, const char *path,
			     AllocatorPtr alloc) noexcept
{
	static constexpr size_t buffer_size = 4096;

	assert(domain != nullptr);
	assert(path != nullptr);

	if (jar.cookies.empty())
		return nullptr;

	const TempPoolLease tpool;

	char *buffer = (char *)p_malloc(tpool, buffer_size);

	size_t length = 0;

	for (auto i = jar.cookies.begin(), end = jar.cookies.end(), next = i;
	     i != end; i = next) {
		next = std::next(i);

		auto *const cookie = &*i;

		if (!domain_matches(domain, cookie->domain.c_str()) ||
		    !path_matches(path, cookie->path.c_str()))
			continue;

		const StringView name(cookie->name.c_str());
		const StringView value(cookie->value.c_str());

		if (buffer_size - length < name.size + 1 + 1 + value.size * 2 + 1 + 2)
			break;

		if (length > 0) {
			buffer[length++] = ';';
			buffer[length++] = ' ';
		}

		memcpy(buffer + length, name.data, name.size);
		length += name.size;
		buffer[length++] = '=';
		if (http_must_quote_token(value))
			length += http_quote_string(buffer + length, value);
		else {
			memcpy(buffer + length, value.data, value.size);
			length += value.size;
		}
	}

	const char *value;
	if (length > 0)
		value = alloc.DupZ({buffer, length});
	else
		value = nullptr;

	return value;
}

void
cookie_jar_http_header(const CookieJar &jar,
		       const char *domain, const char *path,
		       StringMap &headers, AllocatorPtr alloc) noexcept
{
	const char *cookie =
		cookie_jar_http_header_value(jar, domain, path, alloc);

	if (cookie != nullptr) {
		headers.Add(alloc, "cookie2", "$Version=\"1\"");
		headers.Add(alloc, "cookie", cookie);
	}
}
