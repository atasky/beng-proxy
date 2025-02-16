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

#include "ResourceAddress.hxx"
#include "file/Address.hxx"
#include "http/local/Address.hxx"
#include "http/Address.hxx"
#include "cgi/Address.hxx"
#include "nfs/Address.hxx"
#include "uri/Extract.hxx"
#include "uri/Verify.hxx"
#include "uri/Base.hxx"
#include "AllocatorPtr.hxx"
#include "HttpMessageResponse.hxx"
#include "util/StringView.hxx"

ResourceAddress::ResourceAddress(AllocatorPtr alloc,
				 const ResourceAddress &src) noexcept
{
	CopyFrom(alloc, src);
}

void
ResourceAddress::CopyFrom(AllocatorPtr alloc,
			  const ResourceAddress &src) noexcept
{
	type = src.type;

	switch (src.type) {
	case Type::NONE:
		break;

	case Type::LOCAL:
		assert(src.u.file != nullptr);
		u.file = alloc.New<FileAddress>(alloc, *src.u.file);
		break;

	case Type::HTTP:
		assert(src.u.http != nullptr);
		u.http = alloc.New<HttpAddress>(alloc, *src.u.http);
		break;

	case Type::LHTTP:
		assert(src.u.lhttp != nullptr);
		u.lhttp = src.u.lhttp->Dup(alloc);
		break;

	case Type::PIPE:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		u.cgi = src.u.cgi->Clone(alloc);
		break;

	case Type::NFS:
		u.nfs = alloc.New<NfsAddress>(alloc, *src.u.nfs);
		break;
	}
}

ResourceAddress *
ResourceAddress::Dup(AllocatorPtr alloc) const noexcept
{
	return alloc.New<ResourceAddress>(alloc, *this);
}

ResourceAddress
ResourceAddress::WithPath(AllocatorPtr alloc, const char *path) const noexcept
{
	switch (type) {
	case Type::NONE:
	case Type::LOCAL:
	case Type::PIPE:
	case Type::NFS:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		break;

	case Type::HTTP:
		return *alloc.New<HttpAddress>(ShallowCopy(), GetHttp(), path);

	case Type::LHTTP:
		return *alloc.New<LhttpAddress>(ShallowCopy(), GetLhttp(), path);
	}

	assert(false);
	gcc_unreachable();
}

ResourceAddress
ResourceAddress::WithQueryStringFrom(AllocatorPtr alloc,
				     const char *uri) const noexcept
{
	const char *query_string;

	switch (type) {
		CgiAddress *cgi;

	case Type::NONE:
	case Type::LOCAL:
	case Type::PIPE:
	case Type::NFS:
		/* no query string support */
		return {ShallowCopy(), *this};

	case Type::HTTP:
		assert(u.http != nullptr);

		query_string = UriQuery(uri);
		if (query_string == nullptr)
			/* no query string in URI */
			return {ShallowCopy(), *this};

		return *u.http->InsertQueryString(alloc, query_string);

	case Type::LHTTP:
		assert(u.lhttp != nullptr);

		query_string = UriQuery(uri);
		if (query_string == nullptr)
			/* no query string in URI */
			return {ShallowCopy(), *this};

		return *u.lhttp->InsertQueryString(alloc, query_string);

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		assert(u.cgi->path != nullptr);

		query_string = UriQuery(uri);
		if (query_string == nullptr)
			/* no query string in URI */
			return {ShallowCopy(), *this};

		cgi = alloc.New<CgiAddress>(ShallowCopy(), GetCgi());
		cgi->InsertQueryString(alloc, query_string);
		return ResourceAddress(type, *cgi);
	}

	assert(false);
	gcc_unreachable();
}

