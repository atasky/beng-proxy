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

#include "ExternalSession.hxx"
#include "session/Session.hxx"
#include "Instance.hxx"
#include "http/Address.hxx"
#include "http/GlueClient.hxx"
#include "http/ResponseHandler.hxx"
#include "istream/UnusedPtr.hxx"
#include "AllocatorPtr.hxx"
#include "pool/Holder.hxx"
#include "io/Logger.hxx"
#include "util/Background.hxx"
#include "stopwatch.hxx"
#include "strmap.hxx"

class ExternalSessionRefresh final
	: PoolHolder, public BackgroundJob, HttpResponseHandler {

	const HttpAddress address;

public:
	ExternalSessionRefresh(PoolPtr &&_pool,
			       const HttpAddress &_address)
		:PoolHolder(std::move(_pool)),
		 address(GetPool(), _address) {}

	void SendRequest(BpInstance &instance, const SessionId session_id) {
		http_request(pool, instance.event_loop, *instance.fs_balancer,
			     nullptr,
			     session_id.GetClusterHash(),
			     nullptr,
			     HTTP_METHOD_GET, address,
			     {}, nullptr,
			     *this, cancel_ptr);
	}

	/* virtual methods from class HttpResponseHandler */
	void OnHttpResponse(http_status_t status,
			    gcc_unused StringMap &&headers,
			    UnusedIstreamPtr body) noexcept override {
		body.Clear();

		if (status < 200 || status >= 300)
			LogConcat(3, "ExternalSessionManager", "Status ", int(status),
				  " from manager '", address.path, "'");

		unlink();
	}

	void OnHttpError(std::exception_ptr ep) noexcept override {
		LogConcat(2, "ExternalSessionManager", "Failed to refresh external session: ", ep);

		unlink();
	}
};

void
RefreshExternalSession(BpInstance &instance, Session &session)
{
	if (session.external_manager == nullptr ||
	    session.external_keepalive <= std::chrono::seconds::zero())
		/* feature is not enabled */
		return;

	const auto now = instance.event_loop.SteadyNow();
	if (now < session.next_external_keepalive)
		/* not yet */
		return;

	LogConcat(5, "ExternalSessionManager", "refresh '",
		  session.external_manager->path, "'");

	session.next_external_keepalive = now + session.external_keepalive;

	auto pool = pool_new_linear(instance.root_pool, "external_session_refresh",
				    4096);

	auto *refresh = NewFromPool<ExternalSessionRefresh>(std::move(pool),
							    *session.external_manager);
	instance.background_manager.Add(*refresh);

	refresh->SendRequest(instance, session.id);
}
