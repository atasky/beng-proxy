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

#include "Address.hxx"
#include "uri/Base.hxx"
#include "uri/PEdit.hxx"
#include "uri/PRelative.hxx"
#include "uri/Relative.hxx"
#include "uri/Verify.hxx"
#include "uri/Extract.hxx"
#include "AllocatorPtr.hxx"
#include "pexpand.hxx"
#include "util/StringCompare.hxx"
#include "util/StringView.hxx"

#include <stdexcept>

#include <string.h>

HttpAddress::HttpAddress(bool _ssl,
			 const char *_host_and_port, const char *_path)
	:ssl(_ssl),
	 host_and_port(_host_and_port),
	 path(_path)
{
}

HttpAddress::HttpAddress(ShallowCopy shallow_copy,
			 bool _ssl,
			 const char *_host_and_port, const char *_path,
			 const AddressList &_addresses)
	:ssl(_ssl),
	 host_and_port(_host_and_port),
	 path(_path),
	 addresses(shallow_copy, _addresses)
{
}

HttpAddress::HttpAddress(AllocatorPtr alloc, const HttpAddress &src)
	:ssl(src.ssl), http2(src.http2),
	 expand_path(src.expand_path),
	 certificate(alloc.CheckDup(src.certificate)),
	 host_and_port(alloc.CheckDup(src.host_and_port)),
	 path(alloc.Dup(src.path)),
	 addresses(alloc, src.addresses)
{
}

HttpAddress::HttpAddress(AllocatorPtr alloc, const HttpAddress &src,
			 const char *_path)
	:ssl(src.ssl), http2(src.http2),
	 certificate(alloc.CheckDup(src.certificate)),
	 host_and_port(alloc.CheckDup(src.host_and_port)),
	 path(alloc.Dup(_path)),
	 addresses(alloc, src.addresses)
{
}

static HttpAddress *
http_address_new(AllocatorPtr alloc, bool ssl,
		 const char *host_and_port, const char *path)
{
	assert(path != nullptr);

	return alloc.New<HttpAddress>(ssl, host_and_port, path);
}

/**
 * Utility function used by http_address_parse().
 *
 * Throws std::runtime_error on error.
 */
static HttpAddress *
http_address_parse2(AllocatorPtr alloc, bool ssl,
		    const char *uri)
{
	assert(uri != nullptr);

	const char *path = strchr(uri, '/');
	const char *host_and_port;
	if (path != nullptr) {
		if (path == uri || !uri_path_verify_quick(path))
			throw std::runtime_error("malformed HTTP URI");

		host_and_port = alloc.DupZ({uri, path});
		path = alloc.Dup(path);
	} else {
		host_and_port = alloc.Dup(uri);
		path = "/";
	}

	return http_address_new(alloc, ssl, host_and_port, path);
}

HttpAddress *
http_address_parse(AllocatorPtr alloc, const char *uri)
{
	if (auto http = StringAfterPrefix(uri, "http://"))
		return http_address_parse2(alloc, false, http);
	else if (auto https = StringAfterPrefix(uri, "https://"))
		return http_address_parse2(alloc, true, https);
	else if (auto unix = StringAfterPrefix(uri, "unix:/"))
		return http_address_new(alloc, false, nullptr,
					/* rewind to the slash */
					unix - 1);

	throw std::runtime_error("unrecognized URI");
}

HttpAddress *
http_address_with_path(AllocatorPtr alloc, const HttpAddress *uwa,
		       const char *path)
{
	auto *p = alloc.New<HttpAddress>(ShallowCopy(), *uwa);
	p->path = path;
	return p;
}

HttpAddress *
http_address_dup_with_path(AllocatorPtr alloc,
			   const HttpAddress *uwa,
			   const char *path)
{
	assert(uwa != nullptr);

	return alloc.New<HttpAddress>(alloc, *uwa, path);
}

void
HttpAddress::Check() const
{
	if (addresses.empty())
		throw std::runtime_error("no ADDRESS for HTTP address");
}

static constexpr const char *
uri_protocol_prefix(bool has_host) noexcept
{
	return has_host ? "http://" : "unix:";
}

char *
HttpAddress::GetAbsoluteURI(AllocatorPtr alloc,
			    const char *override_path) const noexcept
{
	assert(host_and_port != nullptr);
	assert(override_path != nullptr);
	assert(*override_path == '/');

	return alloc.Concat(uri_protocol_prefix(host_and_port != nullptr),
			    host_and_port == nullptr ? "" : host_and_port,
			    override_path);
}

char *
HttpAddress::GetAbsoluteURI(AllocatorPtr alloc) const noexcept
{
	return GetAbsoluteURI(alloc, path);
}

bool
HttpAddress::HasQueryString() const
{
	return strchr(path, '?') != nullptr;
}

HttpAddress *
HttpAddress::InsertQueryString(AllocatorPtr alloc,
			       const char *query_string) const
{
	return http_address_with_path(alloc, this,
				      uri_insert_query_string(alloc, path,
							      query_string));
}

HttpAddress *
HttpAddress::InsertArgs(AllocatorPtr alloc,
			StringView args, StringView path_info) const
{
	return http_address_with_path(alloc, this,
				      uri_insert_args(alloc, path,
						      args, path_info));
}

bool
HttpAddress::IsValidBase() const
{
	return IsExpandable() || is_base(path);
}

HttpAddress *
HttpAddress::SaveBase(AllocatorPtr alloc,
		      std::string_view suffix) const noexcept
{
	size_t length = base_string(path, suffix);
	if (length == (size_t)-1)
		return nullptr;

	return http_address_dup_with_path(alloc, this,
					  alloc.DupZ({path, length}));
}

HttpAddress *
HttpAddress::LoadBase(AllocatorPtr alloc,
		      std::string_view suffix) const noexcept
{
	assert(path != nullptr);
	assert(*path != 0);
	assert(expand_path || path[strlen(path) - 1] == '/');

	return http_address_dup_with_path(alloc, this,
					  alloc.Concat(path, suffix));
}

const HttpAddress *
HttpAddress::Apply(AllocatorPtr alloc,
		   std::string_view relative) const noexcept
{
	if (relative.empty())
		return this;

	if (UriHasScheme(relative)) {
		HttpAddress *other;
		try {
			other = http_address_parse(alloc, alloc.DupZ(relative));
		} catch (const std::runtime_error &e) {
			return nullptr;
		}

		const char *my_host = host_and_port != nullptr ? host_and_port : "";
		const char *other_host = other->host_and_port != nullptr
			? other->host_and_port
			: "";

		if (strcmp(my_host, other_host) != 0)
			/* if it points to a different host, we cannot apply the
			   address list, and so this function must fail */
			return nullptr;

		other->addresses = AddressList(ShallowCopy(), addresses);
		return other;
	}

	const char *p = uri_absolute(alloc, path, relative);
	assert(p != nullptr);

	return http_address_with_path(alloc, this, p);
}

StringView
HttpAddress::RelativeTo(const HttpAddress &base) const
{
	const char *my_host = host_and_port != nullptr ? host_and_port : "";
	const char *base_host = base.host_and_port != nullptr
		? base.host_and_port
		: "";

	if (strcmp(my_host, base_host) != 0)
		return nullptr;

	return uri_relative(base.path, path);
}

void
HttpAddress::Expand(AllocatorPtr alloc, const MatchData &match_data)
{
	if (expand_path) {
		expand_path = false;
		path = expand_string(alloc, path, match_data);
	}
}
