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

#include "bp/session/Lease.hxx"
#include "bp/session/Session.hxx"
#include "bp/session/Manager.hxx"
#include "event/Loop.hxx"

#include <gtest/gtest.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

TEST(SessionTest, Basic)
{
	EventLoop event_loop;

	SessionManager session_manager(event_loop, std::chrono::minutes(30),
				       0, 0);

	const auto session_id = session_manager.CreateSession()->id;

	SessionLease session{session_manager, session_id};
	ASSERT_TRUE(session);
	ASSERT_EQ(session->id, session_id);

	auto *realm = session->GetRealm("a_realm_name");
	ASSERT_NE(realm, nullptr);

	auto *widget = realm->GetWidget("a_widget_name", false);
	ASSERT_EQ(widget, nullptr);

	widget = realm->GetWidget("a_widget_name", true);
	ASSERT_NE(widget, nullptr);
}
