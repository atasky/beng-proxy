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

#include "Internal.hxx"
#include "Document.hxx"
#include "Item.hxx"
#include "RFC.hxx"
#include "Heap.hxx"
#include "strmap.hxx"
#include "http/ResponseHandler.hxx"
#include "ResourceLoader.hxx"
#include "ResourceAddress.hxx"
#include "memory/sink_rubber.hxx"
#include "stats/AllocatorStats.hxx"
#include "http/Date.hxx"
#include "http/List.hxx"
#include "istream/UnusedPtr.hxx"
#include "istream/TeeIstream.hxx"
#include "istream/RefIstream.hxx"
#include "pool/Holder.hxx"
#include "AllocatorPtr.hxx"
#include "event/FarTimerEvent.hxx"
#include "event/Loop.hxx"
#include "io/Logger.hxx"
#include "util/Cancellable.hxx"
#include "util/Exception.hxx"
#include "util/RuntimeError.hxx"

#include <boost/intrusive/list.hpp>

#include <functional>

#include <string.h>
#include <stdio.h>

static constexpr Event::Duration http_cache_compress_interval = std::chrono::minutes(10);

static constexpr bool
IsModifyingMethod(http_method_t method) noexcept
{
	// TODO: code copied from MethodNeedsCsrfProtection()
	switch (method) {
	case HTTP_METHOD_HEAD:
	case HTTP_METHOD_GET:
	case HTTP_METHOD_OPTIONS:
	case HTTP_METHOD_TRACE:
	case HTTP_METHOD_PROPFIND:
	case HTTP_METHOD_REPORT:
		return false;

	default:
		return true;
	}
}

class HttpCacheRequest final : PoolHolder,
			       HttpResponseHandler,
			       RubberSinkHandler,
			       Cancellable {
public:
	static constexpr auto link_mode = boost::intrusive::normal_link;
	typedef boost::intrusive::link_mode<link_mode> LinkMode;
	typedef boost::intrusive::list_member_hook<LinkMode> SiblingsHook;
	SiblingsHook siblings;

private:
	PoolPtr caller_pool;

	const char *const cache_tag;

	/**
	 * The cache object which got this request.
	 */
	HttpCache &cache;

	/**
	 * The cache key used to address the associated cache document.
	 */
	const char *key;

	/** headers from the original request */
	const StringMap request_headers;

	HttpResponseHandler &handler;

	const HttpCacheRequestInfo request_info;

	/**
	 * Information on the request passed to http_cache_request().
	 */
	HttpCacheResponseInfo info;

	/**
	 * The document which was found in the cache, in case this is a
	 * request to test the validity of the cache entry.  If this is
	 * nullptr, then we had a cache miss.
	 */
	HttpCacheDocument *const document;

	/**
	 * This struct holds response information while this module
	 * receives the response body.
	 */
	struct {
		http_status_t status;
		StringMap *headers;
	} response;

	CancellablePointer cancel_ptr;

	const bool eager_cache;

public:
	HttpCacheRequest(PoolPtr &&_pool, struct pool &_caller_pool,
			 bool _eager_cache,
			 const char *_cache_tag,
			 HttpCache &_cache,
			 const ResourceAddress &_address,
			 const StringMap &_headers,
			 HttpResponseHandler &_handler,
			 const HttpCacheRequestInfo &_info,
			 HttpCacheDocument *_document) noexcept;

	HttpCacheRequest(const HttpCacheRequest &) = delete;
	HttpCacheRequest &operator=(const HttpCacheRequest &) = delete;

	using PoolHolder::GetPool;

	const char *GetKey() const noexcept {
		return key;
	}

	void Start(ResourceLoader &next,
		   const StopwatchPtr &parent_stopwatch,
		   const ResourceRequestParams &params,
		   http_method_t method,
		   const ResourceAddress &address,
		   StringMap &&_headers,
		   CancellablePointer &_cancel_ptr) noexcept {
		_cancel_ptr = *this;

		next.SendRequest(GetPool(), parent_stopwatch,
				 params,
				 method, address,
				 HTTP_STATUS_OK, std::move(_headers),
				 nullptr, nullptr,
				 *this, cancel_ptr);
	}

	EventLoop &GetEventLoop() const noexcept;

	void Serve() noexcept;

	void Put(RubberAllocation &&a, size_t size) noexcept;

	/**
	 * Storing the response body in the rubber allocator has finished
	 * (but may have failed).
	 */
	void RubberStoreFinished() noexcept;

	/**
	 * Abort storing the response body in the rubber allocator.
	 *
	 * This will not remove the request from the HttpCache, because
	 * this method is supposed to be used as a "disposer".
	 */
	void AbortRubberStore() noexcept;

private:
	void Destroy() noexcept {
		this->~HttpCacheRequest();
	}

	/* virtual methods from class Cancellable */
	void Cancel() noexcept override;

	/* virtual methods from class HttpResponseHandler */
	void OnHttpResponse(http_status_t status, StringMap &&headers,
			    UnusedIstreamPtr body) noexcept override;
	void OnHttpError(std::exception_ptr ep) noexcept override;

	/* virtual methods from class RubberSinkHandler */
	void RubberDone(RubberAllocation &&a, size_t size) noexcept override;
	void RubberOutOfMemory() noexcept override;
	void RubberTooLarge() noexcept override;
	void RubberError(std::exception_ptr ep) noexcept override;
};

