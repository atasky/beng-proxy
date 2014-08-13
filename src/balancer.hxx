/*
 * Load balancer for AddressList.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_BALANCER_HXX
#define BENG_PROXY_BALANCER_HXX

#include <sys/socket.h>

struct pool;
struct balancer;
struct AddressList;
struct sockaddr;

struct balancer *
balancer_new(struct pool &pool);

void
balancer_free(struct balancer *balancer);

/**
 * Gets the next socket address to connect to.  These are selected in
 * a round-robin fashion, which results in symmetric load-balancing.
 * If a server is known to be faulty, it is not used (see failure.h).
 *
 * @param session a portion of the session id used to select an
 * address if stickiness is enabled; 0 if there is no session
 */
const struct address_envelope &
balancer_get(struct balancer &balancer, const AddressList &list,
             unsigned session);

void
balancer_event_add(struct balancer &balancer);

void
balancer_event_del(struct balancer &balancer);

#endif
