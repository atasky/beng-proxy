/*
 * FastCGI client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Client.hxx"
#include "Quark.hxx"
#include "Protocol.hxx"
#include "Serialize.hxx"
#include "buffered_socket.hxx"
#include "growing_buffer.hxx"
#include "http_response.hxx"
#include "async.hxx"
#include "istream_fcgi.hxx"
#include "istream_gb.hxx"
#include "istream/istream.hxx"
#include "istream/istream_oo.hxx"
#include "istream/istream_pointer.hxx"
#include "istream/istream_cat.hxx"
#include "please.hxx"
#include "header_parser.hxx"
#include "pevent.hxx"
#include "direct.hxx"
#include "strmap.hxx"
#include "product.h"
#include "pool.hxx"
#include "util/Cast.hxx"
#include "util/ConstBuffer.hxx"
#include "util/CharUtil.hxx"
#include "util/ByteOrder.hxx"
#include "util/StringView.hxx"

#include <glib.h>

#include <sys/socket.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#ifndef NDEBUG
static LIST_HEAD(fcgi_clients);
#endif

struct FcgiClient final : Istream {
#ifndef NDEBUG
    struct list_head siblings;
#endif

    BufferedSocket socket;

    struct lease_ref lease_ref;

    const int stderr_fd;

    struct http_response_handler_ref handler;
    struct async_operation operation;

    const uint16_t id;

    struct Request {
        IstreamPointer input;

        /**
         * This flag is set when the request istream has submitted
         * data.  It is used to check whether the request istream is
         * unavailable, to unschedule the socket write event.
         */
        bool got_data;

        Request():input(nullptr) {}
    } request;

    struct Response {
        enum {
            READ_HEADERS,

            /**
             * There is no response body.  Waiting for the
             * #FCGI_END_REQUEST packet, and then we'll forward the
             * response to the #http_response_handler.
             */
            READ_NO_BODY,

            READ_BODY,
        } read_state = READ_HEADERS;

        /**
         * Only used when read_state==READ_NO_BODY.
         */
        http_status_t status;

        struct strmap *const headers;

        off_t available;

        /**
         * This flag is true in HEAD requests.  HEAD responses may
         * contain a Content-Length header, but no response body will
         * follow (RFC 2616 4.3).
         */
        const bool no_body;

        /**
         * This flag is true if SubmitResponse() is currently calling
         * the HTTP response handler.  During this period,
         * fcgi_client_response_body_read() does nothing, to prevent
         * recursion.
         */
        bool in_handler;

        /**
         * Is the FastCGI application currently sending a STDERR
         * packet?
         */
        bool stderr;

        Response(struct pool &p, bool _no_body)
            :headers(strmap_new(&p)), no_body(_no_body) {}
    } response;

    size_t content_length = 0, skip_length = 0;

    FcgiClient(struct pool &_pool,
               int fd, FdType fd_type, Lease &lease,
               int _stderr_fd,
               uint16_t _id, http_method_t method,
               const struct http_response_handler &_handler, void *_ctx,
               struct async_operation_ref &async_ref);

    ~FcgiClient();

    using Istream::GetPool;

    void Abort();

    /**
     * Release the socket held by this object.
     */
    void ReleaseSocket(bool reuse) {
        socket.Abandon();
        p_lease_release(lease_ref, reuse, GetPool());
    }

    /**
     * Abort receiving the response status/headers from the FastCGI
     * server, and notify the HTTP response handler.
     */
    void AbortResponseHeaders(GError *error);

    /**
     * Abort receiving the response body from the FastCGI server, and
     * notify the response body istream handler.
     */
    void AbortResponseBody(GError *error);

    /**
     * Abort receiving the response from the FastCGI server.  This is
     * a wrapper for AbortResponseHeaders() or AbortResponseBody().
     */
    void AbortResponse(GError *error);

    /**
     * Close the response body.  This is a request from the istream
     * client, and we must not call it back according to the istream API
     * definition.
     */
    void CloseResponseBody();

    /**
     * Find the #FCGI_END_REQUEST packet matching the current request, and
     * returns the offset where it ends, or 0 if none was found.
     */
    gcc_pure
    size_t FindEndRequest(const uint8_t *const data0, size_t size) const;

    bool HandleLine(const char *line, size_t length);

    size_t ParseHeaders(const char *data, size_t length);

    /**
     * Feed data into the FastCGI protocol parser.
     *
     * @return the number of bytes consumed, or 0 if this object has
     * been destructed
     */
    size_t Feed(const uint8_t *data, size_t length);

    /**
     * Submit the response metadata to the #http_response_handler.
     *
     * @return false if the connection was closed
     */
    bool SubmitResponse();

    /**
     * Handle an END_REQUEST packet.  This function will always
     * destroy the client.
     */
    void HandleEnd();

    /**
     * A packet header was received.
     *
     * @return false if the client has been destroyed
     */
    bool HandleHeader(const struct fcgi_record_header &header);

    /**
     * Consume data from the input buffer.
     */
    BufferedResult ConsumeInput(const uint8_t *data, size_t length);

    /* virtual methods from class Istream */

    off_t _GetAvailable(bool partial) override;
    void _Read() override;
    void _Close() override;

    /* istream handler */
    size_t OnData(const void *data, size_t length);
    ssize_t OnDirect(FdType type, int fd, size_t max_length);
    void OnEof();
    void OnError(GError *error);
};

