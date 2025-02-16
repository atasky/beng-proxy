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
#include "pool/pool.hxx"
#include "pool/tpool.hxx"
#include "pool/StringBuilder.hxx"
#include "AllocatorPtr.hxx"
#include "uri/Base.hxx"
#include "uri/Unescape.hxx"
#include "uri/Extract.hxx"
#include "uri/Compare.hxx"
#include "uri/Relative.hxx"
#include "uri/PEdit.hxx"
#include "uri/PEscape.hxx"
#include "uri/PRelative.hxx"
#include "util/StringView.hxx"
#include "pexpand.hxx"

#include <stdexcept>

#include <string.h>

CgiAddress::CgiAddress(AllocatorPtr alloc, const CgiAddress &src) noexcept
	:path(alloc.Dup(src.path)),
	 args(alloc, src.args),
	 params(alloc, src.params),
	 options(alloc, src.options),
	 interpreter(alloc.CheckDup(src.interpreter)),
	 action(alloc.CheckDup(src.action)),
	 uri(alloc.CheckDup(src.uri)),
	 script_name(alloc.CheckDup(src.script_name)),
	 path_info(alloc.CheckDup(src.path_info)),
	 query_string(alloc.CheckDup(src.query_string)),
	 document_root(alloc.CheckDup(src.document_root)),
	 address_list(alloc, src.address_list),
	 parallelism(src.parallelism),
	 concurrency(src.concurrency),
	 request_uri_verbatim(src.request_uri_verbatim),
	 expand_path(src.expand_path),
	 expand_uri(src.expand_uri),
	 expand_script_name(src.expand_script_name),
	 expand_path_info(src.expand_path_info),
	 expand_document_root(src.expand_document_root)
{
}

[[gnu::pure]]
static bool
HasTrailingSlash(const char *p) noexcept
{
	size_t length = strlen(p);
	return length > 0 && p[length - 1] == '/';
}

const char *
CgiAddress::GetURI(AllocatorPtr alloc) const noexcept
{
	if (uri != nullptr)
		return uri;

	const char *sn = script_name;
	if (sn == nullptr)
		sn = "/";

	std::string_view pi = GetPathInfo();
	const char *qm = "";
	const char *qs = query_string;

	if (pi.empty()) {
		if (qs == nullptr)
			return sn;
	}

	if (qs != nullptr)
		qm = "?";
	else
		qs = "";

	if (pi.starts_with('/') && HasTrailingSlash(sn))
		/* avoid generating a double slash when concatenating
		   script_name and path_info */
		pi.remove_prefix(1);

	return alloc.Concat(sn, pi, qm, qs);
}

const char *
CgiAddress::GetId(AllocatorPtr alloc) const noexcept
{
	PoolStringBuilder<256> b;
	b.push_back(path);

	char child_options_buffer[16384];
	b.emplace_back(child_options_buffer,
		       options.MakeId(child_options_buffer));

	if (document_root != nullptr) {
		b.push_back(";d=");
		b.push_back(document_root);
	}

	if (interpreter != nullptr) {
		b.push_back(";i=");
		b.push_back(interpreter);
	}

	if (action != nullptr) {
		b.push_back(";a=");
		b.push_back(action);
	}

	for (auto i : args) {
		b.push_back("!");
		b.push_back(i);
	}

	for (auto i : params) {
		b.push_back("!");
		b.push_back(i);
	}

	if (uri != nullptr) {
		b.push_back(";u=");
		b.push_back(uri);
	} else if (script_name != nullptr) {
		b.push_back(";s=");
		b.push_back(script_name);
	}

	if (path_info != nullptr) {
		b.push_back(";p=");
		b.push_back(path_info);
	}

	if (query_string != nullptr) {
		b.push_back("?");
		b.push_back(query_string);
	}

	return b(alloc);
}

void
CgiAddress::Check(bool is_was) const
{
	if (is_was) {
		if (!address_list.empty()) {
			if (concurrency == 0)
				throw std::runtime_error("Missing concurrency for Remote-WAS");

			if (!address_list.IsSingle())
				throw std::runtime_error("Too many Remote-WAS addresses");

			if (address_list.front().GetFamily() != AF_LOCAL)
				throw std::runtime_error("Remote-WAS requires AF_LOCAL");
		}
	}

	options.Check();
}

CgiAddress *
CgiAddress::Clone(AllocatorPtr alloc) const noexcept
{
	return alloc.New<CgiAddress>(alloc, *this);
}

bool
CgiAddress::IsSameProgram(const CgiAddress &other) const noexcept
{
	// TODO: check args, params, options?
	return strcmp(path, other.path) == 0;
}

bool
CgiAddress::IsSameBase(const CgiAddress &other) const noexcept
{
	return IsSameProgram(other) &&
		StringView(script_name).Equals(other.script_name);
}

void
CgiAddress::InsertQueryString(AllocatorPtr alloc,
			      const char *new_query_string) noexcept
{
	if (query_string != nullptr)
		query_string = alloc.Concat(new_query_string, "&", query_string);
	else
		query_string = alloc.Dup(new_query_string);
}

void
CgiAddress::InsertArgs(AllocatorPtr alloc, StringView new_args,
		       StringView new_path_info) noexcept
{
	uri = uri_insert_args(alloc, uri, new_args, new_path_info);

	if (path_info != nullptr)
		path_info = alloc.Concat(path_info,
					 ';', new_args,
					 new_path_info);
}

