/*
 * An istream filter that escapes the data.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_ISTREAM_ESCAPE_H
#define BENG_PROXY_ISTREAM_ESCAPE_H

struct pool;
struct istream;
struct escape_class;

struct istream *
istream_escape_new(struct pool *pool, struct istream *input,
                   const struct escape_class *class);

#endif