static constexpr struct timeval fcgi_client_timeout = {
    .tv_sec = 120,
    .tv_usec = 0,
};

inline FcgiClient::~FcgiClient()
{
    socket.Destroy();

    if (stderr_fd >= 0)
        close(stderr_fd);

#ifndef NDEBUG
    list_remove(&siblings);
#endif
}

void
FcgiClient::AbortResponseHeaders(GError *error)
{
    assert(response.read_state == Response::READ_HEADERS ||
           response.read_state == Response::READ_NO_BODY);

    operation.Finished();

    if (socket.IsConnected())
        ReleaseSocket(false);

    if (request.input.IsDefined())
        request.input.ClearAndClose();

    handler.InvokeAbort(error);
    Destroy();
}

void
FcgiClient::AbortResponseBody(GError *error)
{
    assert(response.read_state == Response::READ_BODY);

    if (socket.IsConnected())
        ReleaseSocket(false);

    if (request.input.IsDefined())
        request.input.ClearAndClose();

    DestroyError(error);
}

void
FcgiClient::AbortResponse(GError *error)
{
    assert(response.read_state == Response::READ_HEADERS ||
           response.read_state == Response::READ_NO_BODY ||
           response.read_state == Response::READ_BODY);

    if (response.read_state != Response::READ_BODY)
        AbortResponseHeaders(error);
    else
        AbortResponseBody(error);
}

void
FcgiClient::_Close()
{
    assert(response.read_state == Response::READ_BODY);

    if (socket.IsConnected())
        ReleaseSocket(false);

    if (request.input.IsDefined())
        request.input.ClearAndClose();

    Istream::_Close();
}

inline size_t
FcgiClient::FindEndRequest(const uint8_t *const data0, size_t size) const
{
    const uint8_t *data = data0, *const end = data0 + size;

    /* skip the rest of the current packet */
    data += content_length + skip_length;

    while (true) {
        const struct fcgi_record_header *header =
            (const struct fcgi_record_header *)(const void *)data;
        data = (const uint8_t *)(header + 1);
        if (data > end)
            /* reached the end of the given buffer: not found */
            return 0;

        data += FromBE16(header->content_length);
        data += header->padding_length;

        if (header->request_id == id && header->type == FCGI_END_REQUEST)
            /* found it: return the packet end offset */
            return data - data0;
    }
}

inline bool
FcgiClient::HandleLine(const char *line, size_t length)
{
    assert(response.headers != nullptr);
    assert(line != nullptr);

    if (length > 0) {
        header_parse_line(GetPool(), response.headers, {line, length});
        return false;
    } else {
        response.read_state = Response::READ_BODY;
        response.stderr = false;
        return true;
    }
}

inline size_t
FcgiClient::ParseHeaders(const char *data, size_t length)
{
    const char *p = data, *const data_end = data + length;

    const char *next = nullptr;
    bool finished = false;

    const char *eol;
    while ((eol = (const char *)memchr(p, '\n', data_end - p)) != nullptr) {
        next = eol + 1;
        --eol;
        while (eol >= p && IsWhitespaceOrNull(*eol))
            --eol;

        finished = HandleLine(p, eol - p + 1);
        if (finished)
            break;

        p = next;
    }

    return next != nullptr ? next - data : 0;
}

