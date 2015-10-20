/*
 * Process CM4all commands in a HTML stream, e.g. embeddings.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "processor.h"
#include "text_processor.hxx"
#include "css_processor.h"
#include "css_rewrite.hxx"
#include "penv.hxx"
#include "xml_parser.hxx"
#include "uri/uri_escape.hxx"
#include "uri/uri_extract.hxx"
#include "widget.hxx"
#include "widget_approval.hxx"
#include "widget_request.hxx"
#include "widget_lookup.hxx"
#include "widget_class.hxx"
#include "widget-quark.h"
#include "growing_buffer.hxx"
#include "tpool.hxx"
#include "inline_widget.hxx"
#include "async.hxx"
#include "rewrite_uri.hxx"
#include "bp_global.hxx"
#include "expansible_buffer.hxx"
#include "escape_class.hxx"
#include "escape_html.hxx"
#include "strmap.hxx"
#include "css_syntax.hxx"
#include "css_util.hxx"
#include "istream_html_escape.hxx"
#include "istream/istream.hxx"
#include "istream/istream_internal.hxx"
#include "istream/istream_replace.hxx"
#include "istream/istream_cat.hxx"
#include "istream/istream_catch.hxx"
#include "istream/istream_memory.hxx"
#include "istream/istream_string.hxx"
#include "istream/istream_tee.hxx"
#include "pool.hxx"
#include "util/Cast.hxx"
#include "util/CharUtil.hxx"
#include "util/Macros.hxx"
#include "util/StringView.hxx"

#include <daemon/log.h>

#include <glib.h>

#include <assert.h>
#include <string.h>

enum uri_base {
    URI_BASE_TEMPLATE,
    URI_BASE_WIDGET,
    URI_BASE_CHILD,
    URI_BASE_PARENT,
};

struct uri_rewrite {
    enum uri_base base;
    enum uri_mode mode;

    char view[64];
};

enum tag {
    TAG_NONE,
    TAG_IGNORE,
    TAG_OTHER,
    TAG_WIDGET,
    TAG_WIDGET_PATH_INFO,
    TAG_WIDGET_PARAM,
    TAG_WIDGET_HEADER,
    TAG_WIDGET_VIEW,
    TAG_A,
    TAG_FORM,
    TAG_IMG,
    TAG_SCRIPT,
    TAG_PARAM,
    TAG_REWRITE_URI,

    /**
     * The "meta" element.  This may morph into #TAG_META_REFRESH when
     * an http-equiv="refresh" attribute is found.
     */
    TAG_META,

    TAG_META_REFRESH,

    /**
     * The "style" element.  This value later morphs into
     * #TAG_STYLE_PROCESS if #PROCESSOR_STYLE is enabled.
     */
    TAG_STYLE,

    /**
     * Only used when #PROCESSOR_STYLE is enabled.  If active, then
     * CDATA is being fed into the CSS processor.
     */
    TAG_STYLE_PROCESS,
};

struct XmlProcessor {
    struct pool *pool, *caller_pool;

    struct widget *container;
    const char *lookup_id;
    struct processor_env *env;
    unsigned options;

    struct istream *replace;

    XmlParser *parser;
    bool had_input;

    enum tag tag;

    struct uri_rewrite uri_rewrite;

    /**
     * The default value for #uri_rewrite.
     */
    struct uri_rewrite default_uri_rewrite;

    /**
     * A buffer that may be used for various temporary purposes
     * (e.g. attribute transformation).
     */
    struct expansible_buffer *buffer;

    /**
     * These values are used to buffer c:mode/c:base values in any
     * order, even after the actual URI attribute.
     */
    struct {
        bool pending;

        off_t uri_start, uri_end;
        struct expansible_buffer *value;

        /**
         * The positions of the c:mode/c:base attributes after the URI
         * attribute.  These have to be deleted *after* the URI
         * attribute has been rewritten.
         */
        struct {
            off_t start, end;
        } delete_[4];
    } postponed_rewrite;

    struct {
        off_t start_offset;

        struct pool *pool;
        struct widget *widget;

        struct {
            struct expansible_buffer *name;
            struct expansible_buffer *value;
        } param;

        struct expansible_buffer *params;
    } widget;

    /**
     * Only valid if #cdata_stream_active is true.
     */
    off_t cdata_start;
    struct istream cdata_stream;

    struct async_operation async;

    const struct widget_lookup_handler *handler;
    void *handler_ctx;

    struct async_operation_ref *async_ref;
};

bool
processable(const struct strmap *headers)
{
    const char *content_type;

    content_type = strmap_get_checked(headers, "content-type");
    return content_type != nullptr &&
        (strncmp(content_type, "text/html", 9) == 0 ||
         strncmp(content_type, "text/xml", 8) == 0 ||
         strncmp(content_type, "application/xml", 15) == 0 ||
         strncmp(content_type, "application/xhtml+xml", 21) == 0);
}

static inline bool
processor_option_quiet(const XmlProcessor *processor)
{
    return processor->replace == nullptr;
}

static inline bool
processor_option_rewrite_url(const XmlProcessor *processor)
{
    return (processor->options & PROCESSOR_REWRITE_URL) != 0;
}

static inline bool
processor_option_prefix_class(const XmlProcessor *processor)
{
    return (processor->options & PROCESSOR_PREFIX_CSS_CLASS) != 0;
}

static inline bool
processor_option_prefix_id(const XmlProcessor *processor)
{
    return (processor->options & PROCESSOR_PREFIX_XML_ID) != 0;
}

static inline bool
processor_option_prefix(const XmlProcessor *processor)
{
    return (processor->options & (PROCESSOR_PREFIX_CSS_CLASS|PROCESSOR_PREFIX_XML_ID)) != 0;
}

static inline bool
processor_option_style(const XmlProcessor *processor)
{
    return (processor->options & PROCESSOR_STYLE) != 0;
}

