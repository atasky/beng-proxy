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

#include "Serialize.hxx"
#include "Protocol.hxx"
#include "memory/GrowingBuffer.hxx"
#include "strmap.hxx"
#include "util/CharUtil.hxx"
#include "util/ByteOrder.hxx"
#include "util/StringView.hxx"

#include <cassert>
#include <cstdint>

FcgiRecordSerializer::FcgiRecordSerializer(GrowingBuffer &_buffer,
					   uint8_t type,
					   uint16_t request_id_be) noexcept
	:buffer(_buffer),
	 header((struct fcgi_record_header *)buffer.Write(sizeof(*header)))
{
	header->version = FCGI_VERSION_1;
	header->type = type;
	header->request_id = request_id_be;
	header->padding_length = 0;
	header->reserved = 0;
}

void
FcgiRecordSerializer::Commit(size_t content_length) noexcept
{
	assert(content_length < (1 << 16));
	header->content_length = ToBE16(content_length);
}

static size_t
fcgi_serialize_length(GrowingBuffer &gb, std::size_t length) noexcept
{
	if (length < 0x80) {
		uint8_t buffer = (uint8_t)length;
		gb.WriteT(buffer);
		return sizeof(buffer);
	} else {
		/* XXX 31 bit overflow? */
		uint32_t buffer = ToBE32(length | 0x80000000);
		gb.WriteT(buffer);
		return sizeof(buffer);
	}
}

static size_t
fcgi_serialize_pair(GrowingBuffer &gb, std::string_view name,
		    std::string_view value) noexcept
{
	std::size_t size = fcgi_serialize_length(gb, name.size());
	size += fcgi_serialize_length(gb, value.size());

	gb.Write(name);
	gb.Write(value);

	return size + name.size() + value.size();
}

FcgiParamsSerializer::FcgiParamsSerializer(GrowingBuffer &_buffer,
					   uint16_t request_id_be) noexcept
	:record(_buffer, FCGI_PARAMS, request_id_be) {}

FcgiParamsSerializer &
FcgiParamsSerializer::operator()(StringView name,
				 StringView value) noexcept
{
	content_length += fcgi_serialize_pair(record.GetBuffer(), name, value);
	return *this;
}

void
FcgiParamsSerializer::Headers(const StringMap &headers) noexcept
{
	char buffer[512] = "HTTP_";

	for (const auto &pair : headers) {
		size_t i;

		for (i = 0; 5 + i < sizeof(buffer) - 1 && pair.key[i] != 0; ++i) {
			if (IsLowerAlphaASCII(pair.key[i]))
				buffer[5 + i] = (char)(pair.key[i] - 'a' + 'A');
			else if (IsUpperAlphaASCII(pair.key[i]) ||
				 IsDigitASCII(pair.key[i]))
				buffer[5 + i] = pair.key[i];
			else
				buffer[5 + i] = '_';
		}

		(*this)({buffer, 5 + i}, pair.value);
	}
}
