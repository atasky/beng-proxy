/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Widget.hxx"
#include "bp/session/Session.hxx"
#include "pool/tpool.hxx"

#include <assert.h>

WidgetSession *
widget_get_session(Widget *widget, RealmSession *session,
                   bool create)
{
    assert(widget != nullptr);
    assert(session != nullptr);

    if (widget->id == nullptr)
        return nullptr;

    if (widget->parent == nullptr)
        return session->GetWidget(widget->id, create);

    switch (widget->session_scope) {
    case Widget::SessionScope::RESOURCE:
        /* the session is bound to the resource: determine
           widget_session from the parent's session */

        {
            WidgetSession *parent =
                widget_get_session(widget->parent, session, create);
            if (parent == nullptr)
                return nullptr;

            const AutoRewindPool auto_rewind(*tpool);
            return parent->GetChild(widget->id, create);
        }

    case Widget::SessionScope::SITE:
        /* this is a site-global widget: get the widget_session
           directly from the session struct (which is site
           specific) */

        {
            const AutoRewindPool auto_rewind(*tpool);
            return session->GetWidget(widget->id, create);
        }
    }

    assert(0);
    return nullptr;
}
