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

#include "nghttp2/Server.hxx"
#include "http/Headers.hxx"
#include "http/IncomingRequest.hxx"
#include "http/server/Handler.hxx"
#include "fs/FilteredSocket.hxx"
#include "event/Loop.hxx"
#include "event/net/TemplateServerSocket.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "util/PrintException.hxx"
#include "pool/RootPool.hxx"
#include "memory/fb_pool.hxx"

class Connection final
	: public AutoUnlinkIntrusiveListHook,
	  HttpServerConnectionHandler, HttpServerRequestHandler
{
	NgHttp2::ServerConnection http;

public:
	Connection(struct pool &pool, EventLoop &event_loop,
		   UniqueSocketDescriptor fd, SocketAddress address)
		:http(pool,
		      UniquePoolPtr<FilteredSocket>::Make(pool, event_loop,
							  std::move(fd), FD_TCP),
		      address,
		      *this, *this) {}

	/* virtual methods from class HttpServerConnectionHandler */
	void HandleHttpRequest(IncomingHttpRequest &request,
			       const StopwatchPtr &,
			       CancellablePointer &cancel_ptr) noexcept override {
		(void)cancel_ptr;
		// TODO

		if (request.body)
			request.SendResponse(HTTP_STATUS_OK, {},
					     std::move(request.body));
		else
			request.SendMessage(HTTP_STATUS_OK, "Hello, world!\n");
	}

	void HttpConnectionError(std::exception_ptr e) noexcept override {
		PrintException(e);
		delete this;
	}

	void HttpConnectionClosed() noexcept override {
		delete this;
	}
};

typedef TemplateServerSocket<Connection, struct pool &,
			     EventLoop &> Listener;

int
main(int, char **) noexcept
try {
	const ScopeFbPoolInit fb_pool_init;
	RootPool pool;
	EventLoop event_loop;

	Listener listener(event_loop, pool.get(), event_loop);
	listener.ListenTCP(8000);

	event_loop.Dispatch();
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}