inline size_t
FcgiClient::Feed(const uint8_t *data, size_t length)
{
    if (response.stderr) {
        /* ignore errors and partial writes while forwarding STDERR
           payload; there's nothing useful we can do, and we can't let
           this delay/disturb the response delivery */
        if (stderr_fd >= 0)
            write(stderr_fd, data, length);
        else
            fwrite(data, 1, length, stderr);
        return length;
    }

    switch (response.read_state) {
        size_t consumed;

    case Response::READ_HEADERS:
        return ParseHeaders((const char *)data, length);

    case Response::READ_NO_BODY:
        /* unreachable */
        assert(false);
        return 0;

    case Response::READ_BODY:
        if (response.available == 0)
            /* discard following data */
            /* TODO: emit an error when that happens */
            return length;

        if (response.available > 0 &&
            (off_t)length > response.available)
            /* TODO: emit an error when that happens */
            length = response.available;

        consumed = InvokeData(data, length);
        if (consumed > 0 && response.available >= 0) {
            assert((off_t)consumed <= response.available);
            response.available -= consumed;
        }

        return consumed;
    }

    /* unreachable */
    assert(false);
    return 0;
}

inline bool
FcgiClient::SubmitResponse()
{
    assert(response.read_state == Response::READ_BODY);

    http_status_t status = HTTP_STATUS_OK;

    const char *p = response.headers->Remove("status");
    if (p != nullptr) {
        int i = atoi(p);
        if (http_status_is_valid((http_status_t)i))
            status = (http_status_t)i;
    }

    if (http_status_is_empty(status) || response.no_body) {
        response.read_state = Response::READ_NO_BODY;
        response.status = status;

        /* ignore the rest of this STDOUT payload */
        skip_length += content_length;
        content_length = 0;
        return true;
    }

    response.available = -1;
    p = response.headers->Remove("content-length");
    if (p != nullptr) {
        char *endptr;
        unsigned long long l = strtoull(p, &endptr, 10);
        if (endptr > p && *endptr == 0)
            response.available = l;
    }

    operation.Finished();

    response.in_handler = true;
    handler.InvokeResponse(status, response.headers, Cast());
    response.in_handler = false;

    return socket.IsValid();
}

inline void
FcgiClient::HandleEnd()
{
    assert(!socket.IsConnected());

    if (response.read_state == FcgiClient::Response::READ_HEADERS) {
        GError *error =
            g_error_new_literal(fcgi_quark(), 0,
                                "premature end of headers "
                                "from FastCGI application");
        AbortResponseHeaders(error);
        return;
    }

    if (request.input.IsDefined())
        request.input.Close();

    if (response.read_state == FcgiClient::Response::READ_NO_BODY) {
        operation.Finished();
        handler.InvokeResponse(response.status, response.headers, nullptr);
        Destroy();
    } else if (response.available > 0) {
        GError *error =
            g_error_new_literal(fcgi_quark(), 0,
                                "premature end of body "
                                "from FastCGI application");
        AbortResponseBody(error);
    } else
        DestroyEof();
}

inline bool
FcgiClient::HandleHeader(const struct fcgi_record_header &header)
{
    content_length = FromBE16(header.content_length);
    skip_length = header.padding_length;

    if (header.request_id != id) {
        /* wrong request id; discard this packet */
        skip_length += content_length;
        content_length = 0;
        return true;
    }

    switch (header.type) {
    case FCGI_STDOUT:
        response.stderr = false;

        if (response.read_state == FcgiClient::Response::READ_NO_BODY) {
            /* ignore all payloads until #FCGI_END_REQUEST */
            skip_length += content_length;
            content_length = 0;
        }

        return true;

    case FCGI_STDERR:
        response.stderr = true;
        return true;

    case FCGI_END_REQUEST:
        HandleEnd();
        return false;

    default:
        skip_length += content_length;
        content_length = 0;
        return true;
    }
}