/**
 * Wrapper for a uncacheable request which implements #AUTO_FLUSH_CACHE.
 */
class AutoFlushHttpCacheRequest final
	: public HttpResponseHandler, Cancellable
{
	const char *const cache_tag;

	/**
	 * The cache object which got this request.
	 */
	HttpCache &cache;

	HttpResponseHandler &handler;

	CancellablePointer cancel_ptr;

public:
	AutoFlushHttpCacheRequest(const char *_cache_tag,
				  HttpCache &_cache,
				  HttpResponseHandler &_handler) noexcept
		:cache_tag(_cache_tag),
		 cache(_cache),
		 handler(_handler) {}

	AutoFlushHttpCacheRequest(const AutoFlushHttpCacheRequest &) = delete;
	AutoFlushHttpCacheRequest &operator=(const AutoFlushHttpCacheRequest &) = delete;

	void Start(ResourceLoader &next, struct pool &pool,
		   const StopwatchPtr &parent_stopwatch,
		   const ResourceRequestParams &params,
		   http_method_t method,
		   const ResourceAddress &address,
		   StringMap &&_headers, UnusedIstreamPtr body,
		   CancellablePointer &_cancel_ptr) noexcept {
		_cancel_ptr = *this;

		next.SendRequest(pool, parent_stopwatch,
				 params,
				 method, address,
				 HTTP_STATUS_OK, std::move(_headers),
				 std::move(body), nullptr,
				 *this, cancel_ptr);
	}

private:
	void Destroy() noexcept {
		this->~AutoFlushHttpCacheRequest();
	}

	/* virtual methods from class Cancellable */
	void Cancel() noexcept override {
		cancel_ptr.Cancel();
		Destroy();
	}

	/* virtual methods from class HttpResponseHandler */
	void OnHttpResponse(http_status_t status, StringMap &&headers,
			    UnusedIstreamPtr body) noexcept override;

	void OnHttpError(std::exception_ptr e) noexcept override {
		auto &_handler = handler;
		Destroy();
		_handler.InvokeError(std::move(e));
	}
};

class HttpCache {
	const PoolPtr pool;

	EventLoop &event_loop;

	FarTimerEvent compress_timer;

	HttpCacheHeap heap;

	ResourceLoader &resource_loader;

	/**
	 * A list of requests that are currently saving their contents to
	 * the cache.
	 */
	boost::intrusive::list<HttpCacheRequest,
			       boost::intrusive::member_hook<HttpCacheRequest,
							     HttpCacheRequest::SiblingsHook,
							     &HttpCacheRequest::siblings>,
			       boost::intrusive::constant_time_size<false>> requests;

	const bool obey_no_cache;

public:
	HttpCache(struct pool &_pool, size_t max_size,
		  bool obey_no_cache,
		  EventLoop &event_loop,
		  ResourceLoader &_resource_loader);

	HttpCache(const HttpCache &) = delete;
	HttpCache &operator=(const HttpCache &) = delete;

	~HttpCache() noexcept;

	EventLoop &GetEventLoop() const noexcept {
		return event_loop;
	}

	Rubber &GetRubber() noexcept {
		return heap.GetRubber();
	}

	void ForkCow(bool inherit) noexcept {
		heap.ForkCow(inherit);
	}

