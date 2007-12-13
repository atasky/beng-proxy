/*
 * Embed a processed HTML document
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_EMBED_H
#define __BENG_EMBED_H

#include "istream.h"
#include "http.h"

struct widget;
struct processor_env;

istream_t
embed_new(pool_t pool, http_method_t method, const char *url,
          istream_t request_body,
          struct widget *widget,
          struct processor_env *env,
          unsigned options);

istream_t
embed_widget_callback(pool_t pool, struct processor_env *env,
                      struct widget *widget);

#endif
