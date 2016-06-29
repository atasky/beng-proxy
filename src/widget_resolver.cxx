/*
 * Wrapper for widget_registry.hxx which resolves widget classes.
 * This library can manage several concurrent requests for one widget
 * object.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "widget_resolver.hxx"
#include "widget_registry.hxx"
#include "widget.hxx"
#include "widget_class.hxx"
#include "async.hxx"
#include "pool.hxx"
#include "util/Cast.hxx"

#include <boost/intrusive/list.hpp>

struct WidgetResolver;

struct WidgetResolverListener final
    : public boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>> {

    struct pool &pool;

    WidgetResolver &resolver;

    struct async_operation operation;

    const WidgetResolverCallback callback;

#ifndef NDEBUG
    bool finished = false, aborted = false;
#endif

    WidgetResolverListener(struct pool &_pool, WidgetResolver &_resolver,
                           WidgetResolverCallback _callback,
                           struct async_operation_ref &async_ref)
        :pool(_pool), resolver(_resolver),
         callback(_callback) {
        operation.Init2<WidgetResolverListener>();
        async_ref.Set(operation);
    }

    void Finish();

    void Abort();
};

struct WidgetResolver {
    Widget &widget;

    boost::intrusive::list<WidgetResolverListener,
                           boost::intrusive::constant_time_size<false>> listeners;

    struct async_operation_ref async_ref;

    bool finished = false;

#ifndef NDEBUG
    bool running = false;
    bool aborted = false;
#endif

    explicit WidgetResolver(Widget &_widget)
        :widget(_widget) {}

    void Start(struct tcache &translate_cache) {
        /* use the widget pool because the listener pool may be
           aborted, while the others still run */
        widget_class_lookup(*widget.pool, *widget.pool, translate_cache,
                            widget.class_name,
                            BIND_THIS_METHOD(RegistryCallback),
                            async_ref);
    }

    void RemoveListener(WidgetResolverListener &listener);
    void Abort();

    void RegistryCallback(const WidgetClass *cls);
};

void
WidgetResolver::RemoveListener(WidgetResolverListener &listener)
{
    listeners.erase(listeners.iterator_to(listener));

    if (listeners.empty() && !finished)
        /* the last listener has been aborted: abort the widget
           registry */
        Abort();
}

void
WidgetResolver::Abort()
{
    assert(listeners.empty());
    assert(widget.resolver == this);

#ifndef NDEBUG
    aborted = true;
#endif

    widget.resolver = nullptr;
    async_ref.Abort();
    pool_unref(widget.pool);
}

/*
 * async operation
 *
 */

inline void
WidgetResolverListener::Abort()
{
    assert(!finished);
    assert(!aborted);
    assert(resolver.widget.resolver == &resolver);
    assert(!resolver.listeners.empty());
    assert(!resolver.finished || resolver.running);
    assert(!resolver.aborted);

#ifndef NDEBUG
    aborted = true;
#endif

    resolver.RemoveListener(*this);

    pool_unref(&pool);
}


/*
 * registry callback
 *
 */

inline void
WidgetResolverListener::Finish()
{
    assert(!finished);
    assert(!aborted);

#ifndef NDEBUG
    finished = true;
#endif

    operation.Finished();
    callback();
    pool_unref(&pool);
}

void
WidgetResolver::RegistryCallback(const WidgetClass *cls)
{
    assert(widget.cls == nullptr);
    assert(widget.resolver == this);
    assert(!listeners.empty());
    assert(!finished);
    assert(!running);
    assert(!aborted);

    finished = true;

#ifndef NDEBUG
    running = true;
#endif

    widget.cls = cls;

    widget.from_template.view = widget.from_request.view = cls != nullptr
        ? widget_view_lookup(&cls->views, widget.from_template.view_name)
        : nullptr;

    widget.session_sync_pending = cls != nullptr && cls->stateful &&
        /* the widget session code requires a valid view */
        widget.from_template.view != nullptr;

    do {
        auto &l = listeners.front();
        listeners.pop_front();
        l.Finish();
    } while (!listeners.empty());

#ifndef NDEBUG
    running = false;
#endif

    pool_unref(widget.pool);
}


/*
 * constructor
 *
 */

static WidgetResolver *
widget_resolver_alloc(Widget &widget)
{
    auto &pool = *widget.pool;

    pool_ref(&pool);

    return widget.resolver = NewFromPool<WidgetResolver>(pool, widget);
}

void
ResolveWidget(struct pool &pool,
              Widget &widget,
              struct tcache &translate_cache,
              WidgetResolverCallback callback,
              struct async_operation_ref &async_ref)
{
    bool is_new = false;

    assert(widget.class_name != nullptr);
    assert(pool_contains(widget.pool, &widget, sizeof(widget)));

    if (widget.cls != nullptr) {
        /* already resolved successfully */
        callback();
        return;
    }

    /* create new resolver object if it does not already exist */

    WidgetResolver *resolver = widget.resolver;
    if (resolver == nullptr) {
        resolver = widget_resolver_alloc(widget);
        is_new = true;
    } else if (resolver->finished) {
        /* we have already failed to resolve this widget class; return
           immediately, don't try again */
        callback();
        return;
    }

    assert(pool_contains(widget.pool, widget.resolver,
                         sizeof(*widget.resolver)));

    /* add a new listener to the resolver */

    pool_ref(&pool);
    auto listener = NewFromPool<WidgetResolverListener>(pool, pool, *resolver,
                                                        callback,
                                                        async_ref);

    resolver->listeners.push_back(*listener);

    /* finally send request to the widget registry */

    if (is_new)
        resolver->Start(translate_cache);
}
