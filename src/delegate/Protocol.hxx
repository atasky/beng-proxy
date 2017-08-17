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

#ifndef BENG_DELEGATE_PROTOCOL_HXX
#define BENG_DELEGATE_PROTOCOL_HXX

#include <stdint.h>

enum class DelegateRequestCommand : uint16_t {
    /**
     * Open a regular file, and return the file descriptor in a
     * #DelegateResponseCommand::FD packet.
     */
    OPEN,
};

enum class DelegateResponseCommand : uint16_t {
    /**
     * A file was successfully opened, and the file descriptor is in
     * the ancillary message.
     */
    FD,

    /**
     * The operation has failed.  The payload contains the "errno"
     * value as an "int".
     */
    ERRNO,
};

struct DelegateRequestHeader {
    uint16_t length;
    DelegateRequestCommand command;
};

struct DelegateResponseHeader {
    uint16_t length;
    DelegateResponseCommand command;
};

struct DelegateIntPacket {
    DelegateResponseHeader header;
    int value;
};

#endif
