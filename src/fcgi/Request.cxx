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

#include "Request.hxx"
#include "Stock.hxx"
#include "Client.hxx"
#include "http/ResponseHandler.hxx"
#include "lease.hxx"
#include "stock/Item.hxx"
#include "istream/UnusedPtr.hxx"
#include "pool/pool.hxx"
#include "pool/LeakDetector.hxx"
#include "net/SocketDescriptor.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/Cancellable.hxx"
#include "stopwatch.hxx"

#include <sys/socket.h>

class FcgiRequest final : Lease, Cancellable, PoolLeakDetector {
	struct pool &pool;

	StockItem *stock_item;

	CancellablePointer cancel_ptr;

public:
	FcgiRequest(struct pool &_pool, StockItem &_stock_item) noexcept
		:PoolLeakDetector(_pool),
		 pool(_pool), stock_item(&_stock_item)
	{
	}

	void Start(EventLoop &event_loop, StopwatchPtr &&stopwatch,
		   const char *site_name,
		   const char *path,
		   http_method_t method, const char *uri,
		   const char *script_name, const char *path_info,
		   const char *query_string,
		   const char *document_root,
		   const char *remote_addr,
		   StringMap &&headers, UnusedIstreamPtr body,
		   std::span<const char *const> params,
		   UniqueFileDescriptor &&stderr_fd,
		   HttpResponseHandler &handler,
		   CancellablePointer &caller_cancel_ptr) noexcept {
		caller_cancel_ptr = *this;

		fcgi_stock_item_set_site(*stock_item, site_name);
		fcgi_stock_item_set_uri(*stock_item, uri);

		if (!stderr_fd.IsDefined())
			stderr_fd = fcgi_stock_item_get_stderr(*stock_item);

		const char *script_filename = path;

		fcgi_client_request(&pool, event_loop, std::move(stopwatch),
				    fcgi_stock_item_get(*stock_item),
				    fcgi_stock_item_get_domain(*stock_item) == AF_LOCAL
				    ? FdType::FD_SOCKET : FdType::FD_TCP,
				    *this,
				    method, uri,
				    script_filename,
				    script_name, path_info,
				    query_string,
				    document_root,
				    remote_addr,
				    std::move(headers), std::move(body),
				    params,
				    std::move(stderr_fd),
				    handler, cancel_ptr);
	}

private:
	void Destroy() noexcept {
		DeleteFromPool(pool, this);
	}

	/* virtual methods from class Cancellable */
	void Cancel() noexcept override {
		fcgi_stock_aborted(*stock_item);

		cancel_ptr.Cancel();
	}

	/* virtual methods from class Lease */
	void ReleaseLease(bool reuse) noexcept override {
		stock_item->Put(!reuse);
		stock_item = nullptr;

		Destroy();
	}
};

void
fcgi_request(struct pool *pool, EventLoop &event_loop,
	     FcgiStock *fcgi_stock,
	     const StopwatchPtr &parent_stopwatch,
	     const char *site_name,
	     const ChildOptions &options,
	     const char *action,
	     const char *path,
	     std::span<const char *const> args,
	     unsigned parallelism,
	     http_method_t method, const char *uri,
	     const char *script_name, const char *path_info,
	     const char *query_string,
	     const char *document_root,
	     const char *remote_addr,
	     StringMap &&headers, UnusedIstreamPtr body,
	     std::span<const char *const> params,
	     UniqueFileDescriptor &&stderr_fd,
	     HttpResponseHandler &handler,
	     CancellablePointer &cancel_ptr) noexcept
{
	if (action == nullptr)
		action = path;

	StopwatchPtr stopwatch(parent_stopwatch, "fcgi", action);

	StockItem *stock_item;
	try {
		stock_item = fcgi_stock_get(fcgi_stock, options,
					    action, args, parallelism);
	} catch (...) {
		stopwatch.RecordEvent("launch_error");
		body.Clear();
		handler.InvokeError(std::current_exception());
		return;
	}

	stopwatch.RecordEvent("fork");

	auto request = NewFromPool<FcgiRequest>(*pool, *pool, *stock_item);

	request->Start(event_loop, std::move(stopwatch),
		       site_name, path, method, uri,
		       script_name, path_info,
		       query_string, document_root, remote_addr,
		       std::move(headers), std::move(body),
		       params, std::move(stderr_fd),
		       handler, cancel_ptr);
}