bool
CgiAddress::IsValidBase() const noexcept
{
	if (IsExpandable())
		return true;

	const auto pi = GetPathInfo();
	if (pi.empty())
		return script_name != nullptr && is_base(script_name);
	else
		return is_base(pi);
}

const char *
CgiAddress::AutoBase(AllocatorPtr alloc,
		     const char *request_uri) const noexcept
{
	auto pi = GetPathInfo();

	/* XXX implement (un-)escaping of the uri */

	/* either SCRIPT_NAME must end with a slash or PATH_INFO must
	   start with one */
	if (script_name == nullptr || !is_base(script_name)) {
		if (!pi.starts_with('/'))
			return nullptr;

		pi.remove_prefix(1);
	}

	size_t length = base_string(request_uri, pi);
	if (length == 0 || length == (size_t)-1)
		return nullptr;

	return alloc.DupZ({request_uri, length});
}

CgiAddress *
CgiAddress::SaveBase(AllocatorPtr alloc, std::string_view suffix) const noexcept
{
	size_t uri_length = 0;
	if (uri != nullptr) {
		const char *end = UriFindUnescapedSuffix(uri, suffix);
		if (end == nullptr)
			return nullptr;

		uri_length = end - uri;
	}

	std::string_view new_path_info = GetPathInfo();
	const char *new_path_info_end =
		UriFindUnescapedSuffix(new_path_info, suffix);
	if (new_path_info_end == nullptr)
		return nullptr;

	CgiAddress *dest = Clone(alloc);
	if (dest->uri != nullptr)
		dest->uri = alloc.DupZ({dest->uri, uri_length});
	dest->path_info = alloc.DupZ({new_path_info.data(), new_path_info_end});
	return dest;
}

CgiAddress *
CgiAddress::LoadBase(AllocatorPtr alloc, std::string_view suffix) const noexcept
{
	const TempPoolLease tpool;

	char *unescaped = uri_unescape_dup(*tpool, suffix);
	if (unescaped == nullptr)
		return nullptr;

	CgiAddress *dest = Clone(alloc);
	if (dest->uri != nullptr)
		dest->uri = alloc.Concat(dest->uri, unescaped);

	dest->path_info = alloc.Concat(GetPathInfo(), unescaped);
	return dest;
}

[[gnu::pure]]
static const char *
UnescapeApplyPathInfo(AllocatorPtr alloc, const char *base_path_info,
		      StringView relative_escaped) noexcept
{
	if (base_path_info == nullptr)
		base_path_info = "";

	if (relative_escaped.empty())
		return base_path_info;

	if (UriHasAuthority(relative_escaped))
		return nullptr;

	const TempPoolLease tpool;

	char *unescaped = (char *)p_malloc(tpool, relative_escaped.size);
	char *unescaped_end = UriUnescape(unescaped, relative_escaped);
	if (unescaped_end == nullptr)
		return nullptr;

	size_t unescaped_length = unescaped_end - unescaped;

	return uri_absolute(alloc, base_path_info,
			    {unescaped, unescaped_length});
}

const CgiAddress *
CgiAddress::Apply(AllocatorPtr alloc,
		  std::string_view relative) const noexcept
{
	if (relative.empty())
		return this;

	const char *new_path_info = UnescapeApplyPathInfo(alloc, path_info,
							  relative);
	if (new_path_info == nullptr)
		return nullptr;

	auto *dest = alloc.New<CgiAddress>(ShallowCopy(), *this);
	dest->path_info = new_path_info;
	return dest;
}

StringView
CgiAddress::RelativeTo(const CgiAddress &base) const noexcept
{
	if (!IsSameProgram(base))
		return nullptr;

	if (path_info == nullptr || base.path_info == nullptr)
		return nullptr;

	return uri_relative(base.path_info, path_info);
}

StringView
CgiAddress::RelativeToApplied(AllocatorPtr alloc,
			      const CgiAddress &apply_base,
			      StringView relative) const noexcept
{
	if (!IsSameProgram(apply_base))
		return nullptr;

	if (path_info == nullptr)
		return nullptr;

	const char *new_path_info =
		UnescapeApplyPathInfo(alloc, apply_base.path_info, relative);
	if (new_path_info == nullptr)
		return nullptr;

	return uri_relative(path_info, new_path_info);
}

void
CgiAddress::Expand(AllocatorPtr alloc, const MatchData &match_data)
{
	options.Expand(alloc, match_data);

	if (expand_path) {
		expand_path = false;
		path = expand_string_unescaped(alloc, path, match_data);
	}

	if (expand_uri) {
		expand_uri = false;
		uri = expand_string_unescaped(alloc, uri, match_data);
	}

	if (expand_script_name) {
		expand_script_name = false;
		script_name = expand_string_unescaped(alloc, script_name, match_data);
	}

	if (expand_path_info) {
		expand_path_info = false;
		path_info = expand_string_unescaped(alloc, path_info, match_data);
	}

	if (expand_document_root) {
		expand_document_root = false;
		document_root = expand_string_unescaped(alloc, document_root,
							match_data);
	}

	args.Expand(alloc, match_data);
	params.Expand(alloc, match_data);
}
