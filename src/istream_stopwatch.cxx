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

#include "istream_stopwatch.hxx"
#include "istream/ForwardIstream.hxx"
#include "stopwatch.hxx"

class StopwatchIstream final : public ForwardIstream {
    Stopwatch &stopwatch;

public:
    StopwatchIstream(struct pool &p, Istream &_input,
                     Stopwatch &_stopwatch)
        :ForwardIstream(p, _input),
         stopwatch(_stopwatch) {}

    /* virtual methods from class Istream */

    int _AsFd() override;

    /* virtual methods from class IstreamHandler */
    void OnEof() override;
    void OnError(std::exception_ptr ep) override;
};


/*
 * istream handler
 *
 */

void
StopwatchIstream::OnEof()
{
    stopwatch_event(&stopwatch, "end");
    stopwatch_dump(&stopwatch);

    ForwardIstream::OnEof();
}

void
StopwatchIstream::OnError(std::exception_ptr ep)
{
    stopwatch_event(&stopwatch, "abort");
    stopwatch_dump(&stopwatch);

    ForwardIstream::OnError(ep);
}

/*
 * istream implementation
 *
 */

int
StopwatchIstream::_AsFd()
{
    int fd = input.AsFd();
    if (fd >= 0) {
        stopwatch_event(&stopwatch, "as_fd");
        stopwatch_dump(&stopwatch);
        Destroy();
    }

    return fd;
}

/*
 * constructor
 *
 */

Istream *
istream_stopwatch_new(struct pool &pool, Istream &input,
                      Stopwatch *_stopwatch)
{
    if (_stopwatch == nullptr)
        return &input;

    return NewIstream<StopwatchIstream>(pool, input, *_stopwatch);
}
