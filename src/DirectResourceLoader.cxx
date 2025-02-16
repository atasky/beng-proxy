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

#include "DirectResourceLoader.hxx"
#include "ResourceAddress.hxx"
#include "http/XForwardedFor.hxx"
#include "http/ResponseHandler.hxx"
#include "file/Address.hxx"
#include "file/Request.hxx"
#include "http/local/Glue.hxx"
#include "http/Address.hxx"
#include "cgi/Glue.hxx"
#include "cgi/Address.hxx"
#include "fcgi/Request.hxx"
#include "fcgi/Remote.hxx"
#include "was/Glue.hxx"
#include "was/MGlue.hxx"
#include "nfs/Address.hxx"
#include "nfs/Glue.hxx"
#include "pipe_filter.hxx"
#include "delegate/Address.hxx"
#include "delegate/HttpRequest.hxx"
#include "strmap.hxx"
#include "istream/UnusedPtr.hxx"
#include "pool/pool.hxx"
#include "AllocatorPtr.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/StringStrip.hxx"

#include <string.h>

[[gnu::pure]]
static const char *
GetRemoteHost(const XForwardedForConfig &config, AllocatorPtr alloc,
	      const StringMap &headers) noexcept
{
	const char *xff = headers.Get("x-forwarded-for");
	if (xff == nullptr)
		return nullptr;

	const auto remote_host = config.GetRealRemoteHost(xff);
	if (remote_host.empty())
		return nullptr;

	return alloc.DupZ(remote_host);
}

void
DirectResourceLoader::SendRequest(struct pool &pool,
				  const StopwatchPtr &parent_stopwatch,
				  const ResourceRequestParams &params,
				  http_method_t method,
				  const ResourceAddress &address,
				  http_status_t status, StringMap &&headers,
				  UnusedIstreamPtr body,
				  gcc_unused const char *body_etag,
				  HttpResponseHandler &handler,
				  CancellablePointer &cancel_ptr) noexcept
