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

#include "SinkGrowingBuffer.hxx"
#include "istream/Bucket.hxx"
#include "io/FileDescriptor.hxx"

#include <algorithm>

#include <string.h>
#include <unistd.h>

bool
GrowingBufferSink::OnIstreamReady() noexcept
{
	IstreamBucketList list;

	try {
		input.FillBucketList(list);
	} catch (...) {
		input.Clear();
		handler.OnGrowingBufferSinkError(std::current_exception());
		return false;
	}

	std::size_t nbytes = 0;
	bool more = list.HasMore();

	for (const auto &bucket : list) {
		if (!bucket.IsBuffer()) {
			more = true;
			break;
		}

		auto r = bucket.GetBuffer();
		buffer.Write(r);
		nbytes += r.size();
	}

	if (nbytes > 0)
		input.ConsumeBucketList(nbytes);

	if (!more) {
		CloseInput();
		handler.OnGrowingBufferSinkEof(std::move(buffer));
		return false;
	}

	return true;
}

std::size_t
GrowingBufferSink::OnData(std::span<const std::byte> src) noexcept
{
	buffer.Write(src);
	return src.size();
}

IstreamDirectResult
GrowingBufferSink::OnDirect(FdType, FileDescriptor fd, off_t offset,
			    std::size_t max_length) noexcept
{
	auto w = buffer.BeginWrite();
	const std::size_t n = std::min(w.size(), max_length);

	ssize_t nbytes = HasOffset(offset)
		? fd.ReadAt(offset, w.data(), n)
		: fd.Read(w.data(), n);
	if (nbytes <= 0)
		return nbytes < 0
			? IstreamDirectResult::ERRNO
			: IstreamDirectResult::END;

	input.ConsumeDirect(nbytes);
	buffer.CommitWrite(nbytes);

	return IstreamDirectResult::OK;
}

void
GrowingBufferSink::OnEof() noexcept
{
	input.Clear();
	handler.OnGrowingBufferSinkEof(std::move(buffer));
}

void
GrowingBufferSink::OnError(std::exception_ptr error) noexcept
{
	input.Clear();
	handler.OnGrowingBufferSinkError(std::move(error));
}
