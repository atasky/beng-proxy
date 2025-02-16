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

#include "FifoBufferIstream.hxx"
#include "Bucket.hxx"
#include "memory/fb_pool.hxx"

size_t
FifoBufferIstream::Push(std::span<const std::byte> src) noexcept
{
	buffer.AllocateIfNull(fb_pool_get());
	return buffer.MoveFrom(src);
}

void
FifoBufferIstream::SetEof() noexcept
{
	eof = true;
	SubmitBuffer();
}

void
FifoBufferIstream::SubmitBuffer() noexcept
{
	while (!buffer.empty()) {
		size_t nbytes = SendFromBuffer(buffer);
		if (nbytes == 0)
			return;

		if (!eof) {
			handler.OnFifoBufferIstreamConsumed(nbytes);
			if (buffer.empty())
				handler.OnFifoBufferIstreamDrained();
		}
	}

	if (buffer.empty()) {
		if (eof)
			DestroyEof();
		else
			buffer.FreeIfDefined();
	}
}

off_t
FifoBufferIstream::_Skip(off_t length) noexcept
{
	size_t nbytes = std::min<decltype(length)>(length, buffer.GetAvailable());
	buffer.Consume(nbytes);
	buffer.FreeIfEmpty();
	Consumed(nbytes);

	if (nbytes > 0 && !eof) {
		handler.OnFifoBufferIstreamConsumed(nbytes);
		if (buffer.empty())
			handler.OnFifoBufferIstreamDrained();
	}

	return nbytes;
}

void
FifoBufferIstream::_Read() noexcept
{
	SubmitBuffer();
}

void
FifoBufferIstream::_FillBucketList(IstreamBucketList &list) noexcept
{
	auto r = buffer.Read();
	if (!r.empty())
		list.Push(r);

	if (!eof)
		list.SetMore();
}

size_t
FifoBufferIstream::_ConsumeBucketList(size_t nbytes) noexcept
{
	size_t consumed = std::min(nbytes, buffer.GetAvailable());
	buffer.Consume(consumed);
	Consumed(consumed);

	if (consumed > 0 && !eof) {
		handler.OnFifoBufferIstreamConsumed(consumed);
		if (buffer.empty())
			handler.OnFifoBufferIstreamDrained();

		if (buffer.empty())
			buffer.Free();
	}

	return consumed;
}
