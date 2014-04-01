/*
 * High level "Local HTTP" client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "lhttp_request.hxx"
#include "lhttp_stock.hxx"
#include "lhttp_address.h"
#include "http_response.h"
#include "http_client.h"
#include "lease.h"
#include "istream.h"
#include "header-writer.h"

struct lhttp_request {
    struct pool *pool;

    struct lhttp_stock *lhttp_stock;
    struct stock_item *stock_item;
};

/*
 * socket lease
 *
 */

static void
lhttp_socket_release(bool reuse, void *ctx)
{
    struct lhttp_request *request = (struct lhttp_request *)ctx;

    lhttp_stock_put(request->lhttp_stock, request->stock_item, !reuse);
}

static const struct lease lhttp_socket_lease = {
    .release = lhttp_socket_release,
};

/*
 * constructor
 *
 */

void
lhttp_request(struct pool *pool, struct lhttp_stock *lhttp_stock,
              const struct lhttp_address *address,
              http_method_t method,
              struct growing_buffer *headers, struct istream *body,
              const struct http_response_handler *handler, void *handler_ctx,
              struct async_operation_ref *async_ref)
{
    GError *error = nullptr;
    if (!jail_params_check(&address->options.jail, &error)) {
        if (body != nullptr)
            istream_close(body);

        http_response_handler_direct_abort(handler, handler_ctx, error);
        return;
    }

    auto request = NewFromPool<struct lhttp_request>(pool);
    request->pool = pool;
    request->lhttp_stock = lhttp_stock;

    struct stock_item *stock_item =
        lhttp_stock_get(lhttp_stock, pool, address,
                        &error);
    if (stock_item == nullptr) {
        if (body != nullptr)
            istream_close(body);

        http_response_handler_direct_abort(handler, handler_ctx, error);
        return;
    }

    request->stock_item = stock_item;

    if (address->host_and_port != nullptr)
        header_write(headers, "host", address->host_and_port);

    http_client_request(request->pool,
                        lhttp_stock_item_get_socket(stock_item),
                        lhttp_stock_item_get_type(stock_item),
                        &lhttp_socket_lease, request,
                        nullptr, nullptr,
                        method, address->uri, headers, body, true,
                        handler, handler_ctx,
                        async_ref);
}
