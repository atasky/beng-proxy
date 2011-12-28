/*
 * Socket address utilities.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "address.h"

#include <socket/address.h>

const char *
address_to_string(pool_t pool, const struct sockaddr *addr, size_t addrlen)
{
    bool success;
    char host[512];

    success = socket_address_to_string(host, sizeof(host), addr, addrlen);
    if (!success)
        return NULL;

    return p_strdup(pool, host);
}

const char *
address_to_host_string(struct pool *pool, const struct sockaddr *address,
                       size_t address_length)
{
    bool success;
    char host[512];

    success = socket_host_to_string(host, sizeof(host),
                                    address, address_length);
    if (!success)
        return NULL;

    return p_strdup(pool, host);
}
