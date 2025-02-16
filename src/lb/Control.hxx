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

#pragma once

#include "control/Handler.hxx"
#include "control/Server.hxx"
#include "io/Logger.hxx"

#include <string_view>

struct LbInstance;
struct LbControlConfig;
template<typename T> struct ConstBuffer;

class LbControl final : ControlHandler {
	const LLogger logger;

	LbInstance &instance;

	ControlServer server;

public:
	LbControl(LbInstance &_instance, const LbControlConfig &config);

	auto &GetEventLoop() const noexcept {
		return server.GetEventLoop();
	}

	void Enable() noexcept {
		server.Enable();
	}

	void Disable() noexcept {
		server.Disable();
	}

private:
	void InvalidateTranslationCache(std::span<const std::byte> payload,
					SocketAddress address);

	void EnableNode(const char *payload, size_t length);
	void FadeNode(const char *payload, size_t length);

	void QueryNodeStatus(ControlServer &control_server,
			     std::string_view payload,
			     SocketAddress address);

	void QueryStats(ControlServer &control_server, SocketAddress address);

	/* virtual methods from class ControlHandler */
	void OnControlPacket(ControlServer &control_server,
			     BengProxy::ControlCommand command,
			     std::span<const std::byte> payload,
			     std::span<UniqueFileDescriptor> fds,
			     SocketAddress address, int uid) override;

	void OnControlError(std::exception_ptr ep) noexcept override;
};