ResourceAddress
ResourceAddress::WithArgs(AllocatorPtr alloc,
			  std::string_view args, std::string_view path) const noexcept
{
	switch (type) {
		CgiAddress *cgi;

	case Type::NONE:
	case Type::LOCAL:
	case Type::PIPE:
	case Type::NFS:
		/* no arguments support */
		return {ShallowCopy(), *this};

	case Type::HTTP:
		assert(u.http != nullptr);

		return *GetHttp().InsertArgs(alloc, args, path);

	case Type::LHTTP:
		assert(u.lhttp != nullptr);

		return *GetLhttp().InsertArgs(alloc, args, path);

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		assert(u.cgi->path != nullptr);

		if (u.cgi->uri == nullptr && u.cgi->path_info == nullptr)
			return {ShallowCopy(), *this};

		cgi = alloc.New<CgiAddress>(ShallowCopy(), GetCgi());
		cgi->InsertArgs(alloc, args, path);
		return ResourceAddress(type, *cgi);
	}

	assert(false);
	gcc_unreachable();
}

const char *
ResourceAddress::AutoBase(AllocatorPtr alloc, const char *uri) const noexcept
{
	assert(uri != nullptr);

	switch (type) {
	case Type::NONE:
	case Type::LOCAL:
	case Type::PIPE:
	case Type::HTTP:
	case Type::LHTTP:
	case Type::NFS:
		return nullptr;

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		return u.cgi->AutoBase(alloc, uri);
	}

	assert(false);
	gcc_unreachable();
}

ResourceAddress
ResourceAddress::SaveBase(AllocatorPtr alloc,
			  std::string_view suffix) const noexcept
{
	switch (type) {
	case Type::NONE:
	case Type::PIPE:
		return nullptr;

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		{
			auto *cgi = GetCgi().SaveBase(alloc, suffix);
			if (cgi == nullptr)
				return nullptr;

			return ResourceAddress(type, *cgi);
		}

	case Type::LOCAL:
		{
			auto *file = GetFile().SaveBase(alloc, suffix);
			if (file == nullptr)
				return nullptr;

			return *file;
		}

	case Type::HTTP:
		{
			auto *http = GetHttp().SaveBase(alloc, suffix);
			if (http == nullptr)
				return nullptr;

			return *http;
		}

	case Type::LHTTP:
		{
			auto *lhttp = GetLhttp().SaveBase(alloc, suffix);
			if (lhttp == nullptr)
				return nullptr;

			return *lhttp;
		}

	case Type::NFS:
		{
			auto *nfs = GetNfs().SaveBase(alloc, suffix);
			if (nfs == nullptr)
				return nullptr;

			return *nfs;
		}
	}

	assert(false);
	gcc_unreachable();
}

void
ResourceAddress::CacheStore(AllocatorPtr alloc,
			    const ResourceAddress &src,
			    const char *uri, const char *base,
			    bool easy_base, bool expandable)
{
	if (base == nullptr) {
		CopyFrom(alloc, src);
		return;
	} else if (const char *tail = base_tail(uri, base)) {
		/* we received a valid BASE packet - store only the base
		   URI */

		if (easy_base || expandable) {
			/* when the response is expandable, skip appending the
			   tail URI, don't call SaveBase() */
			CopyFrom(alloc, src);
			return;
		}

		if (src.type == Type::NONE) {
			/* _save_base() will fail on a "NONE" address, but in this
			   case, the operation is useful and is allowed as a
			   special case */
			type = Type::NONE;
			return;
		}

		*this = src.SaveBase(alloc, tail);
		if (IsDefined())
			return;

		/* the tail could not be applied to the address, so this is a
		   base mismatch */
	}

	throw HttpMessageResponse(HTTP_STATUS_BAD_GATEWAY, "Base mismatch");
}

ResourceAddress
ResourceAddress::LoadBase(AllocatorPtr alloc,
			  std::string_view suffix) const noexcept
{
	switch (type) {
	case Type::NONE:
	case Type::PIPE:
		assert(false);
		gcc_unreachable();

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		{
			auto *cgi = GetCgi().LoadBase(alloc, suffix);
			if (cgi == nullptr)
				return nullptr;

			return ResourceAddress(type, *cgi);
		}

	case Type::LOCAL:
		{
			auto *file = GetFile().LoadBase(alloc, suffix);
			if (file == nullptr)
				return nullptr;

			return *file;
		}

	case Type::HTTP:
		{
			auto *http = GetHttp().LoadBase(alloc, suffix);
			if (http == nullptr)
				return nullptr;

			return *http;
		}

	case Type::LHTTP:
		{
			auto *lhttp = GetLhttp().LoadBase(alloc, suffix);
			if (lhttp == nullptr)
				return nullptr;

			return *lhttp;
		}

	case Type::NFS:
		{
			auto *nfs = GetNfs().LoadBase(alloc, suffix);
			if (nfs == nullptr)
				return nullptr;

			return *nfs;
		}
	}

	assert(false);
	gcc_unreachable();
}

