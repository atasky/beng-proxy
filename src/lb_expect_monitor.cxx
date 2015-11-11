/*
 * Monitor which expects a string on a TCP connection.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "lb_expect_monitor.hxx"
#include "lb_monitor.hxx"
#include "lb_config.hxx"
#include "pool.hxx"
#include "async.hxx"
#include "gerrno.h"
#include "net/ConnectSocket.hxx"
#include "net/SocketDescriptor.hxx"
#include "net/SocketAddress.hxx"
#include "event/Event.hxx"
#include "event/Callback.hxx"

#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>

struct ExpectMonitor {
    struct pool *pool;
    const lb_monitor_config *config;

    int fd;

    Event event;

    LBMonitorHandler *handler;

    struct async_operation_ref *async_ref;
    struct async_operation operation;

    ExpectMonitor(struct pool *_pool, const lb_monitor_config *_config,
                  LBMonitorHandler &_handler,
                  async_operation_ref *_async_ref)
        :pool(_pool), config(_config),
         handler(&_handler),
         async_ref(_async_ref) {}

    ExpectMonitor(const ExpectMonitor &other) = delete;

    void EventCallback(evutil_socket_t _fd, short events);

    void Abort();
};

static bool
check_expectation(char *received, size_t received_length,
                  const char *expect)
{
    return g_strrstr_len(received, received_length, expect) != NULL;
}

/*
 * async operation
 *
 */

inline void
ExpectMonitor::Abort()
{
    event.Delete();
    close(fd);
    pool_unref(pool);
    delete this;
}

/*
 * libevent callback
 *
 */

inline void
ExpectMonitor::EventCallback(evutil_socket_t _fd, short events)
{
    operation.Finished();

    if (events & EV_TIMEOUT) {
        close(fd);
        handler->Timeout();
    } else {
        char buffer[1024];

        ssize_t nbytes = recv(_fd, buffer, sizeof(buffer),
                              MSG_DONTWAIT);
        if (nbytes < 0) {
            GError *error = new_error_errno();
            close(fd);
            handler->Error(error);
        } else if (!config->fade_expect.empty() &&
                   check_expectation(buffer, nbytes,
                                     config->fade_expect.c_str())) {
            close(fd);
            handler->Fade();
        } else if (config->expect.empty() ||
                   check_expectation(buffer, nbytes,
                                     config->expect.c_str())) {
            close(fd);
            handler->Success();
        } else {
            close(fd);
            GError *error = g_error_new_literal(g_file_error_quark(), 0,
                                                "Expectation failed");
            handler->Error(error);
        }
    }

    pool_unref(pool);
    delete this;
    pool_commit();
}

/*
 * client_socket handler
 *
 */

static void
expect_monitor_success(SocketDescriptor &&fd, void *ctx)
{
    ExpectMonitor *expect =
        (ExpectMonitor *)ctx;

    if (!expect->config->send.empty()) {
        ssize_t nbytes = send(fd.Get(), expect->config->send.data(),
                              expect->config->send.length(),
                              MSG_DONTWAIT);
        if (nbytes < 0) {
            GError *error = new_error_errno();
            expect->handler->Error(error);
            return;
        }
    }

    struct timeval expect_timeout = {
        time_t(expect->config->timeout > 0 ? expect->config->timeout : 10),
        0,
    };

    expect->fd = fd.Steal();
    expect->event.Set(expect->fd, EV_READ|EV_TIMEOUT,
                      MakeEventCallback(ExpectMonitor, EventCallback), expect);
    expect->event.Add(expect_timeout);

    expect->operation.Init2<ExpectMonitor>();
    expect->async_ref->Set(expect->operation);

    pool_ref(expect->pool);
}

static void
expect_monitor_timeout(void *ctx)
{
    ExpectMonitor *expect =
        (ExpectMonitor *)ctx;
    expect->handler->Timeout();
    delete expect;
}

static void
expect_monitor_error(GError *error, void *ctx)
{
    ExpectMonitor *expect =
        (ExpectMonitor *)ctx;
    expect->handler->Error(error);
    delete expect;
}

static constexpr ConnectSocketHandler expect_monitor_handler = {
    .success = expect_monitor_success,
    .timeout = expect_monitor_timeout,
    .error = expect_monitor_error,
};

/*
 * lb_monitor_class
 *
 */

static void
expect_monitor_run(struct pool *pool, const struct lb_monitor_config *config,
                   SocketAddress address,
                   LBMonitorHandler &handler,
                   struct async_operation_ref *async_ref)
{
    ExpectMonitor *expect = new ExpectMonitor(pool, config,
                                              handler,
                                              async_ref);

    const unsigned connect_timeout = config->connect_timeout > 0
        ? config->connect_timeout
        : (config->timeout > 0
           ? config->timeout
           : 30);

    client_socket_new(*pool, address.GetFamily(), SOCK_STREAM, 0,
                      false,
                      SocketAddress::Null(),
                      address,
                      connect_timeout,
                      expect_monitor_handler, expect,
                      *async_ref);
}

const struct lb_monitor_class expect_monitor_class = {
    .run = expect_monitor_run,
};