inline BufferedResult
FcgiClient::ConsumeInput(const uint8_t *data0, size_t length0)
{
    const uint8_t *data = data0, *const end = data0 + length0;

    do {
        if (content_length > 0) {
            bool at_headers = response.read_state == FcgiClient::Response::READ_HEADERS;

            size_t length = end - data;
            if (length > content_length)
                length = content_length;

            size_t nbytes = Feed(data, length);
            if (nbytes == 0) {
                if (at_headers) {
                    /* incomplete header line received, want more
                       data */
                    assert(response.read_state == FcgiClient::Response::READ_HEADERS);
                    assert(socket.IsValid());
                    return BufferedResult::MORE;
                }

                if (!socket.IsValid())
                    return BufferedResult::CLOSED;

                /* the response body handler blocks, wait for it to
                   become ready */
                return BufferedResult::BLOCKING;
            }

            data += nbytes;
            content_length -= nbytes;
            socket.Consumed(nbytes);

            if (at_headers && response.read_state == FcgiClient::Response::READ_BODY) {
                /* the read_state has been switched from HEADERS to
                   BODY: we have to deliver the response now */

                return SubmitResponse()
                    /* continue parsing the response body from the
                       buffer */
                    ? BufferedResult::AGAIN_EXPECT
                    : BufferedResult::CLOSED;
            }

            if (content_length > 0)
                return data < end && response.read_state != FcgiClient::Response::READ_HEADERS
                    /* some was consumed, try again later */
                    ? BufferedResult::PARTIAL
                    /* all input was consumed, want more */
                    : BufferedResult::MORE;

            continue;
        }

        if (skip_length > 0) {
            size_t nbytes = end - data;
            if (nbytes > skip_length)
                nbytes = skip_length;

            data += nbytes;
            skip_length -= nbytes;
            socket.Consumed(nbytes);

            if (skip_length > 0)
                return BufferedResult::MORE;

            continue;
        }

        const struct fcgi_record_header *header =
            (const struct fcgi_record_header *)(const void *)data;
        const size_t remaining = end - data;
        if (remaining < sizeof(*header))
            return BufferedResult::MORE;

        data += sizeof(*header);
        socket.Consumed(sizeof(*header));

        if (!HandleHeader(*header))
            return BufferedResult::CLOSED;
    } while (data != end);

    return BufferedResult::MORE;
}

/*
 * istream handler for the request
 *
 */

inline size_t
FcgiClient::OnData(const void *data, size_t length)
{
    assert(socket.IsConnected());
    assert(request.input.IsDefined());

    request.got_data = true;

    ssize_t nbytes = socket.Write(data, length);
    if (nbytes > 0)
        socket.ScheduleWrite();
    else if (gcc_likely(nbytes == WRITE_BLOCKING || nbytes == WRITE_DESTROYED))
        return 0;
    else if (nbytes < 0) {
        GError *error = g_error_new(fcgi_quark(), errno,
                                    "write to FastCGI application failed: %s",
                                    strerror(errno));
        AbortResponse(error);
        return 0;
    }

    return (size_t)nbytes;
}

inline ssize_t
FcgiClient::OnDirect(FdType type, int fd, size_t max_length)
{
    assert(socket.IsConnected());

    request.got_data = true;

    ssize_t nbytes = socket.WriteFrom(fd, type, max_length);
    if (likely(nbytes > 0))
        socket.ScheduleWrite();
    else if (nbytes == WRITE_BLOCKING)
        return ISTREAM_RESULT_BLOCKING;
    else if (nbytes == WRITE_DESTROYED)
        return ISTREAM_RESULT_CLOSED;
    else if (nbytes < 0 && errno == EAGAIN) {
        request.got_data = false;
        socket.UnscheduleWrite();
    }

    return nbytes;
}

inline void
FcgiClient::OnEof()
{
    assert(request.input.IsDefined());

    request.input.Clear();

    socket.UnscheduleWrite();
}

inline void
FcgiClient::OnError(GError *error)
{
    assert(request.input.IsDefined());

    request.input.Clear();

    g_prefix_error(&error, "FastCGI request stream failed: ");
    AbortResponse(error);
}

/*
 * istream implementation for the response body
 *
 */

off_t
FcgiClient::_GetAvailable(bool partial)
{
    if (response.available >= 0)
        return response.available;

    if (!partial || response.stderr)
        return -1;

    return content_length;
}

