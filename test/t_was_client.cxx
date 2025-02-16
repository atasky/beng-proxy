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

#define HAVE_CHUNKED_REQUEST_BODY
#define ENABLE_HUGE_BODY
#define ENABLE_VALID_PREMATURE
#define ENABLE_MALFORMED_PREMATURE
#define NO_EARLY_RELEASE_SOCKET // TODO: improve the WAS client

#include "t_client.hxx"
#include "tio.hxx"
#include "stopwatch.hxx"
#include "was/Client.hxx"
#include "was/Server.hxx"
#include "was/Lease.hxx"
#include "was/async/Socket.hxx"
#include "system/SetupProcess.hxx"
#include "io/FileDescriptor.hxx"
#include "io/SpliceSupport.hxx"
#include "net/SocketDescriptor.hxx"
#include "lease.hxx"
#include "istream/UnusedPtr.hxx"
#include "istream/SuspendIstream.hxx"
#include "event/FineTimerEvent.hxx"
#include "util/ConstBuffer.hxx"
#include "util/ByteOrder.hxx"
#include "AllocatorPtr.hxx"
#include "memory/fb_pool.hxx"
#include "strmap.hxx"

#include <functional>

static void
RunNull(WasServer &server, struct pool &,
	http_method_t,
	const char *, StringMap &&,
	UnusedIstreamPtr body)
{
	body.Clear();

	server.SendResponse(HTTP_STATUS_NO_CONTENT, {}, nullptr);
}

static void
RunHello(WasServer &server, struct pool &pool,
	 http_method_t,
	 const char *, StringMap &&,
	 UnusedIstreamPtr body)
{
	body.Clear();

	server.SendResponse(HTTP_STATUS_OK, {},
			    istream_string_new(pool, "hello"));
}

static void
RunHuge(WasServer &server, struct pool &pool,
	http_method_t ,
	const char *, StringMap &&,
	UnusedIstreamPtr body)
{
	body.Clear();

	server.SendResponse(HTTP_STATUS_OK, {},
			    istream_head_new(pool,
					     istream_zero_new(pool),
					     524288, true));
}

static void
RunHold(WasServer &server, struct pool &pool,
	http_method_t,
	const char *, StringMap &&,
	UnusedIstreamPtr body)
{
	body.Clear();

	server.SendResponse(HTTP_STATUS_OK, {},
			    istream_block_new(pool));
}

static void
RunBlock(WasServer &server, struct pool &pool,
	http_method_t,
	const char *, StringMap &&,
	UnusedIstreamPtr body)
{
	body.Clear();

	server.SendResponse(HTTP_STATUS_OK, {},
			    istream_block_new(pool));
}

static void
RunNop(WasServer &, struct pool &,
       http_method_t ,
       const char *, StringMap &&,
       UnusedIstreamPtr) noexcept
{
}

static void
RunMirror(WasServer &server, struct pool &,
	  http_method_t,
	  const char *, StringMap &&headers,
	  UnusedIstreamPtr body)
{
	const bool has_body = body;
	server.SendResponse(has_body ? HTTP_STATUS_OK : HTTP_STATUS_NO_CONTENT,
			    std::move(headers), std::move(body));
}

static void
RunMalformedHeaderName(WasServer &server, struct pool &pool,
		       http_method_t, const char *, StringMap &&,
		       UnusedIstreamPtr body)
{
	body.Clear();

	StringMap response_headers(pool, {{"header name", "foo"}});

	server.SendResponse(HTTP_STATUS_NO_CONTENT,
			    std::move(response_headers), nullptr);
}

static void
RunMalformedHeaderValue(WasServer &server, struct pool &pool,
			http_method_t, const char *, StringMap &&,
			UnusedIstreamPtr body)
{
	body.Clear();

	StringMap response_headers(pool, {{"name", "foo\nbar"}});

	server.SendResponse(HTTP_STATUS_NO_CONTENT,
			    std::move(response_headers), nullptr);
}

static void
RunValidPremature(WasServer &server, struct pool &pool,
		  http_method_t,
		  const char *, StringMap &&,
		  UnusedIstreamPtr body)
{
	body.Clear();

	server.SendResponse(HTTP_STATUS_OK, {},
			    NewConcatIstream(pool,
					     istream_head_new(pool,
							      istream_zero_new(pool),
							      512, true),
					     NewSuspendIstream(pool, istream_fail_new(pool, std::make_exception_ptr(std::runtime_error("Error"))),
							       server.GetEventLoop(),
							       std::chrono::milliseconds(10))));
}

