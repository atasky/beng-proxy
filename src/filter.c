/*
 * Filter a resource through an HTTP server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "filter.h"
#include "get.h"
#include "header-parser.h"
#include "strmap.h"

void
filter_new(struct http_cache *cache,
           pool_t pool,
           const struct resource_address *address,
           growing_buffer_t headers,
           istream_t body,
           const struct http_response_handler *handler,
           void *handler_ctx,
           struct async_operation_ref *async_ref)
{
    struct strmap *headers2 = strmap_new(pool, 16);
    header_parse_buffer(pool, headers2, headers);

    resource_get(cache, pool,
                 HTTP_METHOD_POST, address,
                 headers2, body,
                 handler, handler_ctx,
                 async_ref);
}