void
FcgiClient::_Read()
{
    if (response.in_handler)
        /* avoid recursion; the http_response_handler caller will
           continue parsing the response if possible */
        return;

    socket.Read(true);
}

/*
 * socket_wrapper handler
 *
 */

static BufferedResult
fcgi_client_socket_data(const void *buffer, size_t size, void *ctx)
{
    FcgiClient *client = (FcgiClient *)ctx;

    if (client->socket.IsConnected()) {
        /* check if the #FCGI_END_REQUEST packet can be found in the
           following data chunk */
        size_t offset = client->FindEndRequest((const uint8_t *)buffer, size);
        if (offset > 0)
            /* found it: we no longer need the socket, everything we
               need is already in the given buffer */
            client->ReleaseSocket(offset == size);
    }

    const ScopePoolRef ref(client->GetPool() TRACE_ARGS);
    return client->ConsumeInput((const uint8_t *)buffer, size);
}

static bool
fcgi_client_socket_closed(void *ctx)
{
    FcgiClient *client = (FcgiClient *)ctx;

    /* the rest of the response may already be in the input buffer */
    client->ReleaseSocket(false);
    return true;
}

static bool
fcgi_client_socket_remaining(gcc_unused size_t remaining, void *ctx)
{
    gcc_unused
    FcgiClient *client = (FcgiClient *)ctx;

    /* only READ_BODY could have blocked */
    assert(client->response.read_state == FcgiClient::Response::READ_BODY);

    /* the rest of the response may already be in the input buffer */
    return true;
}

static bool
fcgi_client_socket_write(void *ctx)
{
    FcgiClient *client = (FcgiClient *)ctx;

    const ScopePoolRef ref(client->GetPool() TRACE_ARGS);

    client->request.got_data = false;
    client->request.input.Read();

    const bool result = client->socket.IsValid();
    if (result && client->request.input.IsDefined()) {
        if (client->request.got_data)
            client->socket.ScheduleWrite();
        else
            client->socket.UnscheduleWrite();
    }

    return result;
}

static bool
fcgi_client_socket_timeout(void *ctx)
{
    FcgiClient *client = (FcgiClient *)ctx;

    GError *error = g_error_new_literal(fcgi_quark(), 0, "timeout");
    client->AbortResponse(error);
    return false;
}

static void
fcgi_client_socket_error(GError *error, void *ctx)
{
    FcgiClient *client = (FcgiClient *)ctx;

    client->AbortResponse(error);
}

static constexpr BufferedSocketHandler fcgi_client_socket_handler = {
    .data = fcgi_client_socket_data,
    .direct = nullptr,
    .closed = fcgi_client_socket_closed,
    .remaining = fcgi_client_socket_remaining,
    .end = nullptr,
    .write = fcgi_client_socket_write,
    .drained = nullptr,
    .timeout = fcgi_client_socket_timeout,
    .broken = nullptr,
    .error = fcgi_client_socket_error,
};

/*
 * async operation
 *
 */

void
FcgiClient::Abort()
{
    /* async_operation_ref::Abort() can only be used before the
       response was delivered to our callback */
    assert(response.read_state == Response::READ_HEADERS ||
           response.read_state == Response::READ_NO_BODY);
    assert(socket.IsConnected());

    ReleaseSocket(false);

    if (request.input.IsDefined())
        request.input.Close();

    Destroy();
}

/*
 * constructor
 *
 */

inline
FcgiClient::FcgiClient(struct pool &_pool,
                       int fd, FdType fd_type, Lease &lease,
                       int _stderr_fd,
                       uint16_t _id, http_method_t method,
                       const struct http_response_handler &_handler,
                       void *_ctx,
                       struct async_operation_ref &async_ref)
    :Istream(_pool),
     stderr_fd(_stderr_fd),
     id(_id),
     response(GetPool(), http_method_is_empty(method))
{
#ifndef NDEBUG
    list_add(&siblings, &fcgi_clients);
#endif

    socket.Init(GetPool(), fd, fd_type,
                &fcgi_client_timeout, &fcgi_client_timeout,
                fcgi_client_socket_handler, this);

    p_lease_ref_set(lease_ref, lease, GetPool(), "fcgi_client_lease");

    handler.Set(_handler, _ctx);

    operation.Init2<FcgiClient>();
    async_ref.Set(operation);
}