class MalformedPrematureWasServer final : Was::ControlHandler {
	WasSocket socket;

	Was::Control control;

	FineTimerEvent defer_premature;

	WasServerHandler &handler;

public:
	MalformedPrematureWasServer(EventLoop &event_loop,
				    WasSocket &&_socket,
				    WasServerHandler &_handler) noexcept
		:socket(std::move(_socket)),
		 control(event_loop, socket.control, *this),
		 defer_premature(event_loop, BIND_THIS_METHOD(SendPremature)),
		 handler(_handler)
	{
	}

	void Free() noexcept {
		ReleaseError();
	}

	void SendResponse(http_status_t status,
			  StringMap &&headers, UnusedIstreamPtr body) noexcept;

private:
	void Destroy() noexcept {
		this->~MalformedPrematureWasServer();
	}

	void ReleaseError() noexcept {
		if (control.IsDefined())
			control.ReleaseSocket();

		socket.Close();
		Destroy();
	}

	void ReleaseUnused() noexcept;

	void AbortError() noexcept {
		auto &handler2 = handler;
		ReleaseError();
		handler2.OnWasClosed();
	}

	/**
	 * Abort receiving the response status/headers from the WAS server.
	 */
	void AbortUnused() noexcept {
		auto &handler2 = handler;
		ReleaseUnused();
		handler2.OnWasClosed();
	}

protected:
	void SendPremature() noexcept {
		/* the response body was announced as 1 kB - and now
		   we tell the client he already sent 4 kB */
		control.SendUint64(WAS_COMMAND_PREMATURE, 4096);
	}

	/* virtual methods from class Was::ControlHandler */
	bool OnWasControlPacket(enum was_command cmd,
				std::span<const std::byte> payload) noexcept override;
	bool OnWasControlDrained() noexcept override {
		return true;
	}
	void OnWasControlDone() noexcept override {}
	void OnWasControlError(std::exception_ptr) noexcept override {
		AbortError();
	}
};

bool
MalformedPrematureWasServer::OnWasControlPacket(enum was_command cmd,
						std::span<const std::byte> payload) noexcept
{
	(void)payload;

	switch (cmd) {
	case WAS_COMMAND_NOP:
	case WAS_COMMAND_REQUEST:
	case WAS_COMMAND_METHOD:
	case WAS_COMMAND_URI:
	case WAS_COMMAND_SCRIPT_NAME:
	case WAS_COMMAND_PATH_INFO:
	case WAS_COMMAND_QUERY_STRING:
	case WAS_COMMAND_HEADER:
	case WAS_COMMAND_PARAMETER:
	case WAS_COMMAND_REMOTE_HOST:
		break;

	case WAS_COMMAND_STATUS:
		AbortError();
		return false;

	case WAS_COMMAND_NO_DATA:
	case WAS_COMMAND_DATA:
		/* announce a response body of 1 kB */
		if (!control.SendEmpty(WAS_COMMAND_DATA) ||
		    !control.SendUint64(WAS_COMMAND_LENGTH, 1024))
			return false;

		defer_premature.Schedule(std::chrono::milliseconds(1));
		return true;

	case WAS_COMMAND_LENGTH:
		break;

	case WAS_COMMAND_STOP:
	case WAS_COMMAND_PREMATURE:
		break;
	}

	return true;
}

class WasConnection final : public ClientConnection, WasServerHandler, WasLease
{
	EventLoop &event_loop;

	WasSocket socket;

	WasServer *server = nullptr;

	MalformedPrematureWasServer *server2 = nullptr;

	Lease *lease;

	typedef std::function<void(WasServer &server, struct pool &pool,
				   http_method_t method,
				   const char *uri, StringMap &&headers,
				   UnusedIstreamPtr body)> Callback;

	const Callback callback;

public:
	WasConnection(struct pool &pool, EventLoop &_event_loop,
		      Callback &&_callback)
		:event_loop(_event_loop),
		 callback(std::move(_callback))
	{
		WasServerHandler &handler = *this;
		server = NewFromPool<WasServer>(pool, pool, event_loop,
						MakeWasSocket(),
						handler);
	}

	struct MalformedPremature{};

	WasConnection(struct pool &pool, EventLoop &_event_loop,
		      MalformedPremature)
		:event_loop(_event_loop)
	{
		WasServerHandler &handler = *this;
		server2 = NewFromPool<MalformedPrematureWasServer>(pool, event_loop,
								   MakeWasSocket(),
								   handler);
	}