static void
processor_replace_add(XmlProcessor *processor, off_t start, off_t end,
                      struct istream *istream)
{
    istream_replace_add(processor->replace, start, end, istream);
}


/*
 * async operation
 *
 */

static XmlProcessor *
async_to_processor(struct async_operation *ao)
{
    return &ContainerCast2(*ao, &XmlProcessor::async);
}

static void
processor_async_abort(struct async_operation *ao)
{
    auto *processor = async_to_processor(ao);
    struct pool *const widget_pool = processor->container->pool;

    if (processor->container->for_focused.body != nullptr)
        /* the request body was not yet submitted to the focused
           widget; dispose it now */
        istream_free_unused(&processor->container->for_focused.body);

    pool_unref(widget_pool);
    pool_unref(processor->caller_pool);

    if (processor->parser != nullptr)
        parser_close(processor->parser);
}

static const struct async_operation_class processor_async_operation = {
    .abort = processor_async_abort,
};


/*
 * constructor
 *
 */

static void
processor_parser_init(XmlProcessor *processor, struct istream *input);

static XmlProcessor *
processor_new(struct pool *caller_pool,
              struct widget *widget,
              struct processor_env *env,
              unsigned options)
{
    assert(widget != nullptr);

    struct pool *pool = pool_new_linear(caller_pool, "processor", 32768);

    auto processor = NewFromPool<XmlProcessor>(*pool);
    processor->pool = pool;
    processor->caller_pool = caller_pool;

    processor->widget.pool = env->pool;

    processor->container = widget;
    pool_ref(processor->container->pool);

    processor->env = env;
    processor->options = options;
    processor->tag = TAG_NONE;

    processor->buffer = expansible_buffer_new(pool, 128, 2048);

    processor->postponed_rewrite.pending = false;
    processor->postponed_rewrite.value =
        expansible_buffer_new(pool, 1024, 8192);

    processor->widget.widget = nullptr;
    processor->widget.param.name = expansible_buffer_new(pool, 128, 512);
    processor->widget.param.value = expansible_buffer_new(pool, 512, 4096);
    processor->widget.params = expansible_buffer_new(pool, 1024, 8192);

    return processor;
}

struct istream *
processor_process(struct pool *caller_pool, struct istream *istream,
                  struct widget *widget,
                  struct processor_env *env,
                  unsigned options)
{
    assert(istream != nullptr);
    assert(!istream_has_handler(istream));

    auto *processor = processor_new(caller_pool, widget, env, options);
    processor->lookup_id = nullptr;

    /* the text processor will expand entities */
    istream = text_processor(processor->pool, istream, widget, env);

    struct istream *tee = istream_tee_new(processor->pool, istream,
                                          true, true);
    istream = istream_tee_second(tee);
    processor->replace = istream_replace_new(processor->pool, tee);

    processor_parser_init(processor, istream);
    pool_unref(processor->pool);

    if (processor_option_rewrite_url(processor)) {
        processor->default_uri_rewrite.base = URI_BASE_TEMPLATE;
        processor->default_uri_rewrite.mode = URI_MODE_PARTIAL;
        processor->default_uri_rewrite.view[0] = 0;

        if (options & PROCESSOR_FOCUS_WIDGET) {
            processor->default_uri_rewrite.base = URI_BASE_WIDGET;
            processor->default_uri_rewrite.mode = URI_MODE_FOCUS;
        }
    }

    //XXX headers = processor_header_forward(pool, headers);
    return processor->replace;
}

void
processor_lookup_widget(struct pool *caller_pool,
                        struct istream *istream,
                        struct widget *widget, const char *id,
                        struct processor_env *env,
                        unsigned options,
                        const struct widget_lookup_handler *handler,
                        void *handler_ctx,
                        struct async_operation_ref *async_ref)
{
    assert(istream != nullptr);
    assert(!istream_has_handler(istream));
    assert(widget != nullptr);
    assert(id != nullptr);

    if ((options & PROCESSOR_CONTAINER) == 0) {
        GError *error =
            g_error_new_literal(widget_quark(), WIDGET_ERROR_NOT_A_CONTAINER,
                                "Not a container");
        handler->error(error, handler_ctx);
        return;
    }

    auto *processor = processor_new(caller_pool, widget, env, options);

    processor->lookup_id = id;

    processor->replace = nullptr;

    processor_parser_init(processor, istream);

    processor->handler = handler;
    processor->handler_ctx = handler_ctx;

    pool_ref(caller_pool);

    processor->async.Init(processor_async_operation);
    async_ref->Set(processor->async);
    processor->async_ref = async_ref;

    do {
        processor->had_input = false;
        parser_read(processor->parser);
    } while (processor->had_input && processor->parser != nullptr);

    pool_unref(processor->pool);
}

static void
processor_uri_rewrite_init(XmlProcessor *processor)
{
    assert(!processor->postponed_rewrite.pending);

    processor->uri_rewrite = processor->default_uri_rewrite;
}

static void
processor_uri_rewrite_postpone(XmlProcessor *processor,
                               off_t start, off_t end,
                               const void *value, size_t length)
{
    assert(start <= end);

    if (processor->postponed_rewrite.pending)
        /* cannot rewrite more than one attribute per element */
        return;

    /* postpone the URI rewrite until the tag is finished: save the
       attribute value position, save the original attribute value and
       set the "pending" flag */

    processor->postponed_rewrite.uri_start = start;
    processor->postponed_rewrite.uri_end = end;

    bool success = expansible_buffer_set(processor->postponed_rewrite.value,
                                         value, length);

    for (unsigned i = 0; i < ARRAY_SIZE(processor->postponed_rewrite.delete_); ++i)
        processor->postponed_rewrite.delete_[i].start = 0;
    processor->postponed_rewrite.pending = success;
}