void
fcgi_client_request(struct pool *pool, int fd, FdType fd_type,
                    Lease &lease,
                    http_method_t method, const char *uri,
                    const char *script_filename,
                    const char *script_name, const char *path_info,
                    const char *query_string,
                    const char *document_root,
                    const char *remote_addr,
                    struct strmap *headers, struct istream *body,
                    ConstBuffer<const char *> params,
                    int stderr_fd,
                    const struct http_response_handler *handler,
                    void *handler_ctx,
                    struct async_operation_ref *async_ref)
{
    static unsigned next_request_id = 1;
    ++next_request_id;

    struct fcgi_record_header header = {
        .version = FCGI_VERSION_1,
        .type = FCGI_BEGIN_REQUEST,
        .request_id = GUINT16_TO_BE(next_request_id),
        .content_length = 0,
        .padding_length = 0,
        .reserved = 0,
    };
    static constexpr struct fcgi_begin_request begin_request = {
        .role = ToBE16(FCGI_RESPONDER),
        .flags = FCGI_KEEP_CONN,
    };

    assert(http_method_is_valid(method));

    auto client = NewFromPool<FcgiClient>(*pool, *pool,
                                          fd, fd_type, lease,
                                          stderr_fd,
                                          header.request_id, method,
                                          *handler, handler_ctx, *async_ref);

    GrowingBuffer *buffer = growing_buffer_new(pool, 1024);
    header.content_length = ToBE16(sizeof(begin_request));
    growing_buffer_write_buffer(buffer, &header, sizeof(header));
    growing_buffer_write_buffer(buffer, &begin_request, sizeof(begin_request));

    fcgi_serialize_params(buffer, header.request_id,
                          "REQUEST_METHOD", http_method_to_string(method),
                          "REQUEST_URI", uri,
                          "SCRIPT_FILENAME", script_filename,
                          "SCRIPT_NAME", script_name,
                          "PATH_INFO", path_info,
                          "QUERY_STRING", query_string,
                          "DOCUMENT_ROOT", document_root,
                          "SERVER_SOFTWARE", PRODUCT_TOKEN,
                          nullptr);

    if (remote_addr != nullptr)
        fcgi_serialize_params(buffer, header.request_id,
                              "REMOTE_ADDR", remote_addr,
                              nullptr);

    off_t available = body != nullptr
        ? istream_available(body, false)
        : -1;
    if (available >= 0) {
        char value[64];
        snprintf(value, sizeof(value),
                 "%lu", (unsigned long)available);

        const char *content_type = strmap_get_checked(headers, "content-type");

        fcgi_serialize_params(buffer, header.request_id,
                              "HTTP_CONTENT_LENGTH", value,
                              /* PHP wants the parameter without
                                 "HTTP_" */
                              "CONTENT_LENGTH", value,
                              /* same for the "Content-Type" request
                                 header */
                              content_type != nullptr ? "CONTENT_TYPE" : nullptr,
                              content_type,
                              nullptr);
    }

    if (headers != nullptr)
        fcgi_serialize_headers(buffer, header.request_id, headers);

    if (!params.IsEmpty())
        fcgi_serialize_vparams(buffer, header.request_id, params);

    header.type = FCGI_PARAMS;
    header.content_length = ToBE16(0);
    growing_buffer_write_buffer(buffer, &header, sizeof(header));

    struct istream *request;

    if (body != nullptr)
        /* format the request body */
        request = istream_cat_new(pool,
                                  istream_gb_new(pool, buffer),
                                  istream_fcgi_new(pool, body,
                                                   header.request_id),
                                  nullptr);
    else {
        /* no request body - append an empty STDIN packet */
        header.type = FCGI_STDIN;
        header.content_length = ToBE16(0);
        growing_buffer_write_buffer(buffer, &header, sizeof(header));

        request = istream_gb_new(pool, buffer);
    }

    client->request.input.Set(*request,
                              MakeIstreamHandler<FcgiClient>::handler, client,
                              client->socket.GetDirectMask());

    client->socket.ScheduleReadNoTimeout(true);
    client->request.input.Read();
}
