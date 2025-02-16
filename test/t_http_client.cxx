/*
 * Copyright 2007-2022 CM4all GmbH
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define HAVE_EXPECT_100
#define HAVE_CHUNKED_REQUEST_BODY
#define ENABLE_CLOSE_IGNORED_REQUEST_BODY
#define ENABLE_HUGE_BODY
#define USE_BUCKETS

#include "t_client.hxx"
#include "DemoHttpServerConnection.hxx"
#include "http/Client.hxx"
#include "http/Headers.hxx"
#include "system/SetupProcess.hxx"
#include "io/FileDescriptor.hxx"
#include "io/SpliceSupport.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "net/SocketAddress.hxx"
#include "system/Error.hxx"
#include "fs/FilteredSocket.hxx"
#include "fs/NopSocketFilter.hxx"
#include "fs/NopThreadSocketFilter.hxx"
#include "fs/ThreadSocketFilter.hxx"
#include "thread/Pool.hxx"
#include "memory/fb_pool.hxx"
#include "pool/UniquePtr.hxx"
#include "PipeLease.hxx"
#include "istream/New.hxx"
#include "istream/DeferReadIstream.hxx"
#include "istream/PipeLeaseIstream.hxx"
#include "istream/UnusedPtr.hxx"
#include "stopwatch.hxx"
#include "util/AbortFlag.hxx"

#include <memory>

#include <sys/socket.h>
#include <sys/wait.h>

class Server final : DemoHttpServerConnection {
public:
	using DemoHttpServerConnection::DemoHttpServerConnection;

	static auto New(struct pool &pool, EventLoop &event_loop, Mode mode) {
		UniqueSocketDescriptor client_socket, server_socket;
		if (!UniqueSocketDescriptor::CreateSocketPair(AF_LOCAL, SOCK_STREAM, 0,
							      client_socket, server_socket))
			throw MakeErrno("socketpair() failed");

		auto server = std::make_unique<Server>(pool, event_loop,
						       UniquePoolPtr<FilteredSocket>::Make(pool,
											   event_loop,
											   std::move(server_socket),
											   FdType::FD_SOCKET),
						       nullptr,
						       mode);
		return std::make_pair(std::move(server), std::move(client_socket));
	}
};

class HttpClientConnection final : public ClientConnection {
	const pid_t pid = 0;

	std::unique_ptr<Server> server;

	FilteredSocket socket;

	const std::string peer_name{"localhost"};

public:
	HttpClientConnection(EventLoop &_event_loop, pid_t _pid,
			     SocketDescriptor fd,
			     SocketFilterPtr _filter) noexcept
		:pid(_pid), socket(_event_loop) {
		socket.InitDummy(fd, FdType::FD_SOCKET,
				 std::move(_filter));
	}

	HttpClientConnection(EventLoop &_event_loop,
			     std::pair<std::unique_ptr<Server>, UniqueSocketDescriptor> _server,
			     SocketFilterPtr _filter)
		:server(std::move(_server.first)),
		 socket(_event_loop,
			std::move(_server.second), FdType::FD_SOCKET,
			std::move(_filter))
	{
	}

	~HttpClientConnection() noexcept override;

	void Request(struct pool &pool,
		     Lease &lease,
		     http_method_t method, const char *uri,
		     StringMap &&headers,
		     UnusedIstreamPtr body,
		     bool expect_100,
		     HttpResponseHandler &handler,
		     CancellablePointer &cancel_ptr) noexcept override {
		http_client_request(pool, nullptr,
				    socket, lease,
				    peer_name.c_str(),
				    method, uri, headers, {},
				    std::move(body), expect_100,
				    handler, cancel_ptr);
	}

	void InjectSocketFailure() noexcept override {
		socket.Shutdown();
	}
};

template<typename SocketFilterFactory>
struct HttpClientFactory {
	static constexpr bool can_cancel_request_body = false;

	[[no_unique_address]]
	SocketFilterFactory &socket_filter_factory;

	HttpClientFactory(SocketFilterFactory &_socket_filter_factory) noexcept
		:socket_filter_factory(_socket_filter_factory) {}

	HttpClientConnection *New(EventLoop &event_loop,
				  const char *path, const char *mode) noexcept;

	auto *NewWithServer(struct pool &pool,
			    EventLoop &event_loop,
			    DemoHttpServerConnection::Mode mode) noexcept {
		return new HttpClientConnection(event_loop,
						Server::New(pool, event_loop, mode),
						socket_filter_factory());
	}

	auto *NewMirror(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::MIRROR);
	}

	auto *NewDeferMirror(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::DEFER_MIRROR);
	}

	auto *NewNull(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::MODE_NULL);
	}

	auto *NewDummy(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::DUMMY);
	}

	auto *NewClose(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::CLOSE);
	}

	auto *NewFixed(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::FIXED);
	}

	auto *NewTiny(struct pool &p, EventLoop &event_loop) noexcept {
		return NewFixed(p, event_loop);
	}

	auto *NewHuge(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::HUGE_);
	}

	auto *NewTwice100(struct pool &, EventLoop &event_loop) noexcept {
		return New(event_loop, "./test/twice_100.sh", nullptr);
	}

	HttpClientConnection *NewClose100(struct pool &,
					  EventLoop &event_loop) noexcept;

	auto *NewHold(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::HOLD);
	}

	auto *NewBlock(struct pool &pool,
		       EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::BLOCK);
	}

	auto *NewNop(struct pool &pool, EventLoop &event_loop) noexcept {
		return NewWithServer(pool, event_loop,
				     DemoHttpServerConnection::Mode::NOP);
	}

	auto *NewIgnoredRequestBody(struct pool &, EventLoop &event_loop) noexcept {
		return New(event_loop, "./test/ignored_request_body.sh", nullptr);
	}
};

HttpClientConnection::~HttpClientConnection() noexcept
{
	// TODO code copied from ~FilteredSocket()
	if (socket.IsValid()) {
		if (socket.IsConnected())
			socket.Close();
		socket.Destroy();
	}

	if (pid > 0) {
		int status;
		if (waitpid(pid, &status, 0) < 0) {
			perror("waitpid() failed");
			exit(EXIT_FAILURE);
		}

		assert(!WIFSIGNALED(status));
	}
}

template<typename SocketFilterFactory>
HttpClientConnection *
HttpClientFactory<SocketFilterFactory>::New(EventLoop &event_loop,
					    const char *path, const char *mode) noexcept
{
	SocketDescriptor client_socket, server_socket;
	if (!SocketDescriptor::CreateSocketPair(AF_LOCAL, SOCK_STREAM, 0,
						client_socket, server_socket)) {
		perror("socketpair() failed");
		exit(EXIT_FAILURE);
	}

	const auto pid = fork();
	if (pid < 0) {
		perror("fork() failed");
		exit(EXIT_FAILURE);
	}

	if (pid == 0) {
		server_socket.CheckDuplicate(FileDescriptor(STDIN_FILENO));
		server_socket.CheckDuplicate(FileDescriptor(STDOUT_FILENO));

		execl(path, path,
		      "0", "0", mode, nullptr);

		const char *srcdir = getenv("srcdir");
		if (srcdir != nullptr) {
			/* support automake out-of-tree build */
			if (chdir(srcdir) == 0)
				execl(path, path,
				      "0", "0", mode, nullptr);
		}

		perror("exec() failed");
		_exit(EXIT_FAILURE);
	}

	server_socket.Close();
	client_socket.SetNonBlocking();
	return new HttpClientConnection(event_loop, pid, client_socket,
					socket_filter_factory());
}

