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

#include "PrometheusExporter.hxx"
#include "Instance.hxx"
#include "prometheus/Stats.hxx"
#include "prometheus/HttpStats.hxx"
#include "beng-proxy/Control.hxx"
#include "http/Headers.hxx"
#include "http/IncomingRequest.hxx"
#include "http/ResponseHandler.hxx"
#include "memory/istream_gb.hxx"
#include "memory/GrowingBuffer.hxx"

void
BpPrometheusExporter::HandleHttpRequest(IncomingHttpRequest &request,
					const StopwatchPtr &,
					CancellablePointer &) noexcept
{
	GrowingBuffer buffer;

	const char *process = "bp";
	Prometheus::Write(buffer, process, instance.GetStats());

	for (const auto &[name, stats] : instance.listener_stats)
		Prometheus::Write(buffer, process, name.c_str(), stats);

	HttpHeaders headers;
	headers.Write("content-type", "text/plain;version=0.0.4");

	request.SendResponse(HTTP_STATUS_OK, std::move(headers),
			     istream_gb_new(request.pool, std::move(buffer)));
}
