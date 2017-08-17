/*
 * Copyright 2007-2017 Content Management AG
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
 * High level AJP client.
 */

#ifndef BENG_PROXY_AJP_REQUEST_HXX
#define BENG_PROXY_AJP_REQUEST_HXX

#include "StickyHash.hxx"

#include <http/method.h>

struct pool;
class EventLoop;
class Istream;
struct TcpBalancer;
struct HttpAddress;
class StringMap;
class HttpResponseHandler;
class CancellablePointer;

/**
 * @param session_sticky a portion of the session id that is used to
 * select the worker; 0 means disable stickiness
 */
void
ajp_stock_request(struct pool &pool, EventLoop &event_loop,
                  TcpBalancer &tcp_balancer,
                  sticky_hash_t session_sticky,
                  const char *protocol, const char *remote_addr,
                  const char *remote_host, const char *server_name,
                  unsigned server_port, bool is_ssl,
                  http_method_t method,
                  const HttpAddress &uwa,
                  StringMap &&headers, Istream *body,
                  HttpResponseHandler &handler,
                  CancellablePointer &cancel_ptr);

#endif
