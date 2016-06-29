/*
 * Determine the real URI of a widget.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "widget.hxx"
#include "widget_class.hxx"
#include "uri/uri_parser.hxx"
#include "puri_edit.hxx"
#include "puri_relative.hxx"
#include "args.hxx"
#include "tpool.hxx"
#include "http_address.hxx"
#include "lhttp_address.hxx"
#include "cgi_address.hxx"
#include "pool.hxx"
#include "util/StringView.hxx"

#include <assert.h>

/**
 * Returns the "base" address of the widget, i.e. without the widget
 * parameters from the parent container.
 */
static ResourceAddress
widget_base_address(struct pool *pool, Widget *widget, bool stateful)
{
    const ResourceAddress *src = stateful
        ? widget_address(widget) : widget_stateless_address(widget);
    const char *uri;

    if (!src->IsHttp() || widget->from_template.query_string == nullptr)
        return *src;

    const auto &src_http = src->GetHttp();
    const char *const src_path = src_http.path;

    uri = uri_delete_query_string(pool, src_path,
                                  widget->from_template.query_string);

    if (!widget->from_request.query_string.IsEmpty())
        uri = uri_delete_query_string(pool, src_path,
                                      widget->from_request.query_string);

    if (uri == src_path)
        return *src;

    return src->WithPath(*pool, uri);
}

static const ResourceAddress *
widget_get_original_address(const Widget *widget)
{
    assert(widget != nullptr);
    assert(widget->cls != nullptr);

    const WidgetView *view = widget->GetAddressView();
    assert(view != nullptr);

    return &view->address;
}

gcc_pure
static bool
HasTrailingSlash(const char *p)
{
    size_t length = strlen(p);
    return length > 0 && p[length - 1] == '/';
}

const ResourceAddress *
widget_determine_address(const Widget *widget, bool stateful)
{
    struct pool *pool = widget->pool;
    const char *path_info, *uri;
    ResourceAddress *address;

    assert(widget != nullptr);
    assert(widget->cls != nullptr);

    path_info = widget->GetPathInfo(stateful);
    assert(path_info != nullptr);

    const ResourceAddress *original_address =
        widget_get_original_address(widget);
    switch (original_address->type) {
        CgiAddress *cgi;

    case ResourceAddress::Type::NONE:
    case ResourceAddress::Type::LOCAL:
    case ResourceAddress::Type::PIPE:
    case ResourceAddress::Type::NFS:
        break;

    case ResourceAddress::Type::HTTP:
        assert(original_address->GetHttp().path != nullptr);

        if ((!stateful || widget->from_request.query_string.IsEmpty()) &&
            *path_info == 0 &&
            widget->from_template.query_string == nullptr)
            break;

        uri = original_address->GetHttp().path;

        if (*path_info != 0) {
            if (*path_info == '/' && HasTrailingSlash(uri))
                /* avoid generating a double slash when concatenating
                   URI path and path_info */
                ++path_info;

            uri = p_strcat(pool, uri, path_info, nullptr);
        }

        if (widget->from_template.query_string != nullptr)
            uri = uri_insert_query_string(pool, uri,
                                          widget->from_template.query_string);

        if (stateful && !widget->from_request.query_string.IsEmpty())
            uri = uri_append_query_string_n(pool, uri,
                                            widget->from_request.query_string);

        return NewFromPool<ResourceAddress>(*pool, original_address->WithPath(*pool, uri));

    case ResourceAddress::Type::LHTTP:
        assert(original_address->GetLhttp().uri != nullptr);

        if ((!stateful || widget->from_request.query_string.IsEmpty()) &&
            *path_info == 0 &&
            widget->from_template.query_string == nullptr)
            break;

        uri = original_address->GetLhttp().uri;

        if (*path_info != 0) {
            if (*path_info == '/' && HasTrailingSlash(uri))
                /* avoid generating a double slash when concatenating
                   URI path and path_info */
                ++path_info;

            uri = p_strcat(pool, uri, path_info, nullptr);
        }

        if (widget->from_template.query_string != nullptr)
            uri = uri_insert_query_string(pool, uri,
                                          widget->from_template.query_string);

        if (stateful && !widget->from_request.query_string.IsEmpty())
            uri = uri_append_query_string_n(pool, uri,
                                            widget->from_request.query_string);

        return NewFromPool<ResourceAddress>(*pool, original_address->WithPath(*pool, uri));

    case ResourceAddress::Type::CGI:
    case ResourceAddress::Type::FASTCGI:
    case ResourceAddress::Type::WAS:
        if ((!stateful || widget->from_request.query_string.IsEmpty()) &&
            *path_info == 0 &&
            widget->from_template.query_string == nullptr)
            break;

        address = original_address->Dup(*pool);
        cgi = &address->GetCgi();

        if (*path_info != 0)
            cgi->path_info = cgi->path_info != nullptr
                ? uri_absolute(pool, cgi->path_info, path_info)
                : path_info;

        if (!stateful || widget->from_request.query_string.IsEmpty())
            cgi->query_string = widget->from_template.query_string;
        else if (widget->from_template.query_string == nullptr)
            cgi->query_string = p_strdup(*pool,
                                         widget->from_request.query_string);
        else
            cgi->query_string =
                p_strncat(pool,
                          widget->from_request.query_string.data,
                          widget->from_request.query_string.size,
                          "&", (size_t)1,
                          widget->from_template.query_string,
                          strlen(widget->from_template.query_string),
                          nullptr);

        return address;
    }

    return original_address;
}

