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

#include "Service.hxx"

#include <list>
#include <memory>

/**
 * Wrapper for multiple #TranslationService.  This class implements
 * #TranslationCommand::DEFER.
 */
class MultiTranslationService final : public TranslationService {
	using List = std::list<std::shared_ptr<TranslationService>>;

	List items;

	class Request;

public:
	MultiTranslationService() = default;

	template<typename T>
	explicit MultiTranslationService(T &&t) noexcept {
		for (auto &i : t)
			Add(i);
	}

	template<typename T>
	void Add(T &&service) noexcept {
		items.emplace_back(std::forward<T>(service));
	}

	/* virtual methods from class TranslationService */
	void SendRequest(AllocatorPtr alloc,
			 const TranslateRequest &request,
			 const StopwatchPtr &parent_stopwatch,
			 TranslateHandler &handler,
			 CancellablePointer &cancel_ptr) noexcept override;
};
