/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_LB_CONNECTION_H
#define BENG_PROXY_LB_CONNECTION_H

#include <boost/intrusive/list.hpp>

#include <chrono>

#include <stdint.h>

struct pool;
struct SslFactory;
struct SslFilter;
struct ThreadSocketFilter;
class UniqueSocketDescriptor;
class SocketAddress;
struct LbListenerConfig;
struct LbClusterConfig;
struct LbGoto;
struct LbTcpConnection;
struct LbInstance;

struct LbConnection final
    : boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>> {

    struct pool &pool;

    LbInstance &instance;

    const LbListenerConfig &listener;

    /**
     * The client's address formatted as a string (for logging).  This
     * is guaranteed to be non-nullptr.
     */
    const char *client_address;

    SslFilter *ssl_filter = nullptr;
    ThreadSocketFilter *thread_socket_filter = nullptr;

    LbTcpConnection *tcp;

    LbConnection(struct pool &_pool, LbInstance &_instance,
                 const LbListenerConfig &_listener,
                 SocketAddress _client_address);
};

LbConnection *
lb_connection_new(LbInstance &instance,
                  const LbListenerConfig &listener,
                  SslFactory *ssl_factory,
                  UniqueSocketDescriptor &&fd, SocketAddress address);

void
lb_connection_remove(LbConnection *connection);

void
lb_connection_close(LbConnection *connection);

#endif