static void
processor_uri_rewrite_delete(XmlProcessor *processor,
                             off_t start, off_t end)
{
    unsigned i = 0;

    if (!processor->postponed_rewrite.pending) {
        /* no URI attribute found yet: delete immediately */
        istream_replace_add(processor->replace, start, end, nullptr);
        return;
    }

    /* find a free position in the "delete" array */

    while (processor->postponed_rewrite.delete_[i].start > 0) {
        ++i;
        if (i >= ARRAY_SIZE(processor->postponed_rewrite.delete_))
            /* no more room in the array */
            return;
    }

    /* postpone the delete until the URI attribute has been replaced */

    processor->postponed_rewrite.delete_[i].start = start;
    processor->postponed_rewrite.delete_[i].end = end;
}

static void
transform_uri_attribute(XmlProcessor *processor,
                        const XmlParserAttribute *attr,
                        enum uri_base base,
                        enum uri_mode mode,
                        const char *view);

static void
processor_uri_rewrite_attribute(XmlProcessor *processor,
                                const XmlParserAttribute *attr)
{
    processor_uri_rewrite_postpone(processor,
                                   attr->value_start, attr->value_end,
                                   attr->value.data, attr->value.size);
}

static void
processor_uri_rewrite_refresh_attribute(XmlProcessor *processor,
                                        const XmlParserAttribute *attr)
{
    const auto end = attr->value.end();
    const char *p = attr->value.Find(';');
    if (p == nullptr || p + 7 > end || memcmp(p + 1, "URL='", 5) != 0 ||
        end[-1] != '\'')
        return;

    p += 6;

    /* postpone the URI rewrite until the tag is finished: save the
       attribute value position, save the original attribute value and
       set the "pending" flag */

    processor_uri_rewrite_postpone(processor,
                                   attr->value_start + (p - attr->value.data),
                                   attr->value_end - 1,
                                   p, end - 1 - p);
}

static void
processor_uri_rewrite_commit(XmlProcessor *processor)
{
    XmlParserAttribute uri_attribute;
    uri_attribute.value_start = processor->postponed_rewrite.uri_start;
    uri_attribute.value_end = processor->postponed_rewrite.uri_end;

    assert(processor->postponed_rewrite.pending);

    processor->postponed_rewrite.pending = false;

    /* rewrite the URI */

    uri_attribute.value = expansible_buffer_read_string_view(processor->postponed_rewrite.value);
    transform_uri_attribute(processor, &uri_attribute,
                            processor->uri_rewrite.base,
                            processor->uri_rewrite.mode,
                            processor->uri_rewrite.view[0] != 0
                            ? processor->uri_rewrite.view : nullptr);

    /* now delete all c:base/c:mode attributes which followed the
       URI */

    for (unsigned i = 0; i < ARRAY_SIZE(processor->postponed_rewrite.delete_); ++i)
        if (processor->postponed_rewrite.delete_[i].start > 0)
            istream_replace_add(processor->replace,
                                processor->postponed_rewrite.delete_[i].start,
                                processor->postponed_rewrite.delete_[i].end,
                                nullptr);
}

/*
 * CDATA istream
 *
 */

static void
processor_stop_cdata_stream(XmlProcessor *processor)
{
    if (processor->tag != TAG_STYLE_PROCESS)
        return;

    istream_deinit_eof(&processor->cdata_stream);
    processor->tag = TAG_STYLE;
}

static inline XmlProcessor *
cdata_stream_to_processor(struct istream *istream)
{
    return &ContainerCast2(*istream, &XmlProcessor::cdata_stream);
}

static void
processor_cdata_read(struct istream *istream)
{
    auto *processor = cdata_stream_to_processor(istream);
    assert(processor->tag == TAG_STYLE_PROCESS);

    parser_read(processor->parser);
}

static void
processor_cdata_close(struct istream *istream)
{
    auto *processor = cdata_stream_to_processor(istream);
    assert(processor->tag == TAG_STYLE_PROCESS);

    istream_deinit(&processor->cdata_stream);
    processor->tag = TAG_STYLE;
}

static const struct istream_class processor_cdata_istream = {
    .available = nullptr,
    .skip = nullptr,
    .read = processor_cdata_read,
    .as_fd = nullptr,
    .close = processor_cdata_close,
};


/*
 * parser callbacks
 *
 */

static bool
processor_processing_instruction(XmlProcessor *processor,
                                 StringView name)
{
    if (!processor_option_quiet(processor) &&
        processor_option_rewrite_url(processor) &&
        name.EqualsLiteral("cm4all-rewrite-uri")) {
        processor->tag = TAG_REWRITE_URI;
        processor_uri_rewrite_init(processor);
        return true;
    }

    return false;
}

static bool
parser_element_start_in_widget(XmlProcessor *processor,
                               XmlParserTagType type,
                               StringView name)
{
    if (type == TAG_PI)
        return processor_processing_instruction(processor, name);

    if (name.StartsWith({"c:", 2}))
        name.skip_front(2);

    if (name.EqualsLiteral("widget")) {
        if (type == TAG_CLOSE)
            processor->tag = TAG_WIDGET;
    } else if (name.EqualsLiteral("path-info")) {
        processor->tag = TAG_WIDGET_PATH_INFO;
    } else if (name.EqualsLiteral("param") ||
               name.EqualsLiteral("parameter")) {
        processor->tag = TAG_WIDGET_PARAM;
        expansible_buffer_reset(processor->widget.param.name);
        expansible_buffer_reset(processor->widget.param.value);
    } else if (name.EqualsLiteral("header")) {
        processor->tag = TAG_WIDGET_HEADER;
        expansible_buffer_reset(processor->widget.param.name);
        expansible_buffer_reset(processor->widget.param.value);
    } else if (name.EqualsLiteral("view")) {
        processor->tag = TAG_WIDGET_VIEW;
    } else {
        processor->tag = TAG_IGNORE;
        return false;
    }

    return true;
}

