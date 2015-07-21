/*
 * control_handler wrapper which publishes raw packets to
 * #UdpDistribute.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_CONTROL_DISTRIBUTE_HXX
#define BENG_PROXY_CONTROL_DISTRIBUTE_HXX

#include "control_handler.hxx"

#include <stddef.h>

struct ControlServer;
struct UdpDistribute;
class SocketAddress;

class ControlDistribute final : public ControlHandler {
    UdpDistribute *const distribute;

    ControlHandler &next_handler;

public:
    explicit ControlDistribute(ControlHandler &_next_handler);
    ~ControlDistribute();

    int Add();
    void Clear();

    static const struct control_handler handler;

private:
    /* virtual methods from class ControlHandler */
    bool OnControlRaw(const void *data, size_t length,
                      SocketAddress address, int uid) override;

    void OnControlPacket(ControlServer &control_server,
                         enum beng_control_command command,
                         const void *payload, size_t payload_length,
                         SocketAddress address) override;

    void OnControlError(Error &&error) override;
};

#endif