	AllocatorStats GetStats() const noexcept {
		return heap.GetStats();
	}

	void Flush() noexcept {
		heap.Flush();
	}

	void FlushTag(const std::string &tag) noexcept {
		heap.FlushTag(tag);
	}

	void AddRequest(HttpCacheRequest &r) noexcept {
		requests.push_front(r);
	}

	void RemoveRequest(HttpCacheRequest &r) noexcept {
		requests.erase(requests.iterator_to(r));
	}

	void Start(struct pool &caller_pool,
		   const StopwatchPtr &parent_stopwatch,
		   const ResourceRequestParams &params,
		   http_method_t method,
		   const ResourceAddress &address,
		   StringMap &&headers, UnusedIstreamPtr body,
		   HttpResponseHandler &handler,
		   CancellablePointer &cancel_ptr) noexcept;

	void Put(const char *url, const char *tag,
		 const HttpCacheResponseInfo &info,
		 const StringMap &request_headers,
		 http_status_t status,
		 const StringMap &response_headers,
		 RubberAllocation &&a, size_t size) noexcept {
		LogConcat(4, "HttpCache", "put ", url);

		heap.Put(url, tag, info, request_headers,
			 status, response_headers,
			 std::move(a), size);
	}

	void Remove(HttpCacheDocument *document) noexcept {
		heap.Remove(*document);
	}

	void RemoveURL(const char *url, StringMap &headers) noexcept {
		heap.RemoveURL(url, headers);
	}

	void Lock(HttpCacheDocument &document) noexcept {
		heap.Lock(document);
	}

	void Unlock(HttpCacheDocument &document) noexcept {
		heap.Unlock(document);
	}

	/**
	 * Query the cache.
	 *
	 * Caller pool is referenced synchronously and freed
	 * asynchronously (as needed).
	 */
	void Use(struct pool &caller_pool,
		 const StopwatchPtr &parent_stopwatch,
		 const ResourceRequestParams &params,
		 http_method_t method,
		 const ResourceAddress &address,
		 StringMap &&headers,
		 const HttpCacheRequestInfo &info,
		 HttpResponseHandler &handler,
		 CancellablePointer &cancel_ptr) noexcept;

	/**
	 * Send the cached document to the caller.
	 *
	 * Caller pool is left unchanged.
	 */
	void Serve(struct pool &caller_pool,
		   HttpCacheDocument &document,
		   const char *key,
		   HttpResponseHandler &handler) noexcept;

private:
	/**
	 * A resource was not found in the cache.
	 *
	 * Caller pool is referenced synchronously and freed asynchronously.
	 */
	void Miss(struct pool &caller_pool,
		  const StopwatchPtr &parent_stopwatch,
		  const ResourceRequestParams &params,
		  const HttpCacheRequestInfo &info,
		  http_method_t method,
		  const ResourceAddress &address,
		  StringMap &&headers,
		  HttpResponseHandler &handler,
		  CancellablePointer &cancel_ptr) noexcept;

	/**
	 * Revalidate a cache entry.
	 *
	 * Caller pool is referenced synchronously and freed asynchronously.
	 */
	void Revalidate(struct pool &caller_pool,
			const StopwatchPtr &parent_stopwatch,
			const ResourceRequestParams &params,
			const HttpCacheRequestInfo &info,
			HttpCacheDocument &document,
			http_method_t method,
			const ResourceAddress &address,
			StringMap &&headers,
			HttpResponseHandler &handler,
			CancellablePointer &cancel_ptr) noexcept;

	/**
	 * The requested document was found in the cache.  It is either
	 * served or revalidated.
	 *
	 * Caller pool is referenced synchronously and freed asynchronously
	 * (as needed).
	 */
	void Found(const HttpCacheRequestInfo &info,
		   HttpCacheDocument &document,
		   struct pool &caller_pool,
		   const StopwatchPtr &parent_stopwatch,
		   const ResourceRequestParams &params,
		   http_method_t method,
		   const ResourceAddress &address,
		   StringMap &&headers,
		   HttpResponseHandler &handler,
		   CancellablePointer &cancel_ptr) noexcept;

	void OnCompressTimer() noexcept {
		heap.Compress();
		compress_timer.Schedule(http_cache_compress_interval);
	}
};

