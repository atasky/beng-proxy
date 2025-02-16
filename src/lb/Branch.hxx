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

#include "Goto.hxx"
#include "GotoConfig.hxx"

#include <list>

class LbGotoMap;
struct LbGotoIfConfig;
struct LbBranchConfig;

class LbGotoIf {
	const LbGotoIfConfig &config;

	const LbGoto destination;

public:
	LbGotoIf(LbGotoMap &goto_map, const LbGotoIfConfig &_config);

	const LbGotoIfConfig &GetConfig() const {
		return config;
	}

	template<typename R>
	[[gnu::pure]]
	bool MatchRequest(const R &request) const {
		return config.condition.MatchRequest(request);
	}

	const LbGoto &GetDestination() const {
		return destination;
	}
};

class LbBranch {
	const LbBranchConfig &config;

	LbGoto fallback;

	std::list<LbGotoIf> conditions;

public:
	LbBranch(LbGotoMap &goto_map, const LbBranchConfig &_config);

	const LbBranchConfig &GetConfig() const {
		return config;
	}

	template<typename R>
	[[gnu::pure]]
	const LbGoto &FindRequestLeaf(const R &request) const {
		for (const auto &i : conditions)
			if (i.MatchRequest(request))
				return i.GetDestination().FindRequestLeaf(request);

		return fallback.FindRequestLeaf(request);
	}
};
