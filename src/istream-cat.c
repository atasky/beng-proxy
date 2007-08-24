/*
 * This istream filter adds HTTP chunking.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "istream.h"

#include <assert.h>
#include <stdarg.h>

#define MAX_INPUTS 16

struct input {
    struct input *next;
    struct istream_cat *cat;
    istream_t istream;
};

struct istream_cat {
    struct istream output;
    struct input *current;
    struct input inputs[MAX_INPUTS];
};


static const char hex_digits[] = "0123456789abcdef";

static size_t
cat_input_data(const void *data, size_t length, void *ctx)
{
    struct input *input = ctx;
    struct istream_cat *cat = input->cat;

    assert(input->istream != NULL);

    if (input == cat->current)
        return istream_invoke_data(&cat->output, data, length);
    else
        return 0;
}

static void
cat_input_eof(void *ctx)
{
    struct input *input = ctx;
    struct istream_cat *cat = input->cat;

    assert(input->istream != NULL);

    pool_unref(input->istream->pool);
    input->istream = NULL;

    if (input == cat->current) {
        cat->current = input->next;
        if (cat->current == NULL) {
            istream_invoke_eof(&cat->output);
            istream_close(&cat->output);
        }
    }
}

static void
cat_input_free(void *ctx)
{
    struct input *input = ctx;
    struct istream_cat *cat = input->cat;

    if (input->istream != NULL)
        istream_close(&cat->output);
}

static const struct istream_handler cat_input_handler = {
    .data = cat_input_data,
    .eof = cat_input_eof,
    .free = cat_input_free,
};


static inline struct istream_cat *
istream_to_cat(istream_t istream)
{
    return (struct istream_cat *)(((char*)istream) - offsetof(struct istream_cat, output));
}

static void
istream_cat_read(istream_t istream)
{
    struct istream_cat *cat = istream_to_cat(istream);

    while (cat->current != NULL && cat->current->istream == NULL)
        cat->current = cat->current->next;

    if (cat->current == NULL) {
        istream_invoke_eof(&cat->output);
        istream_close(&cat->output);
        return;
    }

    istream_read(cat->current->istream);
}

static void
istream_cat_direct(istream_t istream)
{
    struct istream_cat *cat = istream_to_cat(istream);

    while (cat->current != NULL && cat->current->istream == NULL)
        cat->current = cat->current->next;

    if (cat->current == NULL) {
        istream_invoke_eof(&cat->output);
        istream_close(&cat->output);
        return;
    }

    istream_direct(cat->current->istream);
}

static void
istream_cat_close(istream_t istream)
{
    struct istream_cat *cat = istream_to_cat(istream);
    struct input *input;

    while (cat->current != NULL) {
        input = cat->current;
        if (input->istream != NULL) {
            pool_t pool = cat->current->istream->pool;
            istream_free(&cat->current->istream);
            pool_unref(pool);
            cat->current = input->next;
        }
    }
    
    istream_invoke_free(&cat->output);
}

static const struct istream istream_cat = {
    .read = istream_cat_read,
    .direct = istream_cat_direct,
    .close = istream_cat_close,
};


istream_t
istream_cat_new(pool_t pool, ...)
{
    struct istream_cat *cat = p_malloc(pool, sizeof(*cat));
    va_list ap;
    unsigned num = 0;
    istream_t istream;
    struct input **next_p = &cat->current, *input;

    cat->output = istream_cat;
    cat->output.pool = pool;

    va_start(ap, pool);
    while ((istream = va_arg(ap, istream_t)) != NULL) {
        assert(istream->handler == NULL);
        assert(num < MAX_INPUTS);

        input = &cat->inputs[num++];
        input->next = NULL;
        input->cat = cat;

        input->istream = istream;
        istream->handler = &cat_input_handler;
        istream->handler_ctx = input;
        pool_ref(istream->pool);

        *next_p = input;
        next_p = &input->next;
    }
    va_end(ap);

    return &cat->output;
}