try {
	switch (address.type) {
		const FileAddress *file;
		const CgiAddress *cgi;
#ifdef HAVE_LIBNFS
		const NfsAddress *nfs;
#endif

	case ResourceAddress::Type::NONE:
		break;

	case ResourceAddress::Type::LOCAL:
		/* static files cannot receive a request body, close it */
		body.Clear();

		file = &address.GetFile();
		if (file->delegate != nullptr) {
			if (delegate_stock == nullptr)
				throw std::runtime_error("No delegate stock");

			delegate_stock_request(event_loop, *delegate_stock, pool,
					       file->delegate->delegate,
					       file->delegate->child_options,
					       file->path,
					       file->content_type,
					       handler,
					       cancel_ptr);
			return;
		}

		static_file_get(event_loop,
#ifdef HAVE_URING
				uring,
#endif
				pool, file->base, file->path,
				file->content_type,
				handler, cancel_ptr);
		return;

	case ResourceAddress::Type::NFS:
#ifdef HAVE_LIBNFS
		/* NFS files cannot receive a request body, close it */
		body.Clear();

		nfs = &address.GetNfs();

		nfs_request(pool, *nfs_cache,
			    nfs->server, nfs->export_name,
			    nfs->path, nfs->content_type,
			    handler, cancel_ptr);
		return;
#else
		throw std::runtime_error("NFS support is disabled");
#endif

	case ResourceAddress::Type::PIPE:
		cgi = &address.GetCgi();
		pipe_filter(spawn_service, event_loop, pool, parent_stopwatch,
			    cgi->path, cgi->args.ToArray(pool),
			    cgi->options,
			    status, std::move(headers), std::move(body),
			    handler);
		return;

	case ResourceAddress::Type::CGI:
		cgi_new(spawn_service, event_loop, &pool, parent_stopwatch,
			method, &address.GetCgi(),
			GetRemoteHost(xff, pool, headers),
			headers, std::move(body),
			handler, cancel_ptr);
		return;

	case ResourceAddress::Type::FASTCGI: {
		cgi = &address.GetCgi();

		UniqueFileDescriptor stderr_fd;
		if (cgi->options.stderr_path != nullptr &&
		    !cgi->options.stderr_jailed) {
			stderr_fd = cgi->options.OpenStderrPath();
		}

		const char *remote_ip = GetRemoteHost(xff, pool, headers);

		if (cgi->address_list.empty())
			fcgi_request(&pool, event_loop, fcgi_stock, parent_stopwatch,
				     params.site_name,
				     cgi->options,
				     cgi->action,
				     cgi->path,
				     cgi->args.ToArray(pool),
				     cgi->parallelism,
				     method, cgi->GetURI(pool),
				     cgi->script_name,
				     cgi->path_info,
				     cgi->query_string,
				     cgi->document_root,
				     remote_ip,
				     std::move(headers), std::move(body),
				     cgi->params.ToArray(pool),
				     std::move(stderr_fd),
				     handler, cancel_ptr);
		else
			fcgi_remote_request(&pool, event_loop, tcp_balancer,
					    parent_stopwatch,
					    &cgi->address_list,
					    cgi->path,
					    method, cgi->GetURI(pool),
					    cgi->script_name,
					    cgi->path_info,
					    cgi->query_string,
					    cgi->document_root,
					    remote_ip,
					    std::move(headers), std::move(body),
					    cgi->params.ToArray(pool),
					    std::move(stderr_fd),
					    handler, cancel_ptr);
		return;
	}

	case ResourceAddress::Type::WAS:
#ifdef HAVE_LIBWAS
		cgi = &address.GetCgi();

		if (cgi->concurrency == 0)
			was_request(pool, *was_stock, parent_stopwatch,
				    params.site_name,
				    cgi->options,
				    cgi->action,
				    cgi->path,
				    cgi->args.ToArray(pool),
				    cgi->parallelism,
				    GetRemoteHost(xff, pool, headers),
				    method, cgi->GetURI(pool),
				    cgi->script_name,
				    cgi->path_info,
				    cgi->query_string,
				    std::move(headers), std::move(body),
				    cgi->params.ToArray(pool),
				    handler, cancel_ptr);
		else if (!cgi->address_list.empty())
			SendRemoteWasRequest(pool, *remote_was_stock,
					     parent_stopwatch,
					     cgi->address_list.front(),
					     cgi->parallelism,
					     GetRemoteHost(xff, pool, headers),
					     method, cgi->GetURI(pool),
					     cgi->script_name,
					     cgi->path_info,
					     cgi->query_string,
					     std::move(headers), std::move(body),
					     cgi->params.ToArray(pool),
					     cgi->concurrency,
					     handler, cancel_ptr);
		else
			SendMultiWasRequest(pool, *multi_was_stock, parent_stopwatch,
					    params.site_name,
					    cgi->options,
					    cgi->action,
					    cgi->path,
					    cgi->args.ToArray(pool),
					    cgi->parallelism,
					    GetRemoteHost(xff, pool, headers),
					    method, cgi->GetURI(pool),
					    cgi->script_name,
					    cgi->path_info,
					    cgi->query_string,
					    std::move(headers), std::move(body),
					    cgi->params.ToArray(pool),
					    cgi->concurrency,
					    handler, cancel_ptr);
		return;
#else
		throw std::runtime_error("WAS support is disabled");
#endif

	case ResourceAddress::Type::HTTP:
		any_http_client.SendRequest(pool, parent_stopwatch,
					    params.sticky_hash,
					    method, address.GetHttp(),
					    std::move(headers),
					    std::move(body),
					    handler, cancel_ptr);
		return;

	case ResourceAddress::Type::LHTTP:
		lhttp_request(pool, event_loop, *lhttp_stock,
			      parent_stopwatch,
			      params.site_name,
			      address.GetLhttp(),
			      method, std::move(headers),
			      std::move(body),
			      handler, cancel_ptr);
		return;
	}

	/* the resource could not be located, abort the request */

	throw std::runtime_error("Could not locate resource");
} catch (...) {
	body.Clear();
	handler.InvokeError(std::current_exception());
}
