/*
 * Wrapper for widget-registry.h which resolves widget classes.  This
 * library can manage several concurrent requests for one widget
 * object.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "widget-resolver.h"
#include "widget-registry.h"
#include "widget.h"
#include "widget-class.h"
#include "async.h"

#include <inline/list.h>

struct widget_resolver_listener {
    struct list_head siblings;

    pool_t pool;

    struct widget_resolver *resolver;

    struct async_operation operation;

    widget_resolver_callback_t callback;

    void *callback_ctx;

#ifndef NDEBUG
    bool listed, finished, aborted;
#endif
};

struct widget_resolver {
    pool_t pool;

    struct widget *widget;

    struct list_head listeners;

#ifndef NDEBUG
    unsigned num_listeners;
#endif

    struct async_operation_ref async_ref;

    bool finished;

#ifndef NDEBUG
    bool aborted;
#endif
};


/*
 * async operation
 *
 */

static struct widget_resolver_listener *
async_to_wrl(struct async_operation *ao)
{
    return (struct widget_resolver_listener*)(((char*)ao) - offsetof(struct widget_resolver_listener, operation));
}

static void
wrl_abort(struct async_operation *ao)
{
    struct widget_resolver_listener *listener = async_to_wrl(ao);
    struct widget_resolver *resolver = listener->resolver;

    assert(listener->listed);
    assert(!listener->finished);
    assert(!listener->aborted);
    assert(resolver->widget->resolver == resolver);
    assert(!list_empty(&resolver->listeners));
    assert(!resolver->finished);
    assert(!resolver->aborted);

    assert(resolver->num_listeners > 0);
#ifndef NDEBUG
    --resolver->num_listeners;
    listener->listed = false;
    listener->aborted = true;
#endif

    list_remove(&listener->siblings);
    pool_unref(listener->pool);

    if (list_empty(&resolver->listeners)) {
        /* the last listener has been aborted: abort the widget
           registry */
        assert(resolver->num_listeners == 0);

#ifndef NDEBUG
        resolver->aborted = true;
#endif

        resolver->widget->resolver = NULL;
        async_abort(&resolver->async_ref);
        pool_unref(resolver->pool);
    }
}

static const struct async_operation_class listener_async_operation = {
    .abort = wrl_abort,
};


/*
 * registry callback
 *
 */

static void
widget_resolver_callback(const struct widget_class *class, void *ctx)
{
    struct widget *widget = ctx;
    struct widget_resolver *resolver = widget->resolver;

    assert(widget->class == NULL);
    assert(resolver != NULL);
    assert(resolver->widget == widget);
    assert(!list_empty(&resolver->listeners));
    assert(!resolver->finished);
    assert(!resolver->aborted);

#ifndef NDEBUG
    resolver->finished = true;
#endif

    widget->class = class;
    widget->session_sync_pending = class != NULL && class->stateful;

    do {
        struct widget_resolver_listener *listener =
            (struct widget_resolver_listener *)resolver->listeners.next;

        assert(listener->listed);
        assert(!listener->finished);
        assert(!listener->aborted);

        assert(resolver->num_listeners > 0);
#ifndef NDEBUG
        --resolver->num_listeners;
        listener->listed = false;
        listener->finished = true;
#endif

        list_remove(&listener->siblings);

        async_operation_finished(&listener->operation);
        listener->callback(listener->callback_ctx);
        pool_unref(listener->pool);
    } while (!list_empty(&resolver->listeners));

    assert(resolver->num_listeners == 0);

    pool_unref(resolver->pool);
}


/*
 * constructor
 *
 */

static struct widget_resolver *
widget_resolver_alloc(pool_t pool, struct widget *widget)
{
    struct widget_resolver *resolver = p_malloc(pool, sizeof(*resolver));

    pool_ref(pool);
    
    resolver->pool = pool;
    resolver->widget = widget;
    list_init(&resolver->listeners);

#ifndef NDEBUG
    resolver->num_listeners = 0;
    resolver->finished = false;
    resolver->aborted = false;
#endif

    widget->resolver = resolver;

    return resolver;
}

void
widget_resolver_new(pool_t pool, pool_t widget_pool, struct widget *widget,
                    struct tcache *translate_cache,
                    widget_resolver_callback_t callback, void *ctx,
                    struct async_operation_ref *async_ref)
{
    struct widget_resolver *resolver;
    struct widget_resolver_listener *listener;
    bool new = false;

    assert(widget != NULL);
    assert(widget->class_name != NULL);
    assert(widget->class == NULL);
    assert(pool_contains(widget_pool, widget, sizeof(*widget)));

    /* create new resolver object if it does not already exist */

    resolver = widget->resolver;
    if (resolver == NULL) {
        resolver = widget_resolver_alloc(widget_pool, widget);
        new = true;
    } else if (resolver->finished) {
        /* we have already failed to resolve this widget class; return
           immediately, don't try again */
        callback(ctx);
        return;
    }

    assert(resolver->pool == widget_pool);
    assert(pool_contains(widget_pool, widget->resolver,
                         sizeof(*widget->resolver)));

    /* add a new listener to the resolver */

    pool_ref(pool);
    listener = p_malloc(pool, sizeof(*listener));
    listener->pool = pool;
    listener->resolver = resolver;

    async_init(&listener->operation, &listener_async_operation);
    async_ref_set(async_ref, &listener->operation);

    listener->callback = callback;
    listener->callback_ctx = ctx;

#ifndef NDEBUG
    listener->listed = true;
    listener->finished = false;
    listener->aborted = false;
#endif

    list_add(&listener->siblings, resolver->listeners.prev);

#ifndef NDEBUG
    ++resolver->num_listeners;
#endif

    /* finally send request to the widget registry */

    if (new)
        /* don't pass "pool" here because the listener pool may be
           aborted, while the others still run */
        widget_class_lookup(widget_pool, widget_pool, translate_cache,
                            widget->class_name,
                            widget_resolver_callback, widget,
                            &resolver->async_ref);
}
