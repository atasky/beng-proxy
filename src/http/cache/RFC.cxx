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

#include "RFC.hxx"
#include "Document.hxx"
#include "Internal.hxx"
#include "strmap.hxx"
#include "ResourceAddress.hxx"
#include "io/Logger.hxx"
#include "http/Date.hxx"
#include "http/PHeaderUtil.hxx"
#include "http/PList.hxx"
#include "util/IterableSplitString.hxx"
#include "util/StringCompare.hxx"
#include "util/StringStrip.hxx"
#include "AllocatorPtr.hxx"

#include <stdlib.h>

using std::string_view_literals::operator""sv;

/* check whether the request could produce a cacheable response */
std::optional<HttpCacheRequestInfo>
http_cache_request_evaluate(http_method_t method,
			    const ResourceAddress &address,
			    const StringMap &headers,
			    bool obey_no_cache,
			    bool has_request_body) noexcept
{
	if (method != HTTP_METHOD_GET || has_request_body)
		/* RFC 2616 13.11 "Write-Through Mandatory" */
		return std::nullopt;

	if (headers.Contains("range"))
		return std::nullopt;

	/* RFC 2616 14.8: "When a shared cache receives a request
	   containing an Authorization field, it MUST NOT return the
	   corresponding response as a reply to any other request
	   [...] */
	if (headers.Get("authorization") != nullptr)
		return std::nullopt;

	bool only_if_cached = false;

	if (const char *cache_control = headers.Get("cache-control")) {
		for (std::string_view s : IterableSplitString(cache_control, ',')) {
			s = Strip(s);

			if (obey_no_cache &&
			    (s == "no-cache"sv || s == "no-store"sv))
				return std::nullopt;

			if (s == "only-if-cached"sv)
				only_if_cached = true;
		}
	} else if (obey_no_cache) {
		if (const char *pragma = headers.Get("pragma");
		    pragma != nullptr && strcmp(pragma, "no-cache") == 0)
			return std::nullopt;
	}

	HttpCacheRequestInfo info;
	info.is_remote = address.type == ResourceAddress::Type::HTTP;
	info.only_if_cached = only_if_cached;
	info.has_query_string = address.HasQueryString();

	info.if_match = headers.Get("if-match");
	info.if_none_match = headers.Get("if-none-match");
	info.if_modified_since = headers.Get("if-modified-since");
	info.if_unmodified_since = headers.Get("if-unmodified-since");

	return info;
}

bool
http_cache_vary_fits(const StringMap &vary, const StringMap &headers) noexcept
{
	for (const auto &i : vary) {
		const char *value = headers.Get(i.key);
		if (value == nullptr)
			value = "";

		if (strcmp(i.value, value) != 0)
			/* mismatch in one of the "Vary" request headers */
			return false;
	}

	return true;
}

bool
http_cache_vary_fits(const StringMap *vary, const StringMap &headers) noexcept
{
	return vary == nullptr || http_cache_vary_fits(*vary, headers);
}

bool
http_cache_request_invalidate(http_method_t method) noexcept
{
	/* RFC 2616 13.10 "Invalidation After Updates or Deletions" */
	return method == HTTP_METHOD_PUT || method == HTTP_METHOD_DELETE ||
		method == HTTP_METHOD_POST;
}

[[gnu::pure]]
static std::chrono::system_clock::time_point
parse_translate_time(const char *p,
		     std::chrono::system_clock::duration offset) noexcept
{
	if (p == nullptr)
		return std::chrono::system_clock::from_time_t(-1);

	auto t = http_date_parse(p);
	if (t != std::chrono::system_clock::from_time_t(-1))
		t += offset;

	return t;
}

/**
 * RFC 2616 13.4
 */
static constexpr bool
http_status_cacheable(http_status_t status) noexcept
{
	return status == HTTP_STATUS_OK ||
		status == HTTP_STATUS_NON_AUTHORITATIVE_INFORMATION ||
		status == HTTP_STATUS_PARTIAL_CONTENT ||
		status == HTTP_STATUS_MULTIPLE_CHOICES ||
		status == HTTP_STATUS_MOVED_PERMANENTLY ||
		status == HTTP_STATUS_GONE;
}

/**
 * Determine the difference between this host's real-time clock and
 * the server's clock.  This is used to adjust the "Expires" time
 * stamp.
 *
 * @return the difference or min() if the server did not send a valid
 * "Date" header
 */
