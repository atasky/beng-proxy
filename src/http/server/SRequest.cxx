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

#include "Internal.hxx"
#include "Request.hxx"

BufferedResult
HttpServerConnection::FeedRequestBody(std::span<const std::byte> src) noexcept
{
	assert(request.read_state == Request::BODY);
	assert(request.body_state == Request::BodyState::READING);
	assert(!response.pending_drained);

	const DestructObserver destructed(*this);

	std::size_t nbytes = request_body_reader->FeedBody(src);
	if (nbytes == 0) {
		if (destructed)
			return BufferedResult::CLOSED;

		read_timer.Cancel();
		return BufferedResult::OK;
	}

	request.bytes_received += nbytes;
	socket->DisposeConsumed(nbytes);

	assert(request.read_state == Request::BODY);

	if (request_body_reader->IsEOF()) {
		request.read_state = Request::END;
#ifndef NDEBUG
		request.body_state = Request::BodyState::CLOSED;
#endif

		read_timer.Cancel();

		if (socket->IsConnected())
			socket->SetDirect(false);

		request.request->stopwatch.RecordEvent("request_end");

		request_body_reader->DestroyEof();
		if (destructed)
			return BufferedResult::CLOSED;
	} else
		/* refresh the request body timeout */
		ScheduleReadTimeoutTimer();

	return BufferedResult::OK;
}

void
HttpServerConnection::DiscardRequestBody() noexcept
{
	assert(request.read_state == Request::BODY);
	assert(request.body_state == Request::BodyState::READING);
	assert(!response.pending_drained);

	if (!socket->IsValid() || !socket->IsConnected()) {
		/* this happens when there's an error on the socket while
		   reading the request body before the response gets
		   submitted, and this HTTP server library invokes the
		   handler's abort method; the handler will free the request
		   body, but the socket is already closed */
		assert(request.request == nullptr);
	}

	request.read_state = Request::END;
#ifndef NDEBUG
	request.body_state = Request::BodyState::CLOSED;
#endif

	read_timer.Cancel();

	if (socket->IsConnected())
		socket->SetDirect(false);

	if (request.expect_100_continue)
		/* the request body was optional, and we did not send the "100
		   Continue" response (yet): pretend there never was a request
		   body */
		request.expect_100_continue = false;
	else if (request_body_reader->Discard(*socket))
		/* the remaining data has already been received into the input
		   buffer, and we only need to discard it from there to have a
		   "clean" connection */
		return;
	else
		/* disable keep-alive so we don't need to wait for the client
		   to finish sending the request body */
		keep_alive = false;
}

off_t
HttpServerConnection::RequestBodyReader::_GetAvailable(bool partial) noexcept
{
	assert(connection.IsValid());
	assert(connection.request.read_state == Request::BODY);
	assert(connection.request.body_state == Request::BodyState::READING);
	assert(!connection.response.pending_drained);

	return HttpBodyReader::GetAvailable(*connection.socket, partial);
}

inline void
HttpServerConnection::ReadRequestBody() noexcept
{
	assert(IsValid());
	assert(request.read_state == Request::BODY);
	assert(request.body_state == Request::BodyState::READING);
	assert(!response.pending_drained);

	if (!MaybeSend100Continue())
		return;

	if (request.in_handler)
		/* avoid recursion */
		return;

	if (socket->IsConnected())
		socket->SetDirect(request_body_reader->CheckDirect(socket->GetType()));

	socket->Read();
}

void
HttpServerConnection::RequestBodyReader::_Read() noexcept
{
	connection.ReadRequestBody();
}

void
HttpServerConnection::RequestBodyReader::_ConsumeDirect(std::size_t nbytes) noexcept
{
	HttpBodyReader::_ConsumeDirect(nbytes);

	connection.request.bytes_received += nbytes;
}

void
HttpServerConnection::RequestBodyReader::_Close() noexcept
{
	if (connection.request.read_state == Request::END)
		return;

	if (connection.request.request != nullptr)
		connection.request.request->stopwatch.RecordEvent("close");

	connection.DiscardRequestBody();

	Destroy();
}
