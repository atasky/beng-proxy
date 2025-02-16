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

#ifndef BENG_PROXY_SSL_CONFIG_H
#define BENG_PROXY_SSL_CONFIG_H

#include <string>
#include <vector>

enum class SslVerify {
	NO,
	YES,
	OPTIONAL,
};

struct SslCertKeyConfig {
	std::string cert_file;

	std::string key_file;

	template<typename C, typename K>
	SslCertKeyConfig(C &&_cert_file, K &&_key_file)
		:cert_file(std::forward<C>(_cert_file)),
		 key_file(std::forward<K>(_key_file)) {}
};

/**
 * SSL/TLS configuration.
 */
struct SslConfig {
	std::vector<SslCertKeyConfig> cert_key;

	std::string ca_cert_file;

	SslVerify verify = SslVerify::NO;
};

struct NamedSslCertKeyConfig : SslCertKeyConfig {
	std::string name;

	template<typename N, typename C, typename K>
	NamedSslCertKeyConfig(N &&_name, C &&_cert_file, K &&_key_file) noexcept
		:SslCertKeyConfig(std::forward<C>(_cert_file),
				  std::forward<K>(_key_file)),
		 name(std::forward<N>(_name)) {}
};

struct SslClientConfig {
	std::vector<NamedSslCertKeyConfig> cert_key;
};

#endif
