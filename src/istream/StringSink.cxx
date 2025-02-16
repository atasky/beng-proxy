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

#include "StringSink.hxx"
#include "Sink.hxx"
#include "UnusedPtr.hxx"
#include "pool/pool.hxx"
#include "util/Cancellable.hxx"
#include "util/SpanCast.hxx"

class StringSink final : IstreamSink, Cancellable {
	std::string value;

	StringSinkHandler &handler;

public:
	StringSink(UnusedIstreamPtr &&_input,
		   StringSinkHandler &_handler,
		   CancellablePointer &cancel_ptr)
		:IstreamSink(std::move(_input)),
		 handler(_handler)
	{
		cancel_ptr = *this;
	}

	void Read() noexcept {
		input.Read();
	}

private:
	void Destroy() {
		this->~StringSink();
	}

	/* virtual methods from class Cancellable */
	void Cancel() noexcept override {
		Destroy();
	}

	/* virtual methods from class IstreamHandler */

	size_t OnData(std::span<const std::byte> src) noexcept override {
		value.append(ToStringView(src));
		return src.size();
	}

	void OnEof() noexcept override {
		ClearInput();

		auto &_handler = handler;
		auto _value = std::move(value);
		Destroy();
		_handler.OnStringSinkSuccess(std::move(_value));
	}

	void OnError(std::exception_ptr ep) noexcept override {
		ClearInput();

		auto &_handler = handler;
		Destroy();
		_handler.OnStringSinkError(std::move(ep));
	}
};

/*
 * constructor
 *
 */

StringSink &
NewStringSink(struct pool &pool, UnusedIstreamPtr input,
	      StringSinkHandler &handler, CancellablePointer &cancel_ptr)
{
	return *NewFromPool<StringSink>(pool, std::move(input),
					handler, cancel_ptr);
}

void
ReadStringSink(StringSink &sink) noexcept
{
	sink.Read();
}
