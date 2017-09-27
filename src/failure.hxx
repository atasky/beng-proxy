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

/*
 * Remember which servers (socket addresses) failed recently.
 */

#ifndef BENG_PROXY_FAILURE_HXX
#define BENG_PROXY_FAILURE_HXX

#include "util/Compiler.h"

#include <chrono>

class SocketAddress;

enum failure_status {
    /**
     * No failure, host is ok.
     */
    FAILURE_OK,

    /**
     * Host is being faded out (graceful shutdown).  No new sessions.
     */
    FAILURE_FADE,

    /**
     * The response received from the server indicates a server error.
     */
    FAILURE_RESPONSE,

    /**
     * Host has failed.
     */
    FAILURE_FAILED,

    /**
     * The failure was submitted by a "monitor", and will not expire
     * until the monitor detects recovery.
     */
    FAILURE_MONITOR,
};

void
failure_init() noexcept;

void
failure_deinit() noexcept;

void
failure_set(SocketAddress address,
            enum failure_status status, std::chrono::seconds duration) noexcept;

void
failure_add(SocketAddress address) noexcept;

/**
 * Unset a failure status.
 *
 * @param status the status to be removed; #FAILURE_OK is a catch-all
 * status that matches everything
 */
void
failure_unset(SocketAddress address, enum failure_status status) noexcept;

gcc_pure
enum failure_status
failure_get_status(SocketAddress address) noexcept;

struct ScopeFailureInit {
    ScopeFailureInit() noexcept {
        failure_init();
    }

    ~ScopeFailureInit() noexcept {
        failure_deinit();
    }

    ScopeFailureInit(const ScopeFailureInit &) = delete;
    ScopeFailureInit &operator=(const ScopeFailureInit &) = delete;
};

#endif
