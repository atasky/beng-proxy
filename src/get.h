/*
 * Get resources, either a static file, from a CGI program or from a
 * HTTP server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_GET_H
#define __BENG_GET_H

#include "istream.h"

#include <http/method.h>
#include <http/status.h>

struct pool;
struct http_cache;
struct hstock;
struct tcp_balancer;
struct resource_address;
struct strmap;
struct http_response_handler;
struct async_operation_ref;

/**
 * Requests a resource.  This is a glue function which integrates all
 * client-side protocols implemented by beng-proxy.
 *
 * @param cache a HTTP cache object (optional)
 * @param tcp_balancer the stock (pool) for TCP client connections
 * @param fcgi_stock the stock for FastCGI instances
 * @param was_stock the stock for WAS instances
 * @param delegate_stock the stock for delegate programs
 * @param session_sticky a portion of the session id that is used to
 * select the worker; 0 means disable stickiness
 * @param address the address of the resource
 * @param status a HTTP status code for protocols which do have one
 */
void
resource_get(struct http_cache *cache,
             struct tcp_balancer *tcp_balancer,
             struct hstock *fcgi_stock,
             struct hstock *was_stock,
             struct hstock *delegate_stock,
             struct pool *pool,
             unsigned session_sticky,
             http_method_t method,
             const struct resource_address *address,
             http_status_t status, struct strmap *headers, istream_t body,
             const struct http_response_handler *handler,
             void *handler_ctx,
             struct async_operation_ref *async_ref);

#endif
