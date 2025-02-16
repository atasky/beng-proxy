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

#pragma once

#include "event/Chrono.hxx"
#include "util/BindMethod.hxx"

#include <span>

#include <sys/types.h>
#include <stddef.h>

enum class BufferedResult;
class FilteredSocket;

class SocketFilter {
public:
	virtual void Init(FilteredSocket &_socket) noexcept = 0;

	/**
	 * @see FilteredSocket::SetHandshakeCallback()
	 */
	virtual void SetHandshakeCallback(BoundMethod<void() noexcept> callback) noexcept {
		callback();
	}

	/**
	 * Data has been read from the socket into the input buffer.  Call
	 * FilteredSocket::InternalReadBuffer() and
	 * FilteredSocket::InternalConsumed() to process data from the
	 * buffer.
	 */
	virtual BufferedResult OnData() noexcept = 0;

	virtual bool IsEmpty() const noexcept = 0;

	virtual bool IsFull() const noexcept = 0;

	virtual std::size_t GetAvailable() const noexcept = 0;

	virtual std::span<std::byte> ReadBuffer() noexcept = 0;

	virtual void Consumed(std::size_t nbytes) noexcept = 0;

	virtual void AfterConsumed() noexcept = 0;

	/**
	 * The client asks to read more data.  The filter shall call
	 * FilteredSocket::InvokeData() again.
	 */
	virtual bool Read() noexcept = 0;

	/**
	 * The client asks to write data to the socket.  The filter
	 * processes it, and may then call
	 * FilteredSocket::InvokeWrite().
	 */
	virtual ssize_t Write(std::span<const std::byte> src) noexcept = 0;

	/**
	 * The client is willing to read, but does not expect it yet.  The
	 * filter processes the call, and may then call
	 * FilteredSocket::InternalScheduleRead().
	 */
	virtual void ScheduleRead() noexcept = 0;

	/**
	 * The client wants to be called back as soon as writing becomes
	 * possible.  The filter processes the call, and may then call
	 * FilteredSocket::InternalScheduleWrite().
	 */
	virtual void ScheduleWrite() noexcept = 0;

	/**
	 * The client is not anymore interested in writing.  The filter
	 * processes the call, and may then call
	 * FilteredSocket::InternalUnscheduleWrite().
	 */
	virtual void UnscheduleWrite() noexcept = 0;

	/**
	 * The underlying socket is ready for writing.  The filter may try
	 * calling FilteredSocket::InternalWrite() again.
	 *
	 * This method must not destroy the socket.  If an error occurs,
	 * it shall return false.
	 */
	virtual bool InternalWrite() noexcept = 0;

	/**
	 * Called after the socket has been closed/abandoned (either by
	 * the peer or locally).  The filter shall update its internal
	 * state, but not do any invasive actions.
	 */
	virtual void OnClosed() noexcept {}

	virtual bool OnRemaining(std::size_t remaining) noexcept = 0;

	/**
	 * The buffered_socket has run empty after the socket has been
	 * closed.  The filter may call FilteredSocket::InvokeEnd() as
	 * soon as all its buffers have been consumed.
	 */
	virtual void OnEnd() noexcept = 0;

	virtual void Close() noexcept = 0;
};
