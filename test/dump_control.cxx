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

#include "control_server.hxx"
#include "net/UdpListenerConfig.hxx"
#include "net/Parser.hxx"
#include "event/Loop.hxx"
#include "system/SetupProcess.hxx"
#include "util/PrintException.hxx"

#include "util/Compiler.h"
#include <daemon/log.h>

#include <stdio.h>

class DumpControlHandler final : public ControlHandler {
public:
    void OnControlPacket(gcc_unused ControlServer &control_server,
                         enum beng_control_command command,
                         gcc_unused const void *payload, size_t payload_length,
                         gcc_unused SocketAddress address) override {
        printf("packet command=%u length=%zu\n", command, payload_length);
    }

    void OnControlError(std::exception_ptr ep) override {
        PrintException(ep);
    }
};

int main(int argc, char **argv)
try {
    daemon_log_config.verbose = 5;

    if (argc > 3) {
        fprintf(stderr, "usage: dump-control [LISTEN:PORT [MCAST_GROUP]]\n");
        return 1;
    }

    const char *listen_host = argc >= 2 ? argv[1] : "*";
    const char *mcast_group = argc >= 3 ? argv[2] : NULL;

    SetupProcess();

    EventLoop event_loop;

    UdpListenerConfig config;
    config.bind_address = ParseSocketAddress(listen_host, 1234, true);

    if (mcast_group != nullptr)
        config.multicast_group = ParseSocketAddress(mcast_group, 0, false);

    DumpControlHandler handler;

    ControlServer cs(event_loop, handler, config);

    event_loop.Dispatch();

    return 0;
} catch (const std::exception &e) {
    PrintException(e);
    return EXIT_FAILURE;
 }
