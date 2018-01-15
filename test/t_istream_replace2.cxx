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

#include "istream/istream_replace.hxx"
#include "istream/istream_string.hxx"
#include "istream/UnusedPtr.hxx"

#define EXPECTED_RESULT "abcfoofghijklmnopqrstuvwxyz"

class EventLoop;

static Istream *
create_input(struct pool *pool)
{
    return istream_string_new(*pool, "foo").Steal();
}

static Istream *
create_test(EventLoop &, struct pool *pool, Istream *input)
{
    Istream *istream =
        istream_string_new(*pool, "abcdefghijklmnopqrstuvwxyz").Steal();
    istream = istream_replace_new(*pool, UnusedIstreamPtr(istream));
    istream_replace_add(*istream, 3, 3, UnusedIstreamPtr(input));
    istream_replace_extend(*istream, 3, 4);
    istream_replace_extend(*istream, 3, 5);
    istream_replace_finish(*istream);
    return istream;
}

#include "t_istream_filter.hxx"