static bool
processor_parser_tag_start(const XmlParserTag *tag, void *ctx)
{
    auto *processor = (XmlProcessor *)ctx;

    processor->had_input = true;

    processor_stop_cdata_stream(processor);

    if (processor->tag == TAG_SCRIPT &&
        !tag->name.EqualsLiteralIgnoreCase("script"))
        /* workaround for bugged scripts: ignore all closing tags
           except </SCRIPT> */
        return false;

    processor->tag = TAG_IGNORE;

    if (processor->widget.widget != nullptr)
        return parser_element_start_in_widget(processor, tag->type, tag->name);

    if (tag->type == TAG_PI)
        return processor_processing_instruction(processor, tag->name);

    if (tag->name.EqualsLiteral("c:widget")) {
        if ((processor->options & PROCESSOR_CONTAINER) == 0 ||
            global_translate_cache == nullptr)
            return false;

        if (tag->type == TAG_CLOSE) {
            assert(processor->widget.widget == nullptr);
            return false;
        }

        processor->tag = TAG_WIDGET;
        processor->widget.widget = NewFromPool<widget>(*processor->widget.pool);
        processor->widget.widget->Init(*processor->widget.pool, nullptr);
        expansible_buffer_reset(processor->widget.params);

        processor->widget.widget->parent = processor->container;

        return true;
    } else if (tag->name.EqualsLiteralIgnoreCase("script")) {
        processor->tag = TAG_SCRIPT;
        processor_uri_rewrite_init(processor);

        return true;
    } else if (!processor_option_quiet(processor) &&
               processor_option_style(processor) &&
               tag->name.EqualsLiteralIgnoreCase("style")) {
        processor->tag = TAG_STYLE;
        return true;
    } else if (!processor_option_quiet(processor) &&
               processor_option_rewrite_url(processor)) {
        if (tag->name.EqualsLiteralIgnoreCase("a")) {
            processor->tag = TAG_A;
            processor_uri_rewrite_init(processor);
            return true;
        } else if (tag->name.EqualsLiteralIgnoreCase("link")) {
            /* this isn't actually an anchor, but we are only interested in
               the HREF attribute */
            processor->tag = TAG_A;
            processor_uri_rewrite_init(processor);
            return true;
        } else if (tag->name.EqualsLiteralIgnoreCase("form")) {
            processor->tag = TAG_FORM;
            processor_uri_rewrite_init(processor);
            return true;
        } else if (tag->name.EqualsLiteralIgnoreCase("img")) {
            processor->tag = TAG_IMG;
            processor_uri_rewrite_init(processor);
            return true;
        } else if (tag->name.EqualsLiteralIgnoreCase("iframe") ||
                   tag->name.EqualsLiteralIgnoreCase("embed") ||
                   tag->name.EqualsLiteralIgnoreCase("video") ||
                   tag->name.EqualsLiteralIgnoreCase("audio")) {
            /* this isn't actually an IMG, but we are only interested
               in the SRC attribute */
            processor->tag = TAG_IMG;
            processor_uri_rewrite_init(processor);
            return true;
        } else if (tag->name.EqualsLiteralIgnoreCase("param")) {
            processor->tag = TAG_PARAM;
            processor_uri_rewrite_init(processor);
            return true;
        } else if (tag->name.EqualsLiteralIgnoreCase("meta")) {
            processor->tag = TAG_META;
            processor_uri_rewrite_init(processor);
            return true;
        } else if (processor_option_prefix(processor)) {
            processor->tag = TAG_OTHER;
            return true;
        } else {
            processor->tag = TAG_IGNORE;
            return false;
        }
    } else if (processor_option_prefix(processor)) {
        processor->tag = TAG_OTHER;
        return true;
    } else {
        processor->tag = TAG_IGNORE;
        return false;
    }
}

static void
replace_attribute_value(XmlProcessor *processor,
                        const XmlParserAttribute *attr,
                        struct istream *value)
{
    processor_replace_add(processor,
                          attr->value_start, attr->value_end,
                          value);
}

static void
SplitString(StringView in, char separator,
            StringView &before, StringView &after)
{
    const char *x = in.Find(separator);

    if (x != nullptr) {
        before = {in.data, x};
        after = {x + 1, in.end()};
    } else {
        before = in;
        after = nullptr;
    }
}

static void
transform_uri_attribute(XmlProcessor *processor,
                        const XmlParserAttribute *attr,
                        enum uri_base base,
                        enum uri_mode mode,
                        const char *view)
{
    StringView value = attr->value;
    if (value.StartsWith({"mailto:", 7}))
        /* ignore email links */
        return;

    if (uri_has_authority(value))
        /* can't rewrite if the specified URI is absolute */
        return;

    struct widget *widget = nullptr;
    StringView child_id, suffix;
    struct istream *istream;

    switch (base) {
    case URI_BASE_TEMPLATE:
        /* no need to rewrite the attribute */
        return;

    case URI_BASE_WIDGET:
        widget = processor->container;
        break;

    case URI_BASE_CHILD:
        SplitString(value, '/', child_id, suffix);

        widget = processor->container->FindChild(p_strdup(*processor->pool,
                                                          child_id));
        if (widget == nullptr)
            return;

        value = suffix;
        break;

    case URI_BASE_PARENT:
        widget = processor->container->parent;
        if (widget == nullptr)
            return;

        break;
    }

    assert(widget != nullptr);

    if (widget->cls == nullptr && widget->class_name == nullptr)
        return;

    const char *hash = value.Find('#');
    StringView fragment;
    if (hash != nullptr) {
        /* save the unescaped fragment part of the URI, don't pass it
           to rewrite_widget_uri() */
        fragment = {hash, value.end()};
        value = {value.data, hash};
    } else
        fragment = nullptr;

    istream = rewrite_widget_uri(*processor->pool, *processor->env->pool,
                                 *processor->env,
                                 *global_translate_cache,
                                 *widget,
                                 value, mode, widget == processor->container,
                                 view,
                                 &html_escape_class);
    if (istream == nullptr)
        return;

    if (!fragment.IsEmpty()) {
        /* escape and append the fragment to the new URI */
        struct istream *s = istream_memory_new(processor->pool,
                                               p_strdup(*processor->pool,
                                                        fragment),
                                               fragment.size);
        s = istream_html_escape_new(processor->pool, s);

        istream = istream_cat_new(processor->pool, istream, s, nullptr);
    }

    replace_attribute_value(processor, attr, istream);
}

