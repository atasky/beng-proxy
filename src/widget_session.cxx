/*
 * Widget sessions.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "widget.hxx"
#include "session.hxx"
#include "tpool.hxx"

#include <assert.h>

WidgetSession *
widget_get_session(struct widget *widget, Session *session,
                   bool create)
{
    assert(widget != NULL);
    assert(session != NULL);

    if (widget->id == NULL)
        return NULL;

    if (widget->parent == NULL)
        return session_get_widget(session, widget->id, create);

    switch (widget->session) {
    case widget::WIDGET_SESSION_RESOURCE:
        /* the session is bound to the resource: determine
           widget_session from the parent's session */

        {
            WidgetSession *parent =
                widget_get_session(widget->parent, session, create);
            if (parent == nullptr)
                return nullptr;

            const AutoRewindPool auto_rewind(*tpool);
            return widget_session_get_child(parent, widget->id, create);
        }

    case widget::WIDGET_SESSION_SITE:
        /* this is a site-global widget: get the widget_session
           directly from the session struct (which is site
           specific) */

        {
            const AutoRewindPool auto_rewind(*tpool);
            return session_get_widget(session, widget->id, create);
        }
    }

    assert(0);
    return NULL;
}