static void
UpdateHeader(AllocatorPtr alloc, StringMap &dest,
	     const StringMap &src, const char *name) noexcept
{
	const char *value = src.Get(name);
	if (value != nullptr)
		dest.SecureSet(alloc, name, alloc.Dup(value));
}

static const char *
http_cache_key(struct pool &pool, const ResourceAddress &address) noexcept
{
	switch (address.type) {
	case ResourceAddress::Type::NONE:
	case ResourceAddress::Type::LOCAL:
	case ResourceAddress::Type::PIPE:
		/* not cacheable */
		return nullptr;

	case ResourceAddress::Type::HTTP:
	case ResourceAddress::Type::LHTTP:
	case ResourceAddress::Type::CGI:
	case ResourceAddress::Type::FASTCGI:
	case ResourceAddress::Type::WAS:
	case ResourceAddress::Type::NFS:
		return address.GetId(pool);
	}

	/* unreachable */
	assert(false);
	return nullptr;
}

inline EventLoop &
HttpCacheRequest::GetEventLoop() const noexcept
{
	return cache.GetEventLoop();
}

void
HttpCacheRequest::Put(RubberAllocation &&a, size_t size) noexcept
{
	cache.Put(key, cache_tag, info, request_headers,
		  response.status, *response.headers,
		  std::move(a), size);
}

/*
 * sink_rubber handler
 *
 */

void
HttpCacheRequest::RubberDone(RubberAllocation &&a, size_t size) noexcept
{
	RubberStoreFinished();

	/* the request was successful, and all of the body data has been
	   saved: add it to the cache */
	Put(std::move(a), size);
	Destroy();
}

void
HttpCacheRequest::RubberOutOfMemory() noexcept
{
	LogConcat(4, "HttpCache", "nocache oom ", key);

	RubberStoreFinished();
	Destroy();
}

void
HttpCacheRequest::RubberTooLarge() noexcept
{
	LogConcat(4, "HttpCache", "nocache too large ", key);

	RubberStoreFinished();
	Destroy();
}

void
HttpCacheRequest::RubberError(std::exception_ptr ep) noexcept
{
	LogConcat(4, "HttpCache", "body_abort ", key, ": ", ep);

	RubberStoreFinished();
	Destroy();
}

/*
 * http response handler
 *
 */

void
AutoFlushHttpCacheRequest::OnHttpResponse(http_status_t status,
					  StringMap &&_headers,
					  UnusedIstreamPtr body) noexcept
{
	assert(cache_tag != nullptr);

	if (!http_status_is_error(status))
		cache.FlushTag(cache_tag);

	auto &_handler = handler;
	Destroy();

	_handler.InvokeResponse(status, std::move(_headers), std::move(body));
}

