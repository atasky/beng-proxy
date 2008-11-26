/*
 * Call the translation server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_TRANSLATE_H
#define __BENG_TRANSLATE_H

#include "pool.h"
#include "http.h"
#include "resource-address.h"

struct uri_with_address;
struct hstock;
struct async_operation_ref;

struct translate_request {
    const char *remote_host;
    const char *host;
    const char *uri;
    const char *widget_type;
    const char *session;
    const char *param;
};

struct translate_transformation {
    struct translate_transformation *next;

    enum {
        TRANSFORMATION_PROCESS,
        TRANSFORMATION_FILTER,
    } type;

    union {
        struct {
            unsigned options;

            const char *domain;
        } processor;

        struct resource_address filter;
    } u;
};

struct translate_response {
    http_status_t status;

    struct resource_address address;

    const char *site;
    const char *document_root;
    const char *redirect;
    bool stateful;
    const char *session;
    const char *user;
    const char *language;

    struct translate_transformation *transformation;
};

typedef void (*translate_callback_t)(const struct translate_response *response,
                                     void *ctx);

void
translate(pool_t pool,
          struct hstock *tcp_stock, const char *socket_path,
          const struct translate_request *request,
          translate_callback_t callback,
          void *ctx,
          struct async_operation_ref *async_ref);

struct translate_transformation *
transformation_dup(pool_t pool, const struct translate_transformation *src);

#endif