static void
parser_widget_attr_finished(struct widget *widget,
                            StringView name, StringView value)
{
    if (name.EqualsLiteral("type")) {
        widget->SetClassName(value);
    } else if (name.EqualsLiteral("id")) {
        if (!value.IsEmpty())
            widget->SetId(value);
    } else if (name.EqualsLiteral("display")) {
        if (value.EqualsLiteral("inline"))
            widget->display = widget::WIDGET_DISPLAY_INLINE;
        else if (value.EqualsLiteral("none"))
            widget->display = widget::WIDGET_DISPLAY_NONE;
        else
            widget->display = widget::WIDGET_DISPLAY_NONE;
    } else if (name.EqualsLiteral("session")) {
        if (value.EqualsLiteral("resource"))
            widget->session = widget::WIDGET_SESSION_RESOURCE;
        else if (value.EqualsLiteral("site"))
            widget->session = widget::WIDGET_SESSION_SITE;
    }
}

gcc_pure
static enum uri_base
parse_uri_base(StringView s)
{
    if (s.EqualsLiteral("widget"))
        return URI_BASE_WIDGET;
    else if (s.EqualsLiteral("child"))
        return URI_BASE_CHILD;
    else if (s.EqualsLiteral("parent"))
        return URI_BASE_PARENT;
    else
        return URI_BASE_TEMPLATE;
}

static bool
link_attr_finished(XmlProcessor *processor, const XmlParserAttribute *attr)
{
    if (attr->name.EqualsLiteral("c:base")) {
        processor->uri_rewrite.base = parse_uri_base(attr->value);

        if (processor->tag != TAG_REWRITE_URI)
            processor_uri_rewrite_delete(processor, attr->name_start, attr->end);
        return true;
    }

    if (attr->name.EqualsLiteral("c:mode")) {
        processor->uri_rewrite.mode = parse_uri_mode(attr->value);

        if (processor->tag != TAG_REWRITE_URI)
            processor_uri_rewrite_delete(processor,
                                         attr->name_start, attr->end);
        return true;
    }

    if (attr->name.EqualsLiteral("c:view") &&
        attr->value.size < sizeof(processor->uri_rewrite.view)) {
        memcpy(processor->uri_rewrite.view,
               attr->value.data, attr->value.size);
        processor->uri_rewrite.view[attr->value.size] = 0;

        if (processor->tag != TAG_REWRITE_URI)
            processor_uri_rewrite_delete(processor,
                                         attr->name_start, attr->end);

        return true;
    }

    if (attr->name.EqualsLiteral("xmlns:c")) {
        /* delete "xmlns:c" attributes */
        if (processor->tag != TAG_REWRITE_URI)
            processor_uri_rewrite_delete(processor,
                                         attr->name_start, attr->end);
        return true;
    }

    return false;
}

static const char *
find_underscore(const char *p, const char *end)
{
    assert(p != nullptr);
    assert(end != nullptr);
    assert(p <= end);

    if (p == end)
        return nullptr;

    if (is_underscore_prefix(p, end))
        return p;

    while (true) {
        p = (const char *)memchr(p + 1, '_', end - p);
        if (p == nullptr)
            return nullptr;

        if (IsWhitespaceOrNull(p[-1]) &&
            is_underscore_prefix(p, end))
            return p;
    }
}

static void
handle_class_attribute(XmlProcessor *processor,
                       const XmlParserAttribute *attr)
{
    auto p = attr->value.begin();
    const auto end = attr->value.end();

    const char *u = find_underscore(p, end);
    if (u == nullptr)
        return;

    struct expansible_buffer *const buffer = processor->buffer;
    expansible_buffer_reset(buffer);

    do {
        if (!expansible_buffer_write_buffer(buffer, p, u - p))
            return;

        p = u;

        const unsigned n = underscore_prefix(p, end);
        const char *prefix;
        if (n == 3 && (prefix = processor->container->GetPrefix()) != nullptr) {
            if (!expansible_buffer_write_string(buffer, prefix))
                return;

            p += 3;
        } else if (n == 2 && (prefix = processor->container->GetQuotedClassName()) != nullptr) {
            if (!expansible_buffer_write_string(buffer, prefix))
                return;

            p += 2;
        } else {
            /* failure; skip all underscores and find the next
               match */
            while (u < end && *u == '_')
                ++u;

            if (!expansible_buffer_write_buffer(buffer, p, u - p))
                return;

            p = u;
        }

        u = find_underscore(p, end);
    } while (u != nullptr);

    if (!expansible_buffer_write_buffer(buffer, p, end - p))
        return;

    const size_t length = expansible_buffer_length(buffer);
    void *q = expansible_buffer_dup(buffer, processor->pool);
    replace_attribute_value(processor, attr,
                            istream_memory_new(processor->pool, q, length));
}

static void
handle_id_attribute(XmlProcessor *processor,
                    const XmlParserAttribute *attr)
{
    auto p = attr->value.begin();
    const auto end = attr->value.end();

    const unsigned n = underscore_prefix(p, end);
    if (n == 3) {
        /* triple underscore: add widget path prefix */

        const char *prefix = processor->container->GetPrefix();
        if (prefix == nullptr)
            return;

        processor_replace_add(processor, attr->value_start,
                              attr->value_start + 3,
                              istream_string_new(processor->pool, prefix));
    } else if (n == 2) {
        /* double underscore: add class name prefix */

        const char *class_name = processor->container->GetQuotedClassName();
        if (class_name == nullptr)
            return;

        processor_replace_add(processor, attr->value_start,
                              attr->value_start + 2,
                              istream_string_new(processor->pool,
                                                 class_name));
    }
}