void
ResourceAddress::CacheLoad(AllocatorPtr alloc, const ResourceAddress &src,
			   const char *uri, const char *base,
			   bool unsafe_base, bool expandable)
{
	if (base != nullptr && !expandable) {
		const char *tail = require_base_tail(uri, base);

		if (!unsafe_base && !uri_path_verify_paranoid(tail - 1))
			throw HttpMessageResponse(HTTP_STATUS_BAD_REQUEST, "Malformed URI");

		if (src.type == Type::NONE) {
			/* see code comment in tcache_store_address() */
			type = Type::NONE;
			return;
		}

		*this = src.LoadBase(alloc, tail);
		if (IsDefined())
			return;
	}

	CopyFrom(alloc, src);
}

ResourceAddress
ResourceAddress::Apply(AllocatorPtr alloc,
		       std::string_view relative) const noexcept
{
	const HttpAddress *uwa;
	const CgiAddress *cgi;
	const LhttpAddress *lhttp;

	switch (type) {
	case Type::NONE:
		return nullptr;

	case Type::LOCAL:
	case Type::PIPE:
	case Type::NFS:
		return {ShallowCopy(), *this};

	case Type::HTTP:
		uwa = u.http->Apply(alloc, relative);
		if (uwa == nullptr)
			return nullptr;

		return *uwa;

	case Type::LHTTP:
		lhttp = u.lhttp->Apply(alloc, relative);
		if (lhttp == nullptr)
			return nullptr;

		return *lhttp;

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		cgi = u.cgi->Apply(alloc, relative);
		if (cgi == nullptr)
			return nullptr;

		return ResourceAddress(type, *cgi);
	}

	assert(false);
	gcc_unreachable();
}

std::string_view
ResourceAddress::RelativeTo(const ResourceAddress &base) const noexcept
{
	assert(base.type == type);

	switch (type) {
	case Type::NONE:
	case Type::LOCAL:
	case Type::PIPE:
	case Type::NFS:
		return {};

	case Type::HTTP:
		return u.http->RelativeTo(*base.u.http);

	case Type::LHTTP:
		return u.lhttp->RelativeTo(*base.u.lhttp);

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		return u.cgi->RelativeTo(*base.u.cgi);
	}

	assert(false);
	gcc_unreachable();
}

std::string_view
ResourceAddress::RelativeToApplied(AllocatorPtr alloc,
				   const ResourceAddress &apply_base,
				   std::string_view relative) const
{
	assert(apply_base.type == type);

	switch (type) {
	case Type::NONE:
	case Type::LOCAL:
	case Type::PIPE:
	case Type::NFS:
	case Type::HTTP:
		break;

	case Type::LHTTP:
		return u.lhttp->RelativeToApplied(alloc, *apply_base.u.lhttp,
						  relative);

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		return u.cgi->RelativeToApplied(alloc, *apply_base.u.cgi,
						relative);
	}

	auto applied = apply_base.Apply(alloc, relative);
	return applied.IsDefined()
		? applied.RelativeTo(*this)
		: std::string_view{};
}

const char *
ResourceAddress::GetId(AllocatorPtr alloc) const noexcept
{
	switch (type) {
	case Type::NONE:
		return "";

	case Type::LOCAL:
		return alloc.Dup(u.file->path);

	case Type::HTTP:
		return u.http->GetAbsoluteURI(alloc);

	case Type::LHTTP:
		return u.lhttp->GetId(alloc);

	case Type::PIPE:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		return u.cgi->GetId(alloc);

	case Type::NFS:
		return u.nfs->GetId(alloc);
	}

	assert(false);
	gcc_unreachable();
}