void
HttpCacheRequest::OnHttpResponse(http_status_t status, StringMap &&_headers,
				 UnusedIstreamPtr body) noexcept
{
	const AllocatorPtr alloc{GetPool()};

	HttpCacheDocument *locked_document = document;

	if (document != nullptr && status == HTTP_STATUS_NOT_MODIFIED) {
		assert(!body);

		if (auto _info = http_cache_response_evaluate(request_info,
							      alloc,
							      eager_cache,
							      HTTP_STATUS_OK,
							      _headers, -1);
		    _info && _info->expires >= GetEventLoop().SystemNow()) {
			/* copy the new "Expires" (or "max-age") value from the
			   "304 Not Modified" response */
			auto &item = *(HttpCacheItem *)document;
			item.SetExpires(GetEventLoop().SteadyNow(),
					GetEventLoop().SystemNow(),
					_info->expires);

			/* TODO: this leaks pool memory each time we update
			   headers; how to fix this? */
			UpdateHeader(alloc, document->response_headers, _headers, "expires");
			UpdateHeader(alloc, document->response_headers, _headers, "cache-control");
		}

		LogConcat(5, "HttpCache", "not_modified ", key);
		Serve();

		if (locked_document != nullptr)
			cache.Unlock(*locked_document);

		Destroy();
		return;
	}

	if (document != nullptr &&
	    http_cache_prefer_cached(*document, _headers)) {
		LogConcat(4, "HttpCache", "matching etag '", document->info.etag,
			  "' for ", key, ", using cache entry");

		body.Clear();

		Serve();

		if (locked_document != nullptr)
			cache.Unlock(*locked_document);

		Destroy();
		return;
	}

	if (document != nullptr)
		cache.Remove(document);

	const off_t available = body
		? body.GetAvailable(true)
		: 0;

	if (auto _info = http_cache_response_evaluate(request_info, alloc,
						      eager_cache,
						      status, _headers,
						      available);
	    _info) {
		info = std::move(*_info);
	} else {
		/* don't cache response */
		LogConcat(4, "HttpCache", "nocache ", key);

		if (body)
			body = NewRefIstream(pool, std::move(body));
		else
			/* workaround: if there is no response body,
			   nobody will hold a pool reference, and the
			   headers will be freed after
			   InvokeResponse() returns; in that case, we
			   need to copy all headers into the caller's
			   pool to avoid use-after-free bugs */
			_headers = {caller_pool, _headers};

		handler.InvokeResponse(status, std::move(_headers), std::move(body));
		Destroy();
		return;
	}

	response.status = status;
	response.headers = strmap_dup(pool, &_headers);

	/* move the caller_pool reference to the stack to ensure it gets
	   unreferenced at the end of this method - not earlier and not
	   later */
	const PoolPtr _caller_pool = std::move(caller_pool);

	/* copy the HttpResponseHandler reference to the stack, because
	   the sink_rubber_new() call may destroy this object */
	auto &_handler = handler;

	/* hold an additional pool reference to ensure that all header
	   strings stay valid until he handler returns, just in case
	   sink_rubber_new() destroys this object and the pool; TODO:
	   find a better solution for this */
	const ScopePoolRef ref(pool);

	bool destroy = false;
	if (!body) {
		Put({}, 0);
		destroy = true;

		/* workaround: if there is no response body, nobody
		   will hold a pool reference, and the headers will be
		   freed after InvokeResponse() returns; in that case,
		   we need to copy all headers into the caller's pool
		   to avoid use-after-free bugs */
		_headers = {_caller_pool, _headers};
	} else {
		/* this->info was allocated from the caller pool; duplicate
		   it to keep it alive even after the caller pool is
		   destroyed */
		key = alloc.Dup(key);
		info.MoveToPool(alloc);

		/* tee the body: one goes to our client, and one goes into the
		   cache */
		auto tee = NewTeeIstream(pool, std::move(body),
					 GetEventLoop(),
					 false,
					 /* just in case our handler closes
					    the body without looking at it:
					    defer an Istream::Read() call for
					    the Rubber sink */
					 true);

		cache.AddRequest(*this);

		sink_rubber_new(pool, AddTeeIstream(tee, false),
				cache.GetRubber(), cacheable_size_limit,
				*this,
				cancel_ptr);

		body = std::move(tee);
	}

	_handler.InvokeResponse(status, std::move(_headers), std::move(body));

	if (destroy)
		Destroy();
}

void
HttpCacheRequest::OnHttpError(std::exception_ptr ep) noexcept
{
	ep = NestException(ep, FormatRuntimeError("http_cache %s", key));

	if (document != nullptr)
		cache.Unlock(*document);

	handler.InvokeError(ep);
	Destroy();
}

/*
 * async operation
 *
 */

void
HttpCacheRequest::Cancel() noexcept
{
	if (document != nullptr)
		cache.Unlock(*document);

	cancel_ptr.Cancel();
	Destroy();
}


/*
 * constructor and public methods
 *
 */

HttpCacheRequest::HttpCacheRequest(PoolPtr &&_pool,
				   struct pool &_caller_pool,
				   bool _eager_cache,
				   const char *_cache_tag,
				   HttpCache &_cache,
				   const ResourceAddress &address,
				   const StringMap &_headers,
				   HttpResponseHandler &_handler,
				   const HttpCacheRequestInfo &_request_info,
				   HttpCacheDocument *_document) noexcept
	:PoolHolder(std::move(_pool)), caller_pool(_caller_pool),
	 cache_tag(_cache_tag),
	 cache(_cache),
	 key(http_cache_key(pool, address)),
	 request_headers(pool, _headers),
	 handler(_handler),
	 request_info(_request_info),
	 document(_document),
	 eager_cache(_eager_cache)
{
}

