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

#include "RedirectHttps.hxx"
#include "pool.hxx"
#include "net/HostParser.hxx"

#include <stdio.h>

const char *
MakeHttpsRedirect(struct pool &p, const char *_host, uint16_t port,
                  const char *uri)
{
    char port_buffer[16];
    size_t port_length = 0;
    if (port != 0 && port != 443)
        port_length = sprintf(port_buffer, ":%u", port);

    auto eh = ExtractHost(_host);
    auto host = !eh.host.IsNull()
        ? eh.host
        : _host;

    static constexpr char a = '[';
    static constexpr char b = ']';
    const size_t is_ipv6 = !eh.host.IsNull() && eh.host.Find(':') != nullptr;
    const size_t need_brackets = is_ipv6 && port_length > 0;

    return p_strncat(&p, "https://", size_t(8),
                     &a, need_brackets,
                     host.data, host.size,
                     &b, need_brackets,
                     port_buffer, port_length,
                     uri, strlen(uri),
                     nullptr);
}
