/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Listener.hxx"
#include "net/SocketAddress.hxx"
#include "util/Error.hxx"

#include <daemon/log.h>

void
TrafoListener::RemoveConnection(TrafoConnection &connection)
{
    connections.remove(connection);
}

void
TrafoListener::OnAccept(SocketDescriptor &&new_fd,
                        gcc_unused SocketAddress address)
{
    connections.emplace_back(event_loop, *this, handler, std::move(new_fd));
}

void
TrafoListener::OnAcceptError(Error &&error)
{
    daemon_log(2, "%s\n", error.GetMessage());
}