template<typename SocketFilterFactory>
HttpClientConnection *
HttpClientFactory<SocketFilterFactory>::NewClose100(struct pool &, EventLoop &event_loop) noexcept
{
	SocketDescriptor client_socket, server_socket;
	if (!SocketDescriptor::CreateSocketPair(AF_LOCAL, SOCK_STREAM, 0,
						client_socket, server_socket)) {
		perror("socketpair() failed");
		exit(EXIT_FAILURE);
	}

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork() failed");
		exit(EXIT_FAILURE);
	}

	if (pid == 0) {
		client_socket.Close();

		static const char response[] = "HTTP/1.1 100 Continue\n\n";
		(void)server_socket.Write(response, sizeof(response) - 1);
		server_socket.ShutdownWrite();

		char buffer[64];
		while (server_socket.Read(buffer, sizeof(buffer)) > 0) {}

		_exit(EXIT_SUCCESS);
	}

	server_socket.Close();
	client_socket.SetNonBlocking();
	return new HttpClientConnection(event_loop, pid, client_socket,
					socket_filter_factory());
}

/**
 * Keep-alive disabled, and response body has unknown length, ends
 * when server closes socket.  Check if our HTTP client handles such
 * responses correctly.
 */
static void
test_no_keepalive(auto &factory, Context &c) noexcept
{
	c.connection = factory.NewClose(*c.pool, c.event_loop);
	c.connection->Request(c.pool, c,
			      HTTP_METHOD_GET, "/foo", {},
			      nullptr,
#ifdef HAVE_EXPECT_100
			      false,
#endif
			      c, c.cancel_ptr);
	pool_commit();

	c.WaitForResponse();

	assert(c.status == HTTP_STATUS_OK);
	assert(c.request_error == nullptr);

	/* receive the rest of the response body from the buffer */
	c.event_loop.Dispatch();

	assert(c.released);
	assert(c.body_eof);
	assert(c.body_data > 0);
	assert(c.body_error == nullptr);
}

/**
 * The server ignores the request body, and sends the whole response
 * (keep-alive enabled).  The HTTP client's response body handler
 * blocks, and then more request body data becomes available.  This
 * used to trigger an assertion failure, because the HTTP client
 * forgot about the in-progress request body.
 */
