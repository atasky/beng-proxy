/*
 * HTTP server implementation.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http-server-internal.h"
#include "direct.h"

#include <daemon/log.h>

#include <unistd.h>
#include <errno.h>
#include <string.h>

static size_t
http_server_response_stream_data(const void *data, size_t length, void *ctx)
{
    http_server_connection_t connection = ctx;
    ssize_t nbytes;

    assert(connection->fd >= 0);
    assert(connection->response.writing);
    assert(connection->response.istream != NULL);

    nbytes = write(connection->fd, data, length);

    if (likely(nbytes >= 0)) {
        event2_or(&connection->event, EV_WRITE);
        return (size_t)nbytes;
    }

    if (likely(errno == EAGAIN)) {
        event2_or(&connection->event, EV_WRITE);
        return 0;
    }

    daemon_log(1, "write error on HTTP connection: %s\n", strerror(errno));
    http_server_connection_close(connection);
    return 0;
}

#ifdef __linux
static ssize_t
http_server_response_stream_direct(istream_direct_t type, int fd, size_t max_length, void *ctx)
{
    http_server_connection_t connection = ctx;
    ssize_t nbytes;

    assert(connection->response.writing);

    nbytes = istream_direct_to_socket(type, fd, connection->fd, max_length);
    if (unlikely(nbytes < 0 && errno == EAGAIN))
        return -2;

    if (likely(nbytes > 0))
        event2_or(&connection->event, EV_WRITE);

    return nbytes;
}
#endif

static void
http_server_response_stream_eof(void *ctx)
{
    http_server_connection_t connection = ctx;

    assert(connection->request.read_state != READ_START &&
           connection->request.read_state != READ_HEADERS);
    assert(connection->request.request != NULL);
    assert(connection->response.writing);
    assert(connection->response.istream != NULL);
    assert(istream_pool(connection->response.istream) != NULL);

    connection->response.istream = NULL;
    connection->response.writing = 0;

    if (connection->response.writing_100_continue) {
        /* connection->response.istream contained the string "100
           Continue", and not a full response - return here, because
           we do not want the request/response pair to be
           destructed */
        event2_nand(&connection->event, EV_WRITE);
        return;
    }

    if (connection->request.read_state == READ_BODY &&
        !connection->request.expect_100_continue) {
        /* We are still reading the request body, which we don't need
           anymore.  To discard it, we simply close the connection by
           disabling keepalive; this seems cheaper than redirecting
           the rest of the body to /dev/null */
        connection->keep_alive = 0;
    }

    http_server_request_free(&connection->request.request);

    connection->request.read_state = READ_START;

    if (connection->keep_alive) {
        /* set up events for next request */
        event2_set(&connection->event, EV_READ);
    } else {
        /* keepalive disabled and response is finished: we must close
           the connection */
        http_server_connection_close(connection);
    }
}

static void
http_server_response_stream_abort(void *ctx)
{
    http_server_connection_t connection = ctx;

    assert(connection->response.writing);

    connection->response.istream = NULL;
    connection->response.writing = 0;

    http_server_connection_close(connection);
}

const struct istream_handler http_server_response_stream_handler = {
    .data = http_server_response_stream_data,
#ifdef __linux
    .direct = http_server_response_stream_direct,
#endif
    .eof = http_server_response_stream_eof,
    .abort = http_server_response_stream_abort,
};
