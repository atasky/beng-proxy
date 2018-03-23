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
 * Glue code for the logging protocol.
 */

#ifndef BENG_PROXY_LOG_GLUE_HXX
#define BENG_PROXY_LOG_GLUE_HXX

#include "Config.hxx"
#include "http/Method.h"
#include "http/Status.h"

#include <chrono>
#include <string>
#include <memory>

#include <stdint.h>

struct UidGid;
struct AccessLogConfig;
namespace Net { namespace Log { struct Datagram; }}
struct HttpServerRequest;
class LogClient;

class AccessLogGlue {
    const AccessLogConfig config;

    const std::unique_ptr<LogClient> client;

    AccessLogGlue(const AccessLogConfig &config,
                  std::unique_ptr<LogClient> _client);

public:
    ~AccessLogGlue() noexcept;

    static AccessLogGlue *Create(const AccessLogConfig &config,
                                 const UidGid *user);

    void Log(const Net::Log::Datagram &d);

    /**
     * @param length the number of response body (payload) bytes sent
     * to our HTTP client or negative if there was no response body
     * (which is different from "empty response body")
     * @param bytes_received the number of raw bytes received from our
     * HTTP client
     * @param bytes_sent the number of raw bytes sent to our HTTP client
     * (which includes status line, headers and transport encoding
     * overhead such as chunk headers)
     */
    void Log(HttpServerRequest &request, const char *site,
             const char *forwarded_to,
             const char *host, const char *x_forwarded_for,
             const char *referer, const char *user_agent,
             http_status_t status, int64_t length,
             uint64_t bytes_received, uint64_t bytes_sent,
             std::chrono::steady_clock::duration duration);

    void Log(HttpServerRequest &request, const char *site,
             const char *forwarded_to,
             const char *referer, const char *user_agent,
             http_status_t status, int64_t length,
             uint64_t bytes_received, uint64_t bytes_sent,
             std::chrono::steady_clock::duration duration);
};

#endif
