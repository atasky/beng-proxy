/*
 * Serve HTTP requests from the local VFS.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "handler.h"
#include "request.h"
#include "connection.h"
#include "url-stream.h"
#include "processor.h"
#include "header-writer.h"
#include "widget.h"
#include "embed.h"
#include "frame.h"
#include "http-util.h"
#include "proxy-widget.h"
#include "session.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct proxy_transfer {
    struct request *request2;
    struct http_server_request *request;
    const struct parsed_uri *external_uri;
    const struct translate_response *tr;
    url_stream_t url_stream;
    struct processor_env env;
};

static const char *const copy_headers[] = {
    "age",
    "etag",
    "content-encoding",
    "content-language",
    "content-md5",
    "content-range",
    "content-type",
    "last-modified",
    "retry-after",
    "vary",
    NULL,
};

static const char *const copy_headers_processed[] = {
    "etag",
    "content-language",
    "content-type",
    "vary",
    NULL,
};


static void
proxy_transfer_close(struct proxy_transfer *pt)
{
    pool_t pool;

    assert(pt->request != NULL);
    assert(pt->request->pool != NULL);

    pool = pt->request->pool;

    if (pt->url_stream != NULL) {
        url_stream_t url_stream = pt->url_stream;
        pt->url_stream = NULL;
        url_stream_close(url_stream);
    }

    pt->request = NULL;
    pool_unref(pool);
}

static void 
proxy_response_response(http_status_t status, strmap_t headers,
                        off_t content_length, istream_t body,
                        void *ctx)
{
    struct proxy_transfer *pt = ctx;
    growing_buffer_t response_headers;

    (void)status;

    assert(pt->url_stream != NULL);
    pt->url_stream = NULL;

    response_headers = growing_buffer_new(pt->request->pool, 2048);

    if (pt->tr->process) {
        struct widget *widget;
        unsigned processor_options = 0;

        /* XXX request body? */
        processor_env_init(pt->request->pool, &pt->env, pt->external_uri,
                           pt->request2->args,
                           pt->request2->session,
                           pt->request->headers,
                           0, NULL,
                           embed_widget_callback);
        if (pt->env.frame != NULL) { /* XXX */
            pt->env.widget_callback = frame_widget_callback;

            /* do not show the template contents if the browser is
               only interested in one particular widget for
               displaying the frame */
            processor_options |= PROCESSOR_QUIET;
        }

        widget = p_malloc(pt->request->pool, sizeof(*widget));
        widget_init(widget, NULL);
        widget->from_request.session = session_get_widget(pt->env.session, pt->external_uri->base, 1);

        pool_ref(pt->request->pool);

        body = processor_new(pt->request->pool, body, widget, &pt->env,
                             processor_options);
        if (pt->env.frame != NULL) {
            /* XXX */
            widget_proxy_install(&pt->env, pt->request, body);
            pool_unref(pt->request->pool);
            proxy_transfer_close(pt);
            return;
        }

#ifndef NO_DEFLATE
        if (http_client_accepts_encoding(pt->request->headers, "deflate")) {
            header_write(response_headers, "content-encoding", "deflate");
            body = istream_deflate_new(pt->request->pool, body);
        }
#endif

        pool_unref(pt->request->pool);

        content_length = (off_t)-1;

        headers_copy(headers, response_headers, copy_headers_processed);
    } else {
        headers_copy(headers, response_headers, copy_headers);
    }

    assert(!istream_has_handler(body));

    http_server_response(pt->request, HTTP_STATUS_OK,
                         response_headers,
                         content_length, body);
}

static void 
proxy_response_free(void *ctx)
{
    struct proxy_transfer *pt = ctx;

    pt->url_stream = NULL;

    proxy_transfer_close(pt);
}

static const struct http_client_response_handler proxy_response_handler = {
    .response = proxy_response_response,
    .free = proxy_response_free,
};


void
proxy_callback(struct request *request2)
{
    struct http_server_request *request = request2->request;
    const struct parsed_uri *external_uri = &request2->uri;
    const struct translate_response *tr = request2->translate.response;
    struct proxy_transfer *pt;
    istream_t body;

    pool_ref(request->pool);

    pt = p_calloc(request->pool, sizeof(*pt));
    pt->request2 = request2;
    pt->request = request;
    pt->external_uri = external_uri;
    pt->tr = tr;

    if (request->body == NULL)
        body = NULL;
    else
        body = istream_hold_new(request->pool, request->body);

    pt->url_stream = url_stream_new(request->pool,
                                    request->method, tr->proxy, NULL,
                                    request->content_length, body,
                                    &proxy_response_handler, pt);
    if (pt->url_stream == NULL) {
        proxy_transfer_close(pt);
        http_server_send_message(request,
                                 HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "Internal server error");
    }
}
