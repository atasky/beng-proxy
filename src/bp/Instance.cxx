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

#include "Instance.hxx"
#include "Listener.hxx"
#include "Connection.hxx"
#include "memory/fb_pool.hxx"
#include "control/Server.hxx"
#include "control/Local.hxx"
#include "cluster/TcpBalancer.hxx"
#include "pipe_stock.hxx"
#include "DirectResourceLoader.hxx"
#include "CachedResourceLoader.hxx"
#include "FilterResourceLoader.hxx"
#include "BufferedResourceLoader.hxx"
#include "http/cache/Public.hxx"
#include "fcache.hxx"
#include "translation/Stock.hxx"
#include "translation/Cache.hxx"
#include "translation/Multi.hxx"
#include "translation/Builder.hxx"
#include "widget/Registry.hxx"
#include "http/local/Stock.hxx"
#include "fcgi/Stock.hxx"
#include "was/Stock.hxx"
#include "was/MStock.hxx"
#include "was/RStock.hxx"
#include "delegate/Stock.hxx"
#include "tcp_stock.hxx"
#include "ssl/Client.hxx"
#include "fs/Stock.hxx"
#include "fs/Balancer.hxx"
#include "nghttp2/Stock.hxx"
#include "stock/MapStock.hxx"
#include "session/Manager.hxx"
#include "session/Save.hxx"
#include "nfs/Stock.hxx"
#include "nfs/Cache.hxx"
#include "spawn/Client.hxx"
#include "access_log/Glue.hxx"
#include "util/PrintException.hxx"

#ifdef HAVE_URING
#include "event/uring/Manager.hxx"
#endif

#ifdef HAVE_AVAHI
#include "lib/avahi/Client.hxx"
#include "lib/avahi/Publisher.hxx"
#endif

#include <sys/signal.h>

static constexpr auto COMPRESS_INTERVAL = std::chrono::minutes(10);

BpInstance::BpInstance(BpConfig &&_config) noexcept
	:config(std::move(_config)),
	 shutdown_listener(event_loop, BIND_THIS_METHOD(ShutdownCallback)),
	 sighup_event(event_loop, SIGHUP, BIND_THIS_METHOD(ReloadEventCallback)),
	 compress_timer(event_loop, BIND_THIS_METHOD(OnCompressTimer)),
	 session_save_timer(event_loop, BIND_THIS_METHOD(SaveSessions))
{
	ForkCow(false);
	ScheduleCompress();
}

BpInstance::~BpInstance() noexcept
{
	delete (BufferedResourceLoader *)buffered_filter_resource_loader;

	if (filter_resource_loader != direct_resource_loader)
		delete (FilterResourceLoader *)filter_resource_loader;

	delete (DirectResourceLoader *)direct_resource_loader;

	FreeStocksAndCaches();
}

void
BpInstance::FreeStocksAndCaches() noexcept
{
	delete std::exchange(widget_registry, nullptr);
	translation_service.reset();
	cached_translation_service.reset();
	translation_caches.reset();
	uncached_translation_service.reset();
	translation_stocks.reset();

	if (http_cache != nullptr) {
		delete (CachedResourceLoader *)cached_resource_loader;
		cached_resource_loader = nullptr;

		http_cache_close(http_cache);
		http_cache = nullptr;
	}

	if (filter_cache != nullptr) {
		filter_cache_close(filter_cache);
		filter_cache = nullptr;
	}

	if (lhttp_stock != nullptr) {
		lhttp_stock_free(lhttp_stock);
		lhttp_stock = nullptr;
	}

	if (fcgi_stock != nullptr) {
		fcgi_stock_free(fcgi_stock);
		fcgi_stock = nullptr;
	}

#ifdef HAVE_LIBWAS
	delete std::exchange(was_stock, nullptr);
	delete std::exchange(multi_was_stock, nullptr);
	delete std::exchange(remote_was_stock, nullptr);
#endif

	delete std::exchange(fs_balancer, nullptr);
	delete std::exchange(fs_stock, nullptr);
#ifdef HAVE_NGHTTP2
	delete std::exchange(nghttp2_stock, nullptr);
#endif
	ssl_client_factory.reset();

	delete std::exchange(tcp_balancer, nullptr);

	delete tcp_stock;
	tcp_stock = nullptr;

	if (delegate_stock != nullptr) {
		delegate_stock_free(delegate_stock);
		delegate_stock = nullptr;
	}

#ifdef HAVE_LIBNFS
	if (nfs_cache != nullptr) {
		nfs_cache_free(nfs_cache);
		nfs_cache = nullptr;
	}

	if (nfs_stock != nullptr) {
		nfs_stock_free(nfs_stock);
		nfs_stock = nullptr;
	}
#endif

	delete std::exchange(pipe_stock, nullptr);
}

