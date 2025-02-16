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

#include "PInstance.hxx"
#include "pool/pool.hxx"
#include "pool/Ptr.hxx"
#include "net/Ping.hxx"
#include "net/AllocatedSocketAddress.hxx"
#include "net/Parser.hxx"
#include "util/PrintException.hxx"

#include <stdio.h>
#include <stdlib.h>

static bool success;

class MyPingClientHandler final : public PingClientHandler {
public:
	void PingResponse() noexcept override {
		success = true;
		printf("ok\n");
	}

	void PingTimeout() noexcept override {
		fprintf(stderr, "timeout\n");
	}

	void PingError(std::exception_ptr ep) noexcept override {
		PrintException(ep);
	}
};

int
main(int argc, char **argv) noexcept
try {
	if (argc != 2) {
		fprintf(stderr, "usage: run-ping IP\n");
		return EXIT_FAILURE;
	}

	PInstance instance;
	const auto pool = pool_new_linear(instance.root_pool, "test", 8192);

	const auto address = ParseSocketAddress(argv[1], 0, false);

	MyPingClientHandler handler;
	PingClient client(instance.event_loop, handler);
	client.Start(address);

	instance.event_loop.Dispatch();

	return success ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception &e) {
	PrintException(e);
	return EXIT_FAILURE;
}