inline
HttpCache::HttpCache(struct pool &_pool, size_t max_size,
		     bool _obey_no_cache,
		     EventLoop &_event_loop,
		     ResourceLoader &_resource_loader)
	:pool(pool_new_dummy(&_pool, "http_cache")),
	 event_loop(_event_loop),
	 compress_timer(event_loop, BIND_THIS_METHOD(OnCompressTimer)),
	 heap(pool, event_loop, max_size),
	 resource_loader(_resource_loader),
	 obey_no_cache(_obey_no_cache)
{
	assert(max_size > 0);

	compress_timer.Schedule(http_cache_compress_interval);
}

HttpCache *
http_cache_new(struct pool &pool, size_t max_size,
	       bool obey_no_cache,
	       EventLoop &event_loop,
	       ResourceLoader &resource_loader)
{
	assert(max_size > 0);

	return new HttpCache(pool, max_size, obey_no_cache,
			     event_loop, resource_loader);
}

void
HttpCacheRequest::RubberStoreFinished() noexcept
{
	assert(cancel_ptr);

	cancel_ptr = nullptr;
	cache.RemoveRequest(*this);
}

void
HttpCacheRequest::AbortRubberStore() noexcept
{
	cancel_ptr.Cancel();
	Destroy();
}

inline
HttpCache::~HttpCache() noexcept
{
	requests.clear_and_dispose(std::mem_fn(&HttpCacheRequest::AbortRubberStore));
}

void
http_cache_close(HttpCache *cache) noexcept
{
	delete cache;
}

void
http_cache_fork_cow(HttpCache &cache, bool inherit) noexcept
{
	cache.ForkCow(inherit);
}

AllocatorStats
http_cache_get_stats(const HttpCache &cache) noexcept
{
	return cache.GetStats();
}

void
http_cache_flush(HttpCache &cache) noexcept
{
	cache.Flush();
}

void
http_cache_flush_tag(HttpCache &cache, const std::string &tag) noexcept
{
	cache.FlushTag(tag);
}

void
HttpCache::Miss(struct pool &caller_pool,
		const StopwatchPtr &parent_stopwatch,
		const ResourceRequestParams &params,
		const HttpCacheRequestInfo &info,
		http_method_t method,
		const ResourceAddress &address,
		StringMap &&headers,
		HttpResponseHandler &handler,
		CancellablePointer &cancel_ptr) noexcept
{
	if (info.only_if_cached) {
		handler.InvokeResponse(HTTP_STATUS_GATEWAY_TIMEOUT,
				       {}, UnusedIstreamPtr());
		return;
	}

	/* the cache request may live longer than the caller pool, so
	   allocate a new pool for it from cache.pool */
	auto request_pool = pool_new_linear(pool, "HttpCacheRequest", 8192);

	auto request =
		NewFromPool<HttpCacheRequest>(std::move(request_pool), caller_pool,
					      params.eager_cache,
					      params.cache_tag,
					      *this,
					      address,
					      headers,
					      handler,
					      info, nullptr);

	LogConcat(4, "HttpCache", "miss ", request->GetKey());

	request->Start(resource_loader, parent_stopwatch,
		       params,
		       method, address,
		       std::move(headers),
		       cancel_ptr);
}

[[gnu::pure]]
static bool
CheckETagList(const char *list, const StringMap &response_headers) noexcept
{
	assert(list != nullptr);

	if (strcmp(list, "*") == 0)
		return true;

	const char *etag = response_headers.Get("etag");
	return etag != nullptr && http_list_contains(list, etag);
}

static void
DispatchNotModified(struct pool &pool, const HttpCacheDocument &document,
		    HttpResponseHandler &handler) noexcept
{
	handler.InvokeResponse(HTTP_STATUS_NOT_MODIFIED,
			       StringMap(pool, document.response_headers),
			       UnusedIstreamPtr());
}

