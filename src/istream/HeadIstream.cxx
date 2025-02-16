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

#include "HeadIstream.hxx"
#include "ForwardIstream.hxx"
#include "UnusedPtr.hxx"
#include "Bucket.hxx"
#include "New.hxx"
#include "io/FileDescriptor.hxx"

#include <algorithm>

#include <assert.h>

class HeadIstream final : public ForwardIstream {
	off_t rest;
	const bool authoritative;

public:
	HeadIstream(struct pool &p, UnusedIstreamPtr _input,
		    std::size_t size, bool _authoritative) noexcept
		:ForwardIstream(p, std::move(_input)),
		 rest(size), authoritative(_authoritative) {}

	/* virtual methods from class Istream */

	off_t _GetAvailable(bool partial) noexcept override;
	std::size_t _ConsumeBucketList(std::size_t nbytes) noexcept override;
	void _ConsumeDirect(std::size_t nbytes) noexcept override;
	off_t _Skip(off_t length) noexcept override;
	void _Read() noexcept override;

	void _FillBucketList(IstreamBucketList &list) override;

	int _AsFd() noexcept override {
		return -1;
	}

	/* virtual methods from class IstreamHandler */
	std::size_t OnData(std::span<const std::byte> src) noexcept override;
	IstreamDirectResult OnDirect(FdType type, FileDescriptor fd,
				     off_t offset,
				     std::size_t max_length) noexcept override;
};

/*
 * istream handler
 *
 */

std::size_t
HeadIstream::OnData(std::span<const std::byte> src) noexcept
{
	if (rest == 0) {
		DestroyEof();
		return 0;
	}

	if ((off_t)src.size() > rest)
		src = src.first(rest);

	std::size_t nbytes = InvokeData(src);
	assert((off_t)nbytes <= rest);

	if (nbytes > 0) {
		rest -= nbytes;
		if (rest == 0) {
			DestroyEof();
			return 0;
		}
	}

	return nbytes;
}

void
HeadIstream::_FillBucketList(IstreamBucketList &list)
{
	if (rest == 0)
		return;

	IstreamBucketList tmp1;

	try {
		input.FillBucketList(tmp1);
	} catch (...) {
		Destroy();
		throw;
	}

	std::size_t nbytes = list.SpliceBuffersFrom(std::move(tmp1), rest);
	if ((off_t)nbytes >= rest)
		list.SetMore(false);
}

std::size_t
HeadIstream::_ConsumeBucketList(std::size_t nbytes) noexcept
{
	if ((off_t)nbytes > rest)
		nbytes = rest;

	nbytes = ForwardIstream::_ConsumeBucketList(nbytes);
	rest -= nbytes;
	return nbytes;
}

void
HeadIstream::_ConsumeDirect(std::size_t nbytes) noexcept
{
	assert((off_t)nbytes <= rest);

	rest -= nbytes;
	ForwardIstream::_ConsumeBucketList(nbytes);
}

IstreamDirectResult
HeadIstream::OnDirect(FdType type, FileDescriptor fd, off_t offset,
		      std::size_t max_length) noexcept
{
	if (rest == 0) {
		DestroyEof();
		return IstreamDirectResult::CLOSED;
	}

	if ((off_t)max_length > rest)
		max_length = rest;

	const auto result = InvokeDirect(type, fd, offset, max_length);

	if (result == IstreamDirectResult::OK && rest == 0) {
		DestroyEof();
		return IstreamDirectResult::CLOSED;
	}

	return result;
}

/*
 * istream implementation
 *
 */

off_t
HeadIstream::_GetAvailable(bool partial) noexcept
{
	if (authoritative) {
		assert(partial ||
		       input.GetAvailable(partial) < 0 ||
		       input.GetAvailable(partial) >= (off_t)rest);
		return rest;
	}

	off_t available = input.GetAvailable(partial);
	return std::min(available, rest);
}

off_t
HeadIstream::_Skip(off_t length) noexcept
{
	if (length >= rest)
		length = rest;

	off_t nbytes = ForwardIstream::_Skip(length);
	assert(nbytes <= length);

	if (nbytes > 0)
		rest -= nbytes;

	return nbytes;
}

void
HeadIstream::_Read() noexcept
{
	if (rest == 0) {
		DestroyEof();
	} else {
		ForwardIstream::_Read();
	}
}

/*
 * constructor
 *
 */

UnusedIstreamPtr
istream_head_new(struct pool &pool, UnusedIstreamPtr input,
		 std::size_t size, bool authoritative) noexcept
{
	return NewIstreamPtr<HeadIstream>(pool, std::move(input),
					  size, authoritative);
}
