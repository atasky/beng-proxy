/*
 * Launch and manage FastCGI child processes.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_FCGI_STOCK_H
#define __BENG_FCGI_STOCK_H

#include "stock.h"

struct hstock *
fcgi_stock_new(pool_t pool, unsigned limit);

void
fcgi_stock_get(struct hstock *hstock, pool_t pool,
               const char *executable_path, const char *jail_path,
               stock_callback_t callback, void *callback_ctx,
               struct async_operation_ref *async_ref);

/**
 * Returns the socket descriptor of the specified stock item.
 */
int
fcgi_stock_item_get(const struct stock_item *item);

int
fcgi_stock_item_get_domain(const struct stock_item *item);

/**
 * Translates a path into the application's namespace.
 */
const char *
fcgi_stock_translate_path(const struct stock_item *item,
                          const char *path, pool_t pool);

/**
 * Wrapper for hstock_put().
 */
void
fcgi_stock_put(struct hstock *hstock, struct stock_item *item, bool destroy);

#endif