static void
handle_style_attribute(XmlProcessor *processor,
                       const XmlParserAttribute *attr)
{
    struct widget &widget = *processor->container;
    struct istream *result =
        css_rewrite_block_uris(*processor->pool, *processor->env->pool,
                               *processor->env,
                               *global_translate_cache,
                               widget,
                               attr->value,
                               &html_escape_class);
    if (result != nullptr)
        processor_replace_add(processor, attr->value_start, attr->value_end,
                              result);
}

/**
 * Is this a tag which can have a link attribute?
 */
static bool
is_link_tag(enum tag tag)
{
    return tag == TAG_A || tag == TAG_FORM ||
         tag == TAG_IMG || tag == TAG_SCRIPT ||
        tag == TAG_META || tag == TAG_META_REFRESH ||
        tag == TAG_PARAM || tag == TAG_REWRITE_URI;
}

/**
 * Is this a HTML tag? (i.e. not a proprietary beng-proxy tag)
 */
static bool
is_html_tag(enum tag tag)
{
    return tag == TAG_OTHER || (is_link_tag(tag) && tag != TAG_REWRITE_URI);
}

static void
processor_parser_attr_finished(const XmlParserAttribute *attr, void *ctx)
{
    auto *processor = (XmlProcessor *)ctx;

    processor->had_input = true;

    if (!processor_option_quiet(processor) &&
        is_link_tag(processor->tag) &&
        link_attr_finished(processor, attr))
        return;

    if (!processor_option_quiet(processor) &&
        processor->tag == TAG_META &&
        attr->name.EqualsLiteralIgnoreCase("http-equiv") &&
        attr->value.EqualsLiteralIgnoreCase("refresh")) {
        /* morph TAG_META to TAG_META_REFRESH */
        processor->tag = TAG_META_REFRESH;
        return;
    }

    if (!processor_option_quiet(processor) &&
        processor_option_prefix_class(processor) &&
        /* due to a limitation in the processor and istream_replace,
           we cannot edit attributes followed by a URI attribute */
        !processor->postponed_rewrite.pending &&
        is_html_tag(processor->tag) &&
        attr->name.EqualsLiteral("class")) {
        handle_class_attribute(processor, attr);
        return;
    }

    if (!processor_option_quiet(processor) &&
        processor_option_prefix_id(processor) &&
        /* due to a limitation in the processor and istream_replace,
           we cannot edit attributes followed by a URI attribute */
        !processor->postponed_rewrite.pending &&
        is_html_tag(processor->tag) &&
        (attr->name.EqualsLiteral("id") ||
         attr->name.EqualsLiteral("for"))) {
        handle_id_attribute(processor, attr);
        return;
    }

    if (!processor_option_quiet(processor) &&
        processor_option_style(processor) &&
        processor_option_rewrite_url(processor) &&
        /* due to a limitation in the processor and istream_replace,
           we cannot edit attributes followed by a URI attribute */
        !processor->postponed_rewrite.pending &&
        is_html_tag(processor->tag) &&
        attr->name.EqualsLiteral("style")) {
        handle_style_attribute(processor, attr);
        return;
    }

    switch (processor->tag) {
    case TAG_NONE:
    case TAG_IGNORE:
    case TAG_OTHER:
        break;

    case TAG_WIDGET:
        assert(processor->widget.widget != nullptr);

        parser_widget_attr_finished(processor->widget.widget,
                                    attr->name, attr->value);
        break;

    case TAG_WIDGET_PARAM:
    case TAG_WIDGET_HEADER:
        assert(processor->widget.widget != nullptr);

        if (attr->name.EqualsLiteral("name")) {
            expansible_buffer_set(processor->widget.param.name, attr->value);
        } else if (attr->name.EqualsLiteral("value")) {
            expansible_buffer_set(processor->widget.param.value, attr->value);
        }

        break;

    case TAG_WIDGET_PATH_INFO:
        assert(processor->widget.widget != nullptr);

        if (attr->name.EqualsLiteral("value"))
            processor->widget.widget->path_info
                = p_strdup(*processor->widget.pool, attr->value);

        break;

    case TAG_WIDGET_VIEW:
        assert(processor->widget.widget != nullptr);

        if (attr->name.EqualsLiteral("name")) {
            if (attr->value.IsEmpty()) {
                daemon_log(2, "empty view name\n");
                return;
            }

            processor->widget.widget->view_name =
                p_strdup(*processor->widget.pool, attr->value);
        }

        break;

    case TAG_IMG:
        if (attr->name.EqualsLiteralIgnoreCase("src"))
            processor_uri_rewrite_attribute(processor, attr);
        break;

    case TAG_A:
        if (attr->name.EqualsLiteralIgnoreCase("href")) {
            if (!attr->value.StartsWith({"#", 1}) &&
                !attr->value.StartsWith({"javascript:", 11}))
                processor_uri_rewrite_attribute(processor, attr);
        } else if (processor_option_quiet(processor) &&
                   processor_option_prefix_id(processor) &&
                   attr->name.EqualsLiteralIgnoreCase("name"))
            handle_id_attribute(processor, attr);

        break;

    case TAG_FORM:
        if (attr->name.EqualsLiteralIgnoreCase("action"))
            processor_uri_rewrite_attribute(processor, attr);
        break;

    case TAG_SCRIPT:
        if (!processor_option_quiet(processor) &&
            processor_option_rewrite_url(processor) &&
            attr->name.EqualsLiteralIgnoreCase("src"))
            processor_uri_rewrite_attribute(processor, attr);
        break;

    case TAG_PARAM:
        if (attr->name.EqualsLiteral("value"))
            processor_uri_rewrite_attribute(processor, attr);
        break;

    case TAG_META_REFRESH:
        if (attr->name.EqualsLiteralIgnoreCase("content"))
            processor_uri_rewrite_refresh_attribute(processor, attr);
        break;

    case TAG_REWRITE_URI:
    case TAG_STYLE:
    case TAG_STYLE_PROCESS:
    case TAG_META:
        break;
    }
}