	~WasConnection() noexcept override {
		if (server != nullptr)
			server->Free();
		if (server2 != nullptr)
			server2->Free();
	}

	auto &GetEventLoop() const noexcept {
		return event_loop;
	}

	void Request(struct pool &pool,
		     Lease &_lease,
		     http_method_t method, const char *uri,
		     StringMap &&headers, UnusedIstreamPtr body,
		     [[maybe_unused]] bool expect_100,
		     HttpResponseHandler &handler,
		     CancellablePointer &cancel_ptr) noexcept override {
		lease = &_lease;
		was_client_request(pool, GetEventLoop(), nullptr,
				   socket.control, socket.input, socket.output,
				   *this,
				   nullptr,
				   method, uri, uri, nullptr, nullptr,
				   headers, std::move(body), {},
				   handler, cancel_ptr);
	}

	void InjectSocketFailure() noexcept override {
		socket.control.Shutdown();
	}

	/* virtual methods from class WasServerHandler */

	void OnWasRequest(struct pool &pool, http_method_t method,
			  const char *uri, StringMap &&headers,
			  UnusedIstreamPtr body) noexcept override {
		callback(*server, pool, method, uri,
			 std::move(headers), std::move(body));
	}

	void OnWasClosed() noexcept override {
		server = nullptr;
		server2 = nullptr;
	}

private:
	WasSocket MakeWasSocket() {
		auto s = WasSocket::CreatePair();

		socket = std::move(s.first);
		socket.input.SetNonBlocking();
		socket.output.SetNonBlocking();

		s.second.input.SetNonBlocking();
		s.second.output.SetNonBlocking();
		return std::move(s.second);
	}

	void OnCloseTimer() noexcept {
		if (server != nullptr)
			std::exchange(server, nullptr)->Free();
		if (server2 != nullptr)
			std::exchange(server2, nullptr)->Free();
	}

	/* virtual methods from class WasLease */
	void ReleaseWas(bool reuse) override {
		lease->ReleaseLease(reuse);
	}

	void ReleaseWasStop(uint64_t) override {
		ReleaseWas(false);
	}
};

struct WasFactory {
	static constexpr bool can_cancel_request_body = true;

	auto *NewMirror(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunMirror);
	}

	auto *NewNull(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunNull);
	}

	auto *NewDummy(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunHello);
	}

	auto *NewFixed(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunHello);
	}

	auto *NewTiny(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunHello);
	}

	auto *NewHuge(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunHuge);
	}

	auto *NewHold(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunHold);
	}

	auto *NewBlock(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunBlock);
	}

	auto *NewNop(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunNop);
	}

	auto *NewMalformedHeaderName(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunMalformedHeaderName);
	}

	auto *NewMalformedHeaderValue(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunMalformedHeaderValue);
	}

	auto *NewValidPremature(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop, RunValidPremature);
	}

	auto *NewMalformedPremature(struct pool &pool, EventLoop &event_loop) {
		return new WasConnection(pool, event_loop,
					 WasConnection::MalformedPremature{});
	}
};

static void
test_malformed_header_name(auto &factory, Context &c) noexcept
{
	c.connection = factory.NewMalformedHeaderName(*c.pool, c.event_loop);
	c.connection->Request(c.pool, c,
			      HTTP_METHOD_GET, "/foo", {},
			      nullptr,
			      false,

			      c, c.cancel_ptr);

	c.event_loop.Dispatch();

	assert(c.status == http_status_t(0));
	assert(c.request_error);
	assert(c.released);
}

static void
test_malformed_header_value(auto &factory, Context &c) noexcept
{
	c.connection = factory.NewMalformedHeaderValue(*c.pool, c.event_loop);
	c.connection->Request(c.pool, c,
			      HTTP_METHOD_GET, "/foo", {},
			      nullptr,
			      false,

			      c, c.cancel_ptr);

	c.event_loop.Dispatch();

	assert(c.status == http_status_t(0));
	assert(c.request_error);
	assert(c.released);
}

/*
 * main
 *
 */

int
main(int, char **)
{
	SetupProcess();
	direct_global_init();
	const ScopeFbPoolInit fb_pool_init;

	Instance instance;
	WasFactory factory;

	run_all_tests(instance, factory);
	run_test(instance, factory, test_malformed_header_name);
	run_test(instance, factory, test_malformed_header_value);
}
