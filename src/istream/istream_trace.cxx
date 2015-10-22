/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "istream_trace.hxx"
#include "ForwardIstream.hxx"
#include "pool.hxx"
#include "util/Cast.hxx"

#include <stdio.h>

class TraceIstream final : public ForwardIstream {
public:
    TraceIstream(struct pool &pool, struct istream &_input)
        :ForwardIstream(pool, _input,
                        MakeIstreamHandler<TraceIstream>::handler, this) {
        fprintf(stderr, "%p new()\n", (const void *)this);
    }

    /* virtual methods from class Istream */

    off_t _GetAvailable(bool partial) override {
        fprintf(stderr, "%p available(%d)\n", (const void *)this, partial);
        auto available = ForwardIstream::_GetAvailable(partial);
        fprintf(stderr, "%p available(%d)=%ld\n", (const void *)this,
                partial, (long)available);

        return available;
    }

    off_t _Skip(off_t length) override {
        fprintf(stderr, "%p skip(0x%lu)\n", (const void *)this,
                (unsigned long)length);

        auto result = ForwardIstream::_Skip(length);

        fprintf(stderr, "%p skip(0x%lu) = %lu\n", (const void *)this,
                (unsigned long)length, (unsigned long)result);
        return result;
    }

    void _Read() override {
        fprintf(stderr, "%p read(0x%x)\n", (const void *)this,
                GetHandlerDirect());

        ForwardIstream::_Read();
    }

    int _AsFd() override {
        auto fd = ForwardIstream::_AsFd();
        fprintf(stderr, "%p as_fd()=%d\n", (const void *)this, fd);
        return fd;
    }

    void _Close() override {
        fprintf(stderr, "%p close()\n", (const void *)this);

        ForwardIstream::_Close();
    }

    /* handler */

    size_t OnData(const void *data, size_t length) {
        fprintf(stderr, "%p data(%zu)\n", (const void *)this, length);
        TraceData(data, length);
        auto nbytes = ForwardIstream::OnData(data, length);
        fprintf(stderr, "%p data(%zu)=%zu\n",
                (const void *)this, length, nbytes);
        return nbytes;
    }

    ssize_t OnDirect(FdType type, int fd, size_t max_length) {
        fprintf(stderr, "%p direct(0x%x, %zd)\n", (const void *)this,
                GetHandlerDirect(), max_length);
        auto nbytes = ForwardIstream::OnDirect(type, fd, max_length);
        fprintf(stderr, "%p direct(0x%x, %zd)=%zd\n", (const void *)this,
                GetHandlerDirect(), max_length, nbytes);

        return nbytes;
    }

    void OnEof() {
        fprintf(stderr, "%p eof()\n", (const void *)this);

        ForwardIstream::OnEof();
    }

    void OnError(GError *error) {
        fprintf(stderr, "%p abort('%s')\n", (const void *)this,
                error->message);

        ForwardIstream::OnError(error);
    }

private:
    static void TraceData(const void *data, size_t length);
};

inline void
TraceIstream::TraceData(const void *data0, size_t length)
{
    const char *data = (const char *)data0;
    size_t i;

    fputc('"', stderr);
    for (i = 0; i < length; ++i) {
        if (data[i] == '\n')
            fputs("\\n", stderr);
        else if (data[i] == '\r')
            fputs("\\r", stderr);
        else if (data[i] == 0)
            fputs("\\0", stderr);
        else if (data[i] == '"')
            fputs("\\\"", stderr);
        else
            fputc(data[i], stderr);
    }
    fputs("\"\n", stderr);
}

/*
 * constructor
 *
 */

struct istream *
istream_trace_new(struct pool *pool, struct istream *input)
{
    return NewIstream<TraceIstream>(*pool, *input);
}
