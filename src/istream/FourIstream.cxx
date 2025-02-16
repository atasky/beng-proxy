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

#include "FourIstream.hxx"
#include "ForwardIstream.hxx"
#include "UnusedPtr.hxx"
#include "New.hxx"
#include "Bucket.hxx"
#include "io/FileDescriptor.hxx"

#include <algorithm>

class FourIstream final : public ForwardIstream {
public:
	FourIstream(struct pool &p, UnusedIstreamPtr _input)
		:ForwardIstream(p, std::move(_input)) {}

	/* virtual methods from class Istream */

	off_t _GetAvailable([[maybe_unused]] bool partial) noexcept override {
		return -1;
	}

	off_t _Skip([[maybe_unused]] off_t length) noexcept override {
		return -1;
	}

	void _FillBucketList(IstreamBucketList &list) override {
		IstreamBucketList tmp;

		try {
			input.FillBucketList(tmp);
		} catch (...) {
			Destroy();
			throw;
		}

		list.SpliceBuffersFrom(std::move(tmp), 4);
	}

	int _AsFd() noexcept override {
		return -1;
	}

	/* virtual methods from class IstreamHandler */

	std::size_t OnData(std::span<const std::byte> src) noexcept override {
		if (src.size() > 4)
			src = src.first(4);

		return ForwardIstream::OnData(src);
	}

	IstreamDirectResult OnDirect(FdType type, FileDescriptor fd,
				     off_t offset,
				     std::size_t max_length) noexcept override {
		return ForwardIstream::OnDirect(type, fd, offset,
						std::min(max_length, std::size_t{4}));
	}
};

UnusedIstreamPtr
istream_four_new(struct pool *pool, UnusedIstreamPtr input) noexcept
{
	return NewIstreamPtr<FourIstream>(*pool, std::move(input));
}