static GError *
widget_catch_callback(GError *error, void *ctx)
{
    struct widget *widget = (struct widget *)ctx;

    daemon_log(3, "error from widget '%s': %s\n",
               widget->GetLogName(), error->message);
    g_error_free(error);
    return nullptr;
}

static struct istream *
embed_widget(XmlProcessor &processor, struct processor_env &env,
             struct widget &widget)
{
    assert(widget.class_name != nullptr);

    if (processor.replace != nullptr) {
        if (!widget_copy_from_request(&widget, &env, nullptr) ||
            widget.display == widget::WIDGET_DISPLAY_NONE) {
            widget_cancel(&widget);
            return nullptr;
        }

        struct istream *istream = embed_inline_widget(*processor.pool,
                                                      env, false, widget);
        if (istream != nullptr)
            istream = istream_catch_new(processor.pool, istream,
                                        widget_catch_callback, &widget);

        return istream;
    } else if (widget.id != nullptr &&
               strcmp(processor.lookup_id, widget.id) == 0) {
        struct pool *caller_pool = processor.caller_pool;
        struct pool *const widget_pool = processor.container->pool;
        const struct widget_lookup_handler *handler = processor.handler;
        void *handler_ctx = processor.handler_ctx;

        parser_close(processor.parser);
        processor.parser = nullptr;

        GError *error = nullptr;
        if (!widget_copy_from_request(&widget, &env, &error)) {
            widget_cancel(&widget);
            processor.handler->error(error, processor.handler_ctx);
            pool_unref(widget_pool);
            pool_unref(caller_pool);
            return nullptr;
        }

        handler->found(&widget, handler_ctx);

        pool_unref(caller_pool);
        pool_unref(widget_pool);

        return nullptr;
    } else {
        widget_cancel(&widget);
        return nullptr;
    }
}

static struct istream *
open_widget_element(XmlProcessor *processor, struct widget *widget)
{
    assert(widget->parent == processor->container);

    if (widget->class_name == nullptr) {
        daemon_log(5, "widget without a class\n");
        return nullptr;
    }

    /* enforce the SELF_CONTAINER flag */
    const bool self_container =
        (processor->options & PROCESSOR_SELF_CONTAINER) != 0;
    if (!widget_init_approval(widget, self_container)) {
        daemon_log(5, "widget '%s' is not allowed to embed widget '%s'\n",
                   processor->container->GetLogName(),
                   widget->GetLogName());
        return nullptr;
    }

    if (widget_check_recursion(widget->parent)) {
        daemon_log(5, "maximum widget depth exceeded for widget '%s'\n",
                   widget->GetLogName());
        return nullptr;
    }

    if (!expansible_buffer_is_empty(processor->widget.params))
        widget->query_string = expansible_buffer_strdup(processor->widget.params,
                                                        processor->widget.pool);

    list_add(&widget->siblings, &processor->container->children);

    return embed_widget(*processor, *processor->env, *widget);
}

static void
widget_element_finished(XmlProcessor *processor,
                        const XmlParserTag *tag, struct widget *widget)
{
    struct istream *istream = open_widget_element(processor, widget);
    assert(istream == nullptr || processor->replace != nullptr);

    if (processor->replace != nullptr)
        processor_replace_add(processor, processor->widget.start_offset,
                              tag->end, istream);
}

static bool
header_name_valid(const char *name, size_t length)
{
    /* name must start with "X-" */
    if (length < 3 ||
        (name[0] != 'x' && name[0] != 'X') ||
        name[1] != '-')
        return false;

    /* the rest must be letters, digits or dash */
    for (size_t i = 2; i < length;  ++i)
        if (!IsAlphaNumericASCII(name[i]) && name[i] != '-')
            return false;

    return true;
}

static void
expansible_buffer_append_uri_escaped(struct expansible_buffer *buffer,
                                     const char *value, size_t length)
{
    char *escaped = (char *)p_malloc(tpool, length * 3);
    length = uri_escape(escaped, StringView(value, length));
    expansible_buffer_write_buffer(buffer, escaped, length);
}

