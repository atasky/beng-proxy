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

#pragma once

#include "cluster/StickyHash.hxx"
#include "http/Method.h"
#include "http/Status.h"

struct pool;
class StopwatchPtr;
class UnusedIstreamPtr;
struct ResourceAddress;
class StringMap;
class HttpResponseHandler;
class CancellablePointer;

/**
 * Container for various additional parameters passed to
 * ResourceLoader::SendRequest().  Having this in a separate struct
 * unclutters the #ResourceLoader interface and allows adding more
 * parameters easily.
 */
struct ResourceRequestParams {
	/**
	 * A portion of the session id that is used to select
	 * the worker; 0 means disable stickiness.
	 */
	sticky_hash_t sticky_hash;

	bool eager_cache;

	bool auto_flush_cache;

	/**
	 * An opaque tag string to be assigned to the cache
	 * item (if the response is going to be cached by the
	 * #ResourceLoader); may be nullptr.
	 */
	const char *cache_tag;

	/**
	 * The name of the site this request belongs to; may
	 * be nullptr.
	 */
	const char *site_name;
};

/**
 * Load resources specified by a resource_address.
 */
class ResourceLoader {
public:
	/**
	 * Requests a resource.
	 *
	 * @param address the address of the resource
	 * @param status a HTTP status code for protocols which do have one
	 * @param body the request body
	 * @param body_etag a unique identifier for the request body; if
	 * not nullptr, it may be used to cache POST requests
	 */
	virtual void SendRequest(struct pool &pool,
				 const StopwatchPtr &parent_stopwatch,
				 const ResourceRequestParams &params,
				 http_method_t method,
				 const ResourceAddress &address,
				 http_status_t status, StringMap &&headers,
				 UnusedIstreamPtr body, const char *body_etag,
				 HttpResponseHandler &handler,
				 CancellablePointer &cancel_ptr) noexcept = 0;
};
