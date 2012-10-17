/*
 * The "slice" memory allocator.  It is an allocator for large numbers
 * of small fixed-size objects.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_SLICE_H
#define BENG_PROXY_SLICE_H

#include <inline/compiler.h>

#include <stddef.h>

struct slice_pool;

struct slice_pool *
slice_pool_new(size_t slice_size, unsigned per_area);

gcc_nonnull_all
void
slice_pool_free(struct slice_pool *pool);

gcc_const gcc_nonnull_all
size_t
slice_pool_get_slice_size(const struct slice_pool *pool);

gcc_nonnull_all
void
slice_pool_compress(struct slice_pool *pool);

gcc_pure gcc_nonnull_all
struct slice_area *
slice_pool_get_area(struct slice_pool *pool);

gcc_malloc gcc_nonnull_all
void *
slice_alloc(struct slice_pool *pool, struct slice_area *area);

gcc_nonnull_all
void
slice_free(struct slice_pool *pool, struct slice_area *area, void *p);

#endif