static void
processor_parser_tag_finished(const XmlParserTag *tag, void *ctx)
{
    auto *processor = (XmlProcessor *)ctx;

    processor->had_input = true;

    if (processor->postponed_rewrite.pending)
        processor_uri_rewrite_commit(processor);

    if (processor->tag == TAG_WIDGET) {
        if (tag->type == TAG_OPEN || tag->type == TAG_SHORT)
            processor->widget.start_offset = tag->start;
        else if (processor->widget.widget == nullptr)
            return;

        assert(processor->widget.widget != nullptr);

        if (tag->type == TAG_OPEN)
            return;

        struct widget *widget = processor->widget.widget;
        processor->widget.widget = nullptr;

        widget_element_finished(processor, tag, widget);
    } else if (processor->tag == TAG_WIDGET_PARAM) {
        struct pool_mark_state mark;

        assert(processor->widget.widget != nullptr);

        if (expansible_buffer_is_empty(processor->widget.param.name))
            return;

        pool_mark(tpool, &mark);

        size_t length;
        const char *p = (const char *)
            expansible_buffer_read(processor->widget.param.value, &length);
        if (memchr(p, '&', length) != nullptr) {
            char *q = (char *)p_memdup(tpool, p, length);
            length = unescape_inplace(&html_escape_class, q, length);
            p = q;
        }

        if (!expansible_buffer_is_empty(processor->widget.params))
            expansible_buffer_write_buffer(processor->widget.params, "&", 1);

        size_t name_length;
        const char *name = (const char *)
            expansible_buffer_read(processor->widget.param.name, &name_length);

        expansible_buffer_append_uri_escaped(processor->widget.params,
                                             name, name_length);

        expansible_buffer_write_buffer(processor->widget.params, "=", 1);

        expansible_buffer_append_uri_escaped(processor->widget.params,
                                             p, length);

        pool_rewind(tpool, &mark);
    } else if (processor->tag == TAG_WIDGET_HEADER) {
        assert(processor->widget.widget != nullptr);

        if (tag->type == TAG_CLOSE)
            return;

        size_t length;
        const char *name = (const char *)
            expansible_buffer_read(processor->widget.param.name, &length);
        if (!header_name_valid(name, length)) {
            daemon_log(3, "invalid widget HTTP header name\n");
            return;
        }

        if (processor->widget.widget->headers == nullptr)
            processor->widget.widget->headers =
                strmap_new(processor->widget.pool);

        char *value = expansible_buffer_strdup(processor->widget.param.value,
                                               processor->widget.pool);
        if (strchr(value, '&') != nullptr) {
            length = unescape_inplace(&html_escape_class,
                                      value, strlen(value));
            value[length] = 0;
        }

        processor->widget.widget->headers->Add(expansible_buffer_strdup(processor->widget.param.name,
                                                                        processor->widget.pool),
                                               value);
    } else if (processor->tag == TAG_SCRIPT) {
        if (tag->type == TAG_OPEN)
            parser_script(processor->parser);
        else
            processor->tag = TAG_NONE;
    } else if (processor->tag == TAG_REWRITE_URI) {
        /* the settings of this tag become the new default */
        processor->default_uri_rewrite = processor->uri_rewrite;

        processor_replace_add(processor, tag->start, tag->end, nullptr);
    } else if (processor->tag == TAG_STYLE) {
        if (tag->type == TAG_OPEN && !processor_option_quiet(processor) &&
            processor_option_style(processor)) {
            /* create a CSS processor for the contents of this style
               element */

            processor->tag = TAG_STYLE_PROCESS;

            unsigned options = 0;
            if (processor->options & PROCESSOR_REWRITE_URL)
                options |= CSS_PROCESSOR_REWRITE_URL;
            if (processor->options & PROCESSOR_PREFIX_CSS_CLASS)
                options |= CSS_PROCESSOR_PREFIX_CLASS;
            if (processor->options & PROCESSOR_PREFIX_XML_ID)
                options |= CSS_PROCESSOR_PREFIX_ID;

            istream_init(&processor->cdata_stream, &processor_cdata_istream,
                         processor->pool);

            struct istream *istream =
                css_processor(processor->pool, &processor->cdata_stream,
                              processor->container, processor->env,
                              options);

            /* the end offset will be extended later with
               istream_replace_extend() */
            processor->cdata_start = tag->end;
            processor_replace_add(processor, tag->end, tag->end, istream);
        }
    }
}

static size_t
processor_parser_cdata(const char *p gcc_unused, size_t length,
                       gcc_unused bool escaped, off_t start,
                       void *ctx)
{
    auto *processor = (XmlProcessor *)ctx;

    processor->had_input = true;

    if (processor->tag == TAG_STYLE_PROCESS) {
        /* XXX unescape? */
        length = istream_invoke_data(&processor->cdata_stream, p, length);
        if (length > 0)
            istream_replace_extend(processor->replace, processor->cdata_start,
                                   start + length);
    } else if (processor->replace != nullptr && processor->widget.widget == nullptr)
        istream_replace_settle(processor->replace, start + length);

    return length;
}

static void
processor_parser_eof(void *ctx, off_t length gcc_unused)
{
    auto *processor = (XmlProcessor *)ctx;
    struct pool *const widget_pool = processor->container->pool;

    assert(processor->parser != nullptr);

    processor->parser = nullptr;

    processor_stop_cdata_stream(processor);

    if (processor->container->for_focused.body != nullptr)
        /* the request body could not be submitted to the focused
           widget, because we didn't find it; dispose it now */
        istream_free_unused(&processor->container->for_focused.body);

    if (processor->replace != nullptr)
        istream_replace_finish(processor->replace);

    if (processor->lookup_id != nullptr) {
        /* widget was not found */
        processor->async.Finished();

        processor->handler->not_found(processor->handler_ctx);
        pool_unref(processor->caller_pool);
    }

    pool_unref(widget_pool);
}

static void
processor_parser_abort(GError *error, void *ctx)
{
    auto *processor = (XmlProcessor *)ctx;
    struct pool *const widget_pool = processor->container->pool;

    assert(processor->parser != nullptr);

    processor->parser = nullptr;

    processor_stop_cdata_stream(processor);

    if (processor->container->for_focused.body != nullptr)
        /* the request body could not be submitted to the focused
           widget, because we didn't find it; dispose it now */
        istream_free_unused(&processor->container->for_focused.body);

    if (processor->lookup_id != nullptr) {
        processor->async.Finished();
        processor->handler->error(error, processor->handler_ctx);
        pool_unref(processor->caller_pool);
    } else
        g_error_free(error);

    pool_unref(widget_pool);
}

static const XmlParserHandler processor_parser_handler = {
    .tag_start = processor_parser_tag_start,
    .tag_finished = processor_parser_tag_finished,
    .attr_finished = processor_parser_attr_finished,
    .cdata = processor_parser_cdata,
    .eof = processor_parser_eof,
    .abort = processor_parser_abort,
};

static void
processor_parser_init(XmlProcessor *processor, struct istream *input)
{
    processor->parser = parser_new(*processor->pool, input,
                                   &processor_parser_handler, processor);
}