void
BpInstance::ForkCow(bool inherit) noexcept
{
	fb_pool_fork_cow(inherit);

	if (translation_caches)
		translation_caches->ForkCow(inherit);

	if (http_cache != nullptr)
		http_cache_fork_cow(*http_cache, inherit);

	if (filter_cache != nullptr)
		filter_cache_fork_cow(*filter_cache, inherit);

#ifdef HAVE_LIBNFS
	if (nfs_cache != nullptr)
		nfs_cache_fork_cow(*nfs_cache, inherit);
#endif
}

void
BpInstance::Compress() noexcept
{
	fb_pool_compress();
}

void
BpInstance::ScheduleCompress() noexcept
{
	compress_timer.Schedule(COMPRESS_INTERVAL);
}

void
BpInstance::OnCompressTimer() noexcept
{
	Compress();
	ScheduleCompress();
}

void
BpInstance::FadeChildren() noexcept
{
	if (lhttp_stock != nullptr)
		lhttp_stock_fade_all(*lhttp_stock);

	if (fcgi_stock != nullptr)
		fcgi_stock_fade_all(*fcgi_stock);

#ifdef HAVE_LIBWAS
	if (was_stock != nullptr)
		was_stock->FadeAll();
	if (multi_was_stock != nullptr)
		multi_was_stock->FadeAll();
#endif

	if (delegate_stock != nullptr)
		delegate_stock->FadeAll();
}

void
BpInstance::FadeTaggedChildren(std::string_view tag) noexcept
{
	if (lhttp_stock != nullptr)
		lhttp_stock_fade_tag(*lhttp_stock, tag);

	if (fcgi_stock != nullptr)
		fcgi_stock_fade_tag(*fcgi_stock, tag);

#ifdef HAVE_LIBWAS
	if (was_stock != nullptr)
		was_stock->FadeTag(tag);
	if (multi_was_stock != nullptr)
		multi_was_stock->FadeTag(tag);
#endif

	// TODO: delegate_stock
}

void
BpInstance::FlushTranslationCaches() noexcept
{
	if (widget_registry != nullptr)
		widget_registry->FlushCache();

	if (translation_caches)
		translation_caches->Flush();
}

void
BpInstance::OnMemoryWarning(uint64_t memory_usage,
			    uint64_t memory_max) noexcept
{
	fprintf(stderr, "Spawner memory warning: %" PRIu64 " of %" PRIu64 " bytes used\n",
		memory_usage, memory_max);

	if (lhttp_stock != nullptr)
		lhttp_stock_discard_some(*lhttp_stock);

#ifdef HAVE_LIBWAS
	if (multi_was_stock != nullptr)
		multi_was_stock->DiscardSome();
#endif

	// TODO: stop unused child processes
}

bool
BpInstance::OnAvahiError(std::exception_ptr e) noexcept
{
	PrintException(e);
	return true;
}

void
BpInstance::SaveSessions() noexcept
{
	session_save(*session_manager);

	ScheduleSaveSessions();
}

void
BpInstance::ScheduleSaveSessions() noexcept
{
	/* save all sessions every 2 minutes */
	session_save_timer.Schedule(std::chrono::minutes(2));
}
