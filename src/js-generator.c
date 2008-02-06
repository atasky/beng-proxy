/*
 * Generate JavaScript snippets.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "js-generator.h"
#include "growing-buffer.h"
#include "widget.h"
#include "session.h"

static void
growing_buffer_write_jscript_string(growing_buffer_t gb, const char *s)
{
    if (s == NULL)
        growing_buffer_write_string(gb, "null");
    else {
        growing_buffer_write_string(gb, "\"");
        growing_buffer_write_string(gb, s); /* XXX escape */
        growing_buffer_write_string(gb, "\"");
    }
}

void
js_generate_widget(struct growing_buffer *gb, const struct widget *widget,
                   pool_t pool)
{
    const char *prefix, *parent_prefix;

    if (widget->parent != NULL)
        js_generate_widget(gb, widget->parent, pool);

    prefix = widget_prefix(pool, widget);
    if (prefix == NULL)
        return;

    growing_buffer_write_string(gb, "var ");
    growing_buffer_write_string(gb, prefix);
    growing_buffer_write_string(gb, "widget = ");

    if (widget->parent == NULL) {
        growing_buffer_write_string(gb, "rootWidget;\n");
    } else {
        growing_buffer_write_string(gb, "new beng_widget(");

        parent_prefix = widget_prefix(pool, widget->parent);
        assert(parent_prefix != NULL);

        growing_buffer_write_string(gb, parent_prefix);
        growing_buffer_write_string(gb, "widget, ");
        growing_buffer_write_jscript_string(gb, widget->id);
        growing_buffer_write_string(gb, ");\n");
    }
}

void
js_generate_includes(struct growing_buffer *gb)
{
    /* XXX make the URL configurable */
    growing_buffer_write_string(gb, "<script type=\"text/javascript\" src=\"/cm4all-beng-proxy/beng-proxy.js\"></script>\n"
                                "<script type=\"text/javascript\" src=\"/cm4all-beng-proxy/google-gadget.js\"></script>\n");
}

void
js_generate_root_widget(struct growing_buffer *gb, const char *session_id)
{
    growing_buffer_write_string(gb, "var rootWidget = new beng_root_widget(beng_proxy(\"");

    if (session_id != NULL)
        growing_buffer_write_string(gb, session_id);

    growing_buffer_write_string(gb, "\"));\n");
}

void
js_generate_preferences(struct growing_buffer *gb, const struct widget *widget,
                        pool_t pool)
{
    const char *prefix, *query_string;

    query_string = widget->from_request.session == NULL
        ? NULL
        : widget->from_request.session->query_string;

    prefix = widget_prefix(pool, widget);
    if (prefix == NULL)
        return;

    growing_buffer_write_string(gb, prefix);
    growing_buffer_write_string(gb, "widget._query_string = ");
    growing_buffer_write_jscript_string(gb, query_string);
    growing_buffer_write_string(gb, ";\n");
}

istream_t
js_generate_tail(pool_t pool)
{
    return istream_string_new(pool, "<script type=\"text/javascript\">\n"
                              "_beng_onload();\n"
                              "</script>");
}
