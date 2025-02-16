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

#include "Config.hxx"
#include "io/ConfigParser.hxx"
#include "io/FileLineParser.hxx"
#include "util/HexParse.hxx"
#include "util/StringAPI.hxx"

#include <stdexcept>

bool
CertDatabaseConfig::ParseLine(const char *word, LineParser &line)
{
	if (StringIsEqual(word, "connect")) {
		connect = line.ExpectValueAndEnd();
		return true;
	} else if (StringIsEqual(word, "schema")) {
		schema = line.ExpectValueAndEnd();
		return true;
	} else if (StringIsEqual(word, "wrap_key")) {
		const char *name = line.ExpectValue();
		const char *hex_key = line.ExpectValue();
		line.ExpectEnd();

		CertDatabaseConfig::AES256 key;
		if (!ParseLowerHexFixed(hex_key, key))
			throw LineParser::Error("Malformed AES256 key");

		auto i = wrap_keys.emplace(name, key);
		if (!i.second)
			throw LineParser::Error("Duplicate wrap_key name");

		if (default_wrap_key.empty())
			default_wrap_key = i.first->first;

		return true;
	} else
		return false;
}

void
CertDatabaseConfig::Check()
{
	if (connect.empty())
		throw std::runtime_error("Missing 'connect'");
}

CertDatabaseConfig
LoadStandaloneCertDatabaseConfig(const char *path)
{
	struct StandaloneCertDatabaseConfigParser final : ConfigParser {
		CertDatabaseConfig config;

		/* virtual methods from class ConfigParser */
		void ParseLine(FileLineParser &line) override {
			const char *word = line.ExpectWord();
			if (!config.ParseLine(word, line))
				throw std::runtime_error{"Unknown option"};
		}

		void Finish() override {
			config.Check();
		}
	} parser;

	VariableConfigParser v_parser(parser);
	CommentConfigParser parser2(v_parser);
	IncludeConfigParser parser3(path, parser2);

	ParseConfigFile(path, parser3);

	return std::move(parser.config);
}