const char *
ResourceAddress::GetHostAndPort() const noexcept
{
	switch (type) {
	case Type::NONE:
	case Type::LOCAL:
	case Type::PIPE:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
	case Type::NFS:
		return nullptr;

	case Type::HTTP:
		return u.http->host_and_port;

	case Type::LHTTP:
		return u.lhttp->host_and_port;
	}

	assert(false);
	gcc_unreachable();
}

const char *
ResourceAddress::GetUriPath() const noexcept
{
	switch (type) {
	case Type::NONE:
	case Type::LOCAL:
	case Type::PIPE:
	case Type::NFS:
		return nullptr;

	case Type::HTTP:
		return u.http->path;

	case Type::LHTTP:
		return u.lhttp->uri;

	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		if (u.cgi->uri != nullptr)
			return u.cgi->uri;

		return u.cgi->script_name;
	}

	assert(false);
	gcc_unreachable();
}

void
ResourceAddress::Check() const
{
	switch (type) {
	case Type::NONE:
		break;

	case Type::HTTP:
		u.http->Check();
		break;

	case Type::LOCAL:
		u.file->Check();
		break;

	case Type::LHTTP:
		u.lhttp->Check();
		break;

	case Type::PIPE:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		u.cgi->Check(type == Type::WAS);
		break;

	case Type::NFS:
		u.nfs->Check();
		break;
	}
}

bool
ResourceAddress::IsValidBase() const noexcept
{
	switch (type) {
	case Type::NONE:
		return true;

	case Type::LOCAL:
		return u.file->IsValidBase();

	case Type::HTTP:
		return u.http->IsValidBase();

	case Type::LHTTP:
		return u.lhttp->IsValidBase();

	case Type::PIPE:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		return u.cgi->IsValidBase();

	case Type::NFS:
		return u.nfs->IsValidBase();
	}

	assert(false);
	gcc_unreachable();
}

bool
ResourceAddress::HasQueryString() const noexcept
{
	switch (type) {
	case Type::NONE:
		return false;

	case Type::LOCAL:
		return u.file->HasQueryString();

	case Type::HTTP:
		return u.http->HasQueryString();

	case Type::LHTTP:
		return u.lhttp->HasQueryString();

	case Type::PIPE:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		return u.cgi->HasQueryString();

	case Type::NFS:
		return u.nfs->HasQueryString();
	}

	/* unreachable */
	assert(false);
	return false;
}

bool
ResourceAddress::IsExpandable() const noexcept
{
	switch (type) {
	case Type::NONE:
		return false;

	case Type::LOCAL:
		return u.file->IsExpandable();

	case Type::PIPE:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		return u.cgi->IsExpandable();

	case Type::HTTP:
		return u.http->IsExpandable();

	case Type::LHTTP:
		return u.lhttp->IsExpandable();

	case Type::NFS:
		return u.nfs->IsExpandable();
	}

	assert(false);
	gcc_unreachable();
}

void
ResourceAddress::Expand(AllocatorPtr alloc, const MatchData &match_data)
{
	switch (type) {
		FileAddress *file;
		CgiAddress *cgi;
		HttpAddress *uwa;
		LhttpAddress *lhttp;

	case Type::NONE:
		break;

	case Type::LOCAL:
		u.file = file = alloc.New<FileAddress>(alloc, *u.file);
		file->Expand(alloc, match_data);
		break;

	case Type::PIPE:
	case Type::CGI:
	case Type::FASTCGI:
	case Type::WAS:
		u.cgi = cgi = u.cgi->Clone(alloc);
		cgi->Expand(alloc, match_data);
		break;

	case Type::HTTP:
		/* copy the http_address object (it's a pointer, not
		   in-line) and expand it */
		u.http = uwa = alloc.New<HttpAddress>(alloc, *u.http);
		uwa->Expand(alloc, match_data);
		break;

	case Type::LHTTP:
		/* copy the lhttp_address object (it's a pointer, not
		   in-line) and expand it */
		u.lhttp = lhttp = u.lhttp->Dup(alloc);
		lhttp->Expand(alloc, match_data);
		break;

	case Type::NFS:
		/* copy the nfs_address object (it's a pointer, not
		   in-line) and expand it */
		u.nfs = u.nfs->Expand(alloc, match_data);
		break;
	}
}
