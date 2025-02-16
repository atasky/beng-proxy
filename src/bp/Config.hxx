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

#pragma once

#include "access_log/Config.hxx"
#include "ssl/Config.hxx"
#include "http/CookieSameSite.hxx"
#include "net/SocketConfig.hxx"
#include "spawn/Config.hxx"

#include <forward_list>
#include <chrono>
#include <string_view>

#include <stddef.h>

/**
 * Configuration.
 */
struct BpConfig {
	struct Listener : SocketConfig {
		std::string tag;

#ifdef HAVE_AVAHI
		std::string zeroconf_service;
		std::string zeroconf_interface;
#endif

		/**
		 * If non-empty, then this listener has its own
		 * translation server(s) and doesn't use the global
		 * server.
		 */
		std::forward_list<AllocatedSocketAddress> translation_sockets;

		enum class Handler {
			TRANSLATION,
			PROMETHEUS_EXPORTER,
		} handler = Handler::TRANSLATION;

		bool auth_alt_host = false;

		bool ssl = false;

		SslConfig ssl_config;

		Listener() {
			listen = 64;
			tcp_defer_accept = 10;
		}

		explicit Listener(SocketAddress _address) noexcept
			:SocketConfig(_address)
		{
			listen = 64;
			tcp_defer_accept = 10;
		}

#ifdef HAVE_AVAHI
		/**
		 * @return the name of the interface where the
		 * Zeroconf service shall be published
		 */
		[[gnu::pure]]
		const char *GetZeroconfInterface() const noexcept {
			if (!zeroconf_interface.empty())
				return zeroconf_interface.c_str();

			if (!interface.empty())
				return interface.c_str();

			return nullptr;
		}
#endif
	};

	std::forward_list<Listener> listen;

	AccessLogConfig access_log;

	AccessLogConfig child_error_log;

	std::string session_cookie = "beng_proxy_session";

	std::chrono::seconds session_idle_timeout = std::chrono::minutes(30);

	std::string session_save_path;

	struct ControlListener : SocketConfig {
		ControlListener() {
			pass_cred = true;
		}

		explicit ControlListener(SocketAddress _bind_address)
			:SocketConfig(_bind_address) {
			pass_cred = true;
		}
	};

	std::forward_list<ControlListener> control_listen;

	std::forward_list<AllocatedSocketAddress> translation_sockets;

	/** maximum number of simultaneous connections */
	unsigned max_connections = 32768;

	size_t http_cache_size = 512 * 1024 * 1024;

	size_t filter_cache_size = 128 * 1024 * 1024;

	size_t nfs_cache_size = 256 * 1024 * 1024;

	unsigned translate_cache_size = 131072;
	unsigned translate_stock_limit = 32;

	unsigned tcp_stock_limit = 0;

	unsigned lhttp_stock_limit = 0, lhttp_stock_max_idle = 8;
	unsigned fcgi_stock_limit = 0, fcgi_stock_max_idle = 8;

	unsigned was_stock_limit = 0, was_stock_max_idle = 16;
	unsigned multi_was_stock_limit = 0, multi_was_stock_max_idle = 16;
	unsigned remote_was_stock_limit = 0, remote_was_stock_max_idle = 16;

	unsigned cluster_size = 0, cluster_node = 0;

	CookieSameSite session_cookie_same_site = CookieSameSite::DEFAULT;

	bool dynamic_session_cookie = false;

	bool verbose_response = false;

	bool emulate_mod_auth_easy = false;

	bool http_cache_obey_no_cache = true;

	SpawnConfig spawn;

	SslClientConfig ssl_client;

	BpConfig() {
#ifdef HAVE_LIBSYSTEMD
		spawn.systemd_scope = "bp-spawn.scope";
		spawn.systemd_scope_description = "The cm4all-beng-proxy child process spawner";
		spawn.systemd_slice = "system-cm4all.slice";
#endif
	}

	void HandleSet(std::string_view name, const char *value);

	void Finish(unsigned default_port);
};

/**
 * Load and parse the specified configuration file.  Throws an
 * exception on error.
 */
void
LoadConfigFile(BpConfig &config, const char *path);
