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

#include "util/IntrusiveForwardList.hxx"

/**
 * List of key/value pairs.
 */
class KeyValueList {
public:
	struct Item : IntrusiveForwardListHook {
		const char *key, *value;

		Item(const char *_key, const char *_value) noexcept
			:key(_key), value(_value) {}
	};

private:
	using List = IntrusiveForwardList<Item>;

	typedef List::const_iterator const_iterator;

	List list;

public:
	KeyValueList() = default;
	KeyValueList(const KeyValueList &) = delete;
	KeyValueList(KeyValueList &&src) noexcept
		:list(std::move(src.list)) {}

	template<typename Alloc>
	KeyValueList(Alloc &&alloc, const KeyValueList &src) noexcept {
		for (const auto &i : src)
			Add(alloc, alloc.Dup(i.key), alloc.Dup(i.value));
	}

	KeyValueList &operator=(KeyValueList &&src) noexcept {
		list = std::move(src.list);
		return *this;
	}

	const_iterator begin() const noexcept {
		return list.begin();
	}

	const_iterator end() const noexcept {
		return list.end();
	}

	bool IsEmpty() const noexcept {
		return list.empty();
	}

	void Clear() noexcept {
		list.clear();
	}

	template<typename Alloc>
	void Add(Alloc &&alloc, const char *key, const char *value) noexcept {
		auto item = alloc.template New<Item>(key, value);
		list.push_front(*item);
	}

	void Reverse() noexcept {
		list.reverse();
	}
};