const char *
widget_absolute_uri(struct pool *pool, Widget *widget, bool stateful,
                    StringView relative_uri)
{
    assert(widget_address(widget)->IsHttp());

    if (relative_uri.StartsWith({"~/", 2})) {
        relative_uri.skip_front(2);
        stateful = false;
    } else if (!relative_uri.IsEmpty() && relative_uri.front() == '/' &&
               widget->cls != nullptr && widget->cls->anchor_absolute) {
        relative_uri.skip_front(1);
        stateful = false;
    }

    const auto *uwa =
        &(stateful
          ? widget_address(widget)
          : widget_stateless_address(widget))->GetHttp();
    const char *base = uwa->path;
    if (relative_uri.IsNull())
        return uwa->GetAbsoluteURI(pool);

    const char *uri = uri_absolute(pool, base, relative_uri);
    assert(uri != nullptr);
    if (!relative_uri.IsEmpty() &&
        widget->from_template.query_string != nullptr)
        /* the relative_uri is non-empty, and uri_absolute() has
           removed the query string: re-add the configured query
           string */
        uri = uri_insert_query_string(pool, uri,
                                      widget->from_template.query_string);

    return uwa->GetAbsoluteURI(pool, uri);
}

StringView
widget_relative_uri(struct pool *pool, Widget *widget, bool stateful,
                    StringView relative_uri)
{
    const ResourceAddress *base;
    if (relative_uri.size >= 2 && relative_uri[0] == '~' &&
        relative_uri[1] == '/') {
        relative_uri.skip_front(2);
        base = widget_get_original_address(widget);
    } else if (relative_uri.size >= 1 && relative_uri[0] == '/' &&
               widget->cls != nullptr && widget->cls->anchor_absolute) {
        relative_uri.skip_front(1);
        base = widget_get_original_address(widget);
    } else
        base = NewFromPool<ResourceAddress>(*pool, widget_base_address(pool, widget, stateful));

    ResourceAddress address_buffer;
    const auto address = base->Apply(*pool, relative_uri, address_buffer);
    if (address == nullptr)
        return nullptr;

    const ResourceAddress *original_address =
        widget_get_original_address(widget);
    return address->RelativeTo(*original_address);
}

/**
 * Returns true when the widget has the specified widget path.
 *
 * @param other the path to compare with; may be nullptr (i.e. never
 * matches)
 */
gcc_pure
static bool
compare_widget_path(const Widget *widget, const char *other)
{
    assert(widget != nullptr);

    if (other == nullptr)
        return false;

    const char *path = widget->GetIdPath();
    if (path == nullptr)
        return false;

    return strcmp(path, other) == 0;
}

const char *
widget_external_uri(struct pool *pool,
                    const struct parsed_uri *external_uri,
                    StringMap *args,
                    Widget *widget, bool stateful,
                    StringView relative_uri,
                    const char *frame, const char *view)
{
    const char *qmark, *args2, *new_uri;
    StringView p;

    const char *path = widget->GetIdPath();
    if (path == nullptr ||
        external_uri == nullptr ||
        widget->cls == &root_widget_class)
        return nullptr;

    const AutoRewindPool auto_rewind(*tpool);

    if (!relative_uri.IsNull()) {
        p = widget_relative_uri(tpool, widget, stateful, relative_uri);
        if (p.IsNull())
            return nullptr;
    } else
        p = nullptr;

    if (!p.IsNull() && relative_uri.Find('?') == nullptr &&
        widget->from_template.query_string != nullptr) {
        /* no query string in relative_uri: if there is one in the new
           URI, check it and remove the configured parameters */
        const char *uri =
            uri_delete_query_string(tpool, p_strdup(*tpool, p),
                                    widget->from_template.query_string);
        p = uri;
    }

    StringView query_string;
    if (!p.IsNull() && (qmark = p.Find('?')) != nullptr) {
        /* separate query_string from path_info */
        query_string = { qmark, p.end() };
        p.size = qmark - p.data;
    } else {
        query_string = nullptr;
    }

    StringView suffix;
    if (!p.IsNull() && widget->cls->direct_addressing &&
        compare_widget_path(widget, frame)) {
        /* new-style direct URI addressing: append */
        suffix = p;
        p = nullptr;
    } else
        suffix.SetEmpty();

    /* the URI is relative to the widget's base URI.  Convert the URI
       into an absolute URI to the template page on this server and
       add the appropriate args. */
    args2 = args_format_n(tpool, args,
                          "focus", path,
                          p.IsNull() ? nullptr : "path", p,
                          frame == nullptr ? nullptr : "frame", frame,
                          nullptr);

    new_uri = p_strncat(pool,
                        external_uri->base.data,
                        external_uri->base.size,
                        ";", (size_t)1,
                        args2, strlen(args2),
                        "&view=", (size_t)(view != nullptr ? 6 : 0),
                        view != nullptr ? view : "",
                        view != nullptr ? strlen(view) : (size_t)0,
                        "/", (size_t)(suffix.size > 0),
                        suffix.data, suffix.size,
                        query_string.data, query_string.size,
                        nullptr);
    return new_uri;
}