static void
test_ignored_request_body(auto &factory, Context &c) noexcept
{
	auto delayed = istream_delayed_new(*c.pool, c.event_loop);
	AbortFlag abort_flag(delayed.second.cancel_ptr);
	auto zero = istream_zero_new(*c.pool);

	c.data_blocking = 1;
	c.connection = factory.NewIgnoredRequestBody(*c.pool, c.event_loop);
	c.connection->Request(c.pool, c,
			      HTTP_METHOD_GET, "/ignored-request-body", {},
			      std::move(delayed.first),
#ifdef HAVE_EXPECT_100
			      false,
#endif
			      c, c.cancel_ptr);

	c.WaitForEnd();

	/* at this point, the HTTP client must have closed the request
	   body; but if it has not due to the bug, this will trigger
	   the assertion failure: */
	if (!abort_flag.aborted) {
		delayed.second.Set(std::move(zero));
		c.event_loop.Dispatch();
	}

	assert(abort_flag.aborted);

	assert(c.released);
	assert(c.connection == nullptr);
	assert(c.status == HTTP_STATUS_OK);
	assert(c.consumed_body_data == 3);
	assert(c.body_error == nullptr);
	assert(!c.reuse);
}

static char *
RandomString(AllocatorPtr alloc, std::size_t length) noexcept
{
	char *p = alloc.NewArray<char>(length + 1), *q = p;
	for (std::size_t i = 0; i < length; ++i)
		*q++ = 'A' + (i % 26);
	*q = 0;
	return p;
}

static PipeLease
FillPipeLease(struct pool &pool, PipeStock *stock,
	      std::size_t length)
{
	PipeLease pl(stock);
	pl.Create();

	char *data = RandomString(pool, length);
	auto nbytes = pl.GetWriteFd().Write(data, length);
	if (nbytes < 0)
		throw MakeErrno("Failed to write to pipe");

	if (std::size_t(nbytes) < length)
		throw std::runtime_error("Short write to pipe");

	return pl;
}

static UnusedIstreamPtr
FillPipeLeaseIstream(struct pool &pool, PipeStock *stock,
		     std::size_t length)
{
	return NewIstreamPtr<PipeLeaseIstream>(pool,
					       FillPipeLease(pool, stock,
							     length),
					       length);
}

/**
 * Send a request with "Expect: 100-continue" with a request body that
 * can be spliced.
 */
static void
test_expect_100_continue_splice(auto &factory, Context &c) noexcept
{
	constexpr std::size_t length = 4096;

	c.connection = factory.NewDeferMirror(*c.pool, c.event_loop);
	c.connection->Request(c.pool, c,
			      HTTP_METHOD_POST, "/expect_100_continue_splice",
			      {},
			      NewIstreamPtr<DeferReadIstream>(*c.pool, c.event_loop,
							      FillPipeLeaseIstream(*c.pool, nullptr, length)),
			      true,
			      c, c.cancel_ptr);

	c.WaitForEnd();

	assert(c.released);
	assert(c.connection == nullptr);
	assert(c.status == HTTP_STATUS_OK);
	assert(c.consumed_body_data == length);
	assert(c.body_error == nullptr);
	assert(c.reuse);
}

/*
 * main
 *
 */

static void
RunHttpClientTests(Instance &instance, auto &socket_filter_factory) noexcept
{
	HttpClientFactory factory{socket_filter_factory};

	run_all_tests(instance, factory);
	run_test(instance, factory, test_no_keepalive);
	run_test(instance, factory, test_ignored_request_body);
	run_test(instance, factory, test_expect_100_continue_splice);
}

struct NullSocketFilterFactory {
	SocketFilterPtr operator()() const noexcept {
		return {};
	}
};

struct NopSocketFilterFactory {
	SocketFilterPtr operator()() const noexcept {
		return SocketFilterPtr{new NopSocketFilter()};
	}
};

struct NopThreadSocketFilterFactory {
	EventLoop &event_loop;

	explicit NopThreadSocketFilterFactory(EventLoop &_event_loop) noexcept
		:event_loop(_event_loop) {
		/* keep the eventfd unregistered if the ThreadQueue is
		   empty, so EventLoop::Dispatch() doesn't keep
		   running after the HTTP request has completed */
		thread_pool_set_volatile();
	}

	~NopThreadSocketFilterFactory() noexcept {
		thread_pool_stop();
		thread_pool_join();
		thread_pool_deinit();
	}

	SocketFilterPtr operator()() const noexcept {
		return SocketFilterPtr{
			new ThreadSocketFilter(event_loop,
					       thread_pool_get_queue(event_loop),
					       std::make_unique<NopThreadSocketFilter>())
		};
	}
};

int
main(int, char **)
{
	SetupProcess();

	direct_global_init();
	const ScopeFbPoolInit fb_pool_init;

	Instance instance;

	{
		NullSocketFilterFactory socket_filter_factory;
		RunHttpClientTests(instance, socket_filter_factory);
	}

	{
		NopSocketFilterFactory socket_filter_factory;
		RunHttpClientTests(instance, socket_filter_factory);
	}

	{
		NopThreadSocketFilterFactory socket_filter_factory{instance.event_loop};
		RunHttpClientTests(instance, socket_filter_factory);
	}
}