static bool
CheckCacheRequest(struct pool &pool, const HttpCacheRequestInfo &info,
		  const HttpCacheDocument &document,
		  HttpResponseHandler &handler) noexcept
{
	bool ignore_if_modified_since = false;

	if (info.if_match != nullptr &&
	    !CheckETagList(info.if_match, document.response_headers)) {
		handler.InvokeResponse(HTTP_STATUS_PRECONDITION_FAILED,
				       {}, UnusedIstreamPtr());
		return false;
	}

	if (info.if_none_match != nullptr) {
		if (CheckETagList(info.if_none_match, document.response_headers)) {
			DispatchNotModified(pool, document, handler);
			return false;
		}

		/* RFC 2616 14.26: "If none of the entity tags match, then the
		   server MAY perform the requested method as if the
		   If-None-Match header field did not exist, but MUST also
		   ignore any If-Modified-Since header field(s) in the
		   request." */
		ignore_if_modified_since = true;
	}

	if (info.if_modified_since && !ignore_if_modified_since) {
		const char *last_modified = document.response_headers.Get("last-modified");
		if (last_modified != nullptr) {
			if (strcmp(info.if_modified_since, last_modified) == 0) {
				/* common fast path: client sends the previous
				   Last-Modified header string as-is */
				DispatchNotModified(pool, document, handler);
				return false;
			}

			const auto ims = http_date_parse(info.if_modified_since);
			const auto lm = http_date_parse(last_modified);
			if (ims != std::chrono::system_clock::from_time_t(-1) &&
			    lm != std::chrono::system_clock::from_time_t(-1) &&
			    lm <= ims) {
				DispatchNotModified(pool, document, handler);
				return false;
			}
		}
	}

	if (info.if_unmodified_since) {
		const char *last_modified = document.response_headers.Get("last-modified");
		if (last_modified != nullptr) {
			const auto iums = http_date_parse(info.if_unmodified_since);
			const auto lm = http_date_parse(last_modified);
			if (iums != std::chrono::system_clock::from_time_t(-1) &&
			    lm != std::chrono::system_clock::from_time_t(-1) &&
			    lm > iums) {
				handler.InvokeResponse(HTTP_STATUS_PRECONDITION_FAILED,
						       {}, UnusedIstreamPtr());
				return false;
			}
		}
	}

	return true;
}

void
HttpCache::Serve(struct pool &caller_pool,
		 HttpCacheDocument &document,
		 const char *key,
		 HttpResponseHandler &handler) noexcept
{
	LogConcat(4, "HttpCache", "serve ", key);

	auto body = heap.OpenStream(caller_pool, document);

	StringMap headers = body
		? StringMap{ShallowCopy{}, caller_pool, document.response_headers}
		/* workaround: if there is no response body, nobody
		   will hold a pool reference, and the headers will be
		   freed after InvokeResponse() returns; in that case,
		   we need to copy all headers into the caller's pool
		   to avoid use-after-free bugs */
		: StringMap{caller_pool, document.response_headers};

	handler.InvokeResponse(document.status,
			       std::move(headers),
			       std::move(body));
}

/**
 * Send the cached document to the caller.
 *
 * Caller pool is left unchanged.
 */
void
HttpCacheRequest::Serve() noexcept
{
	if (!CheckCacheRequest(pool, request_info, *document, handler))
		return;

	cache.Serve(caller_pool, *document, key, handler);
}

void
HttpCache::Revalidate(struct pool &caller_pool,
		      const StopwatchPtr &parent_stopwatch,
		      const ResourceRequestParams &params,
		      const HttpCacheRequestInfo &info,
		      HttpCacheDocument &document,
		      http_method_t method,
		      const ResourceAddress &address,
		      StringMap &&headers,
		      HttpResponseHandler &handler,
		      CancellablePointer &cancel_ptr) noexcept
{
	/* the cache request may live longer than the caller pool, so
	   allocate a new pool for it from cache.pool */
	auto request_pool = pool_new_linear(pool, "HttpCacheRequest", 8192);

	Lock(document);

	auto request =
		NewFromPool<HttpCacheRequest>(std::move(request_pool), caller_pool,
					      params.eager_cache,
					      params.cache_tag,
					      *this,
					      address,
					      headers,
					      handler,
					      info, &document);

	LogConcat(4, "HttpCache", "test ", request->GetKey());

	if (document.info.last_modified != nullptr)
		headers.Set(request->GetPool(),
			    "if-modified-since", document.info.last_modified);

	if (document.info.etag != nullptr)
		headers.Set(request->GetPool(),
			    "if-none-match", document.info.etag);

	request->Start(resource_loader, parent_stopwatch,
		       params,
		       method, address, std::move(headers),
		       cancel_ptr);
}

[[gnu::pure]]
static bool
http_cache_may_serve(EventLoop &event_loop,
		     const HttpCacheRequestInfo &info,
		     const HttpCacheDocument &document) noexcept
{
	return info.only_if_cached ||
		document.info.expires >= event_loop.SystemNow();
}

