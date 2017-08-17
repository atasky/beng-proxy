/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk.com>
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

#ifndef BENG_LB_LISTENER_HXX
#define BENG_LB_LISTENER_HXX

#include "Goto.hxx"
#include "io/Logger.hxx"
#include "net/ServerSocket.hxx"

struct SslFactory;
struct LbListenerConfig;
struct LbInstance;
class LbGotoMap;

/**
 * Listener on a TCP port.
 */
class LbListener final : public ServerSocket {
    LbInstance &instance;

    const LbListenerConfig &config;

    LbGoto destination;

    SslFactory *ssl_factory = nullptr;

    const Logger logger;

public:
    LbListener(LbInstance &_instance,
               const LbListenerConfig &_config);
    ~LbListener();

    void Setup();
    void Scan(LbGotoMap &goto_map);

    unsigned FlushSSLSessionCache(long tm);

protected:
    void OnAccept(UniqueSocketDescriptor &&fd, SocketAddress address) override;
    void OnAcceptError(std::exception_ptr ep) override;
};

#endif