[[gnu::pure]]
static std::chrono::system_clock::duration
GetServerDateOffset(const HttpCacheRequestInfo &request_info,
		    std::chrono::system_clock::time_point now,
		    const StringMap &response_headers) noexcept
{
	if (!request_info.is_remote)
		/* server is local (e.g. FastCGI); we don't need an offset */
		return std::chrono::system_clock::duration::zero();

	const auto server_date = GetServerDate(response_headers);
	if (server_date == std::chrono::system_clock::from_time_t(-1))
		return std::chrono::system_clock::duration::min();

	return now - server_date;
}

std::optional<HttpCacheResponseInfo>
http_cache_response_evaluate(const HttpCacheRequestInfo &request_info,
			     AllocatorPtr alloc,
			     bool eager_cache,
			     http_status_t status, const StringMap &headers,
			     off_t body_available) noexcept
{
	if (!http_status_cacheable(status))
		return std::nullopt;

	if (body_available != (off_t)-1 && body_available > cacheable_size_limit)
		/* too large for the cache */
		return std::nullopt;

	HttpCacheResponseInfo info;
	info.expires = std::chrono::system_clock::from_time_t(-1);
	if (const char *cache_control = headers.Get("cache-control")) {
		for (std::string_view s : IterableSplitString(cache_control, ',')) {
			s = Strip(s);

			if (s.starts_with("private"sv) ||
			    s == "no-cache"sv || s == "no-store"sv)
				return std::nullopt;

			if (SkipPrefix(s, "max-age="sv)) {
				/* RFC 2616 14.9.3 */
				char value[16];
				int seconds;

				if (s.size() >= sizeof(value))
					continue;

				*std::copy(s.begin(), s.end(), value) = 0;

				seconds = atoi(value);
				if (seconds > 0)
					info.expires = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
			}
		}
	}

	const auto now = std::chrono::system_clock::now();

	const auto offset = GetServerDateOffset(request_info, now, headers);
	if (offset == std::chrono::system_clock::duration::min())
		/* we cannot determine whether to cache a resource if the
		   server does not provide its system time */
		return std::nullopt;

	if (info.expires == std::chrono::system_clock::from_time_t(-1)) {
		/* RFC 2616 14.9.3: "If a response includes both an Expires
		   header and a max-age directive, the max-age directive
		   overrides the Expires header" */

		info.expires = parse_translate_time(headers.Get("expires"), offset);
		if (info.expires != std::chrono::system_clock::from_time_t(-1) &&
		    info.expires < now)
			LogConcat(4, "HttpCache", "invalid 'expires' header");
	}

	if (request_info.has_query_string &&
	    !eager_cache &&
	    info.expires == std::chrono::system_clock::from_time_t(-1))
		/* RFC 2616 13.9: "since some applications have traditionally
		   used GETs and HEADs with query URLs (those containing a "?"
		   in the rel_path part) to perform operations with
		   significant side effects, caches MUST NOT treat responses
		   to such URIs as fresh unless the server provides an
		   explicit expiration time" */
		return std::nullopt;

	info.last_modified = headers.Get("last-modified");
	info.etag = headers.Get("etag");

	info.vary = nullptr;
	const auto vary = headers.EqualRange("vary");
	for (auto i = vary.first; i != vary.second; ++i) {
		const char *value = i->value;
		if (*value == 0)
			continue;

		if (strcmp(value, "*") == 0)
			/* RFC 2616 13.6: A Vary header field-value of
			   "*" always fails to match and subsequent
			   requests on that resource can only be
			   properly interpreted by the origin
			   server. */
			return std::nullopt;

		if (info.vary == nullptr)
			info.vary = value;
		else
			info.vary = alloc.Concat(info.vary, ", ", value);
	}

	if (info.expires == std::chrono::system_clock::from_time_t(-1) &&
	    info.last_modified == nullptr &&
	    info.etag == nullptr) {
		if (eager_cache)
			// TODO
			info.expires = std::chrono::system_clock::now() + std::chrono::hours(1);
		else
			return std::nullopt;
	}

	return info;
}

void
http_cache_copy_vary(StringMap &dest, AllocatorPtr alloc, const char *vary,
		     const StringMap &request_headers) noexcept
{
	for (const char *const*list = http_list_split(alloc, vary);
	     *list != nullptr; ++list) {
		const char *name = *list;
		const char *value = request_headers.Get(name);
		if (value == nullptr)
			value = "";
		else
			value = alloc.Dup(value);
		dest.Set(alloc, name, value);
	}
}

bool
http_cache_prefer_cached(const HttpCacheDocument &document,
			 const StringMap &response_headers) noexcept
{
	if (document.info.etag == nullptr)
		return false;

	const char *etag = response_headers.Get("etag");

	/* if the ETags are the same, then the resource hasn't changed,
	   but the server was too lazy to check that properly */
	return etag != nullptr && strcmp(etag, document.info.etag) == 0;
}
