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

#include "Widget.hxx"
#include "Error.hxx"
#include "Class.hxx"
#include "Ref.hxx"
#include "uri/PRelative.hxx"
#include "util/StringSplit.hxx"
#include "AllocatorPtr.hxx"

#include <string.h>
#include <assert.h>

bool
Widget::HasFocus() const noexcept
{
	assert(parent != nullptr);

	return id != nullptr &&
		parent->from_request.focus_ref != nullptr &&
		strcmp(id, parent->from_request.focus_ref->id) == 0 &&
		parent->from_request.focus_ref->next == nullptr;
}

bool
Widget::DescendantHasFocus() const noexcept
{
	assert(parent != nullptr);

	return id != nullptr &&
		parent->from_request.focus_ref != nullptr &&
		strcmp(id, parent->from_request.focus_ref->id) == 0 &&
		parent->from_request.focus_ref->next != nullptr;
}

void
Widget::CopyFromRequest()
{
	assert(parent != nullptr);
	assert(lazy.address == nullptr);
	assert(from_request.path_info == nullptr);
	assert(from_request.query_string.empty());
	assert(from_request.focus_ref == nullptr);
	assert(from_request.method == HTTP_METHOD_GET);
	assert(!from_request.body);

	if (id == nullptr)
		return;

	/* are we focused? */

	if (HasFocus()) {
		if (parent->for_focused == nullptr)
			return;

		auto &src = *parent->for_focused;

		/* we're in focus.  forward query string and request body. */
		from_request.path_info = src.path_info;
		if (from_request.path_info != nullptr) {
			from_request.path_info =
				uri_compress(pool, from_request.path_info);
			if (from_request.path_info == nullptr)
				throw WidgetError(*this, WidgetErrorCode::FORBIDDEN,
						  "path compression failed");
		}

		from_request.query_string = src.query_string;

		from_request.method = src.method;
		from_request.body = std::move(src.body);
	} else if (DescendantHasFocus()) {
		/* we are the parent (or grant-parent) of the focused widget.
		   store the relative focus_ref. */

		from_request.focus_ref = parent->from_request.focus_ref->next;
		parent->from_request.focus_ref = nullptr;

		for_focused = std::exchange(parent->for_focused, nullptr);
	}
}

void
Widget::CopyFromRedirectLocation(std::string_view location,
				 RealmSession *session) noexcept
{
	assert(cls != nullptr);

	from_request.method = HTTP_METHOD_GET;
	from_request.body = nullptr;

	const auto [_path_info, _query_string] = Split(location, '?');
	from_request.path_info = p_strdup(pool, _path_info);
	from_request.query_string = _query_string;

	lazy.address = nullptr;

	if (session != nullptr) {
		assert(cls->stateful);

		auto *ws = GetSession(*session, true);
		if (ws != nullptr)
			SaveToSession(*ws);
	}
}
