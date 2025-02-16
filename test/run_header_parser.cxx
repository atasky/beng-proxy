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

#include "pool/RootPool.hxx"
#include "http/HeaderParser.hxx"
#include "memory/GrowingBuffer.hxx"
#include "AllocatorPtr.hxx"
#include "strmap.hxx"

#include <unistd.h>
#include <stdio.h>

int
main(int argc gcc_unused, char **argv gcc_unused)
{
	std::byte buffer[16];
	ssize_t nbytes;

	RootPool pool;
	const AllocatorPtr alloc{pool};

	GrowingBuffer gb;

	/* read input from stdin */

	while ((nbytes = read(0, buffer, sizeof(buffer))) > 0)
		gb.Write(std::span{buffer}.first(nbytes));

	/* parse the headers */

	auto *headers = strmap_new(pool);
	header_parse_buffer(alloc, *headers, std::move(gb));

	/* dump headers */

	for (const auto &i : *headers)
		printf("%s: %s\n", i.key, i.value);
}
