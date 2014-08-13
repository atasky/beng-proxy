/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "address_resolver.hxx"
#include "address_list.hxx"
#include "address_quark.h"
#include "pool.hxx"

#include <socket/resolver.h>

#include <assert.h>
#include <netdb.h>

bool
address_list_resolve(struct pool *pool, AddressList *address_list,
                     const char *host_and_port, int default_port,
                     const struct addrinfo *hints,
                     GError **error_r)
{
    assert(pool != NULL);
    assert(address_list != NULL);

    struct addrinfo *ai;
    int ret = socket_resolve_host_port(host_and_port, default_port,
                                       hints, &ai);
    if (ret != 0) {
        g_set_error(error_r, resolver_quark(), ret,
                    "Failed to resolve '%s': %s",
                    host_and_port, gai_strerror(ret));
        return false;
    }

    for (const struct addrinfo *i = ai; i != NULL; i = i->ai_next)
        address_list->Add(pool, i->ai_addr, i->ai_addrlen);

    freeaddrinfo(ai);

    return true;
}

AddressList *
address_list_resolve_new(struct pool *pool,
                         const char *host_and_port, int default_port,
                         const struct addrinfo *hints,
                         GError **error_r)
{
    auto address_list = NewFromPool<AddressList>(*pool);
    address_list->Init();
    if (!address_list_resolve(pool, address_list,
                              host_and_port, default_port, hints, error_r)) {
        p_free(pool, address_list);
        return NULL;
    }

    return address_list;
}