void
HttpCache::Found(const HttpCacheRequestInfo &info,
		 HttpCacheDocument &document,
		 struct pool &caller_pool,
		 const StopwatchPtr &parent_stopwatch,
		 const ResourceRequestParams &params,
		 http_method_t method,
		 const ResourceAddress &address,
		 StringMap &&headers,
		 HttpResponseHandler &handler,
		 CancellablePointer &cancel_ptr) noexcept
{
	if (!CheckCacheRequest(caller_pool, info, document, handler))
		return;

	if (http_cache_may_serve(GetEventLoop(), info, document))
		Serve(caller_pool, document,
		      http_cache_key(caller_pool, address),
		      handler);
	else
		Revalidate(caller_pool, parent_stopwatch,
			   params,
			   info, document,
			   method, address, std::move(headers),
			   handler, cancel_ptr);
}

void
HttpCache::Use(struct pool &caller_pool,
	       const StopwatchPtr &parent_stopwatch,
	       const ResourceRequestParams &params,
	       http_method_t method,
	       const ResourceAddress &address,
	       StringMap &&headers,
	       const HttpCacheRequestInfo &info,
	       HttpResponseHandler &handler,
	       CancellablePointer &cancel_ptr) noexcept
{
	auto *document = heap.Get(http_cache_key(caller_pool, address), headers);

	if (document == nullptr)
		Miss(caller_pool, parent_stopwatch,
		     params, info,
		     method, address, std::move(headers),
		     handler, cancel_ptr);
	else
		Found(info, *document, caller_pool, parent_stopwatch,
		      params,
		      method, address, std::move(headers),
		      handler, cancel_ptr);
}

inline void
HttpCache::Start(struct pool &caller_pool,
		 const StopwatchPtr &parent_stopwatch,
		 const ResourceRequestParams &params,
		 http_method_t method,
		 const ResourceAddress &address,
		 StringMap &&headers, UnusedIstreamPtr body,
		 HttpResponseHandler &handler,
		 CancellablePointer &cancel_ptr) noexcept
{
	const char *key = http_cache_key(caller_pool, address);
	if (/* this address type cannot be cached; skip the rest of this
	       library */
	    key == nullptr ||
	    /* don't cache a huge request URI; probably it contains lots
	       and lots of unique parameters, and that's not worth the
	       cache space anyway */
	    strlen(key) > 8192) {
		resource_loader.SendRequest(caller_pool, parent_stopwatch,
					    params,
					    method, address,
					    HTTP_STATUS_OK, std::move(headers),
					    std::move(body), nullptr,
					    handler, cancel_ptr);
		return;
	}

	if (auto info = http_cache_request_evaluate(method, address, headers,
						    obey_no_cache, body)) {
		assert(!body);

		Use(caller_pool, parent_stopwatch, params,
		    method, address, std::move(headers), *info,
		    handler, cancel_ptr);
	} else if (params.auto_flush_cache && IsModifyingMethod(method)) {
		LogConcat(4, "HttpCache", "auto_flush? ", key);

		/* TODO merge IsModifyingMethod() and
		   http_cache_request_invalidate()? */
		RemoveURL(key, headers);

		auto request =
			NewFromPool<AutoFlushHttpCacheRequest>(caller_pool,
							       params.cache_tag,
							       *this, handler);
		request->Start(resource_loader, caller_pool, parent_stopwatch,
			       params,
			       method, address,
			       std::move(headers), std::move(body),
			       cancel_ptr);
	} else {
		if (http_cache_request_invalidate(method))
			RemoveURL(key, headers);

		LogConcat(4, "HttpCache", "ignore ", key);

		resource_loader.SendRequest(caller_pool, parent_stopwatch,
					    params,
					    method, address,
					    HTTP_STATUS_OK, std::move(headers),
					    std::move(body), nullptr,
					    handler, cancel_ptr);
	}
}

void
http_cache_request(HttpCache &cache,
		   struct pool &pool,
		   const StopwatchPtr &parent_stopwatch,
		   const ResourceRequestParams &params,
		   http_method_t method,
		   const ResourceAddress &address,
		   StringMap &&headers, UnusedIstreamPtr body,
		   HttpResponseHandler &handler,
		   CancellablePointer &cancel_ptr) noexcept
{
	cache.Start(pool, parent_stopwatch, params,
		    method, address, std::move(headers), std::move(body),
		    handler, cancel_ptr);
}
