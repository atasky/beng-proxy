/*
 * Copyright 2007-2022 CM4all GmbH
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

/*
 * Pick the output of a single widget for displaying it in an IFRAME.
 */

#pragma once

struct pool;
struct WidgetContext;
template<typename T> class SharedPoolPtr;
class Widget;
class HttpResponseHandler;
class WidgetLookupHandler;
class CancellablePointer;
class StopwatchPtr;

/**
 * Request the contents of the specified widget.  This is a wrapper
 * for widget_http_request() with some additional checks (untrusted
 * host, session management).
 */
void
frame_top_widget(struct pool &pool, Widget &widget,
		 SharedPoolPtr<WidgetContext> ctx,
		 const StopwatchPtr &parent_stopwatch,
		 HttpResponseHandler &_handler,
		 CancellablePointer &cancel_ptr);

/**
 * Looks up a child widget in the specified widget.  This is a wrapper
 * for widget_http_lookup() with some additional checks (untrusted
 * host, session management).
 */
void
frame_parent_widget(struct pool &pool, Widget &widget, const char *id,
		    SharedPoolPtr<WidgetContext> ctx,
		    const StopwatchPtr &parent_stopwatch,
		    WidgetLookupHandler &handler,
		    CancellablePointer &cancel_ptr);
