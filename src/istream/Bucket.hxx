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

#include "util/StaticVector.hxx"

#include <span>

class IstreamBucket {
public:
	enum class Type {
		BUFFER,
	};

private:
	Type type;

	union {
		std::span<const std::byte> buffer;
	};

public:
	explicit IstreamBucket(std::span<const std::byte> _buffer) noexcept
		:type(Type::BUFFER),
		 buffer(_buffer) {}


	Type GetType() const noexcept {
		return type;
	}

	bool IsBuffer() const noexcept {
		return type == Type::BUFFER;
	}

	std::span<const std::byte> GetBuffer() const noexcept {
		assert(type == Type::BUFFER);

		return buffer;
	}
};

class IstreamBucketList {
	using List = StaticVector<IstreamBucket, 64>;
	List list;

	bool more = false;

public:
	IstreamBucketList() = default;

	IstreamBucketList(const IstreamBucketList &) = delete;
	IstreamBucketList &operator=(const IstreamBucketList &) = delete;

	void SetMore(bool _more=true) noexcept {
		more = _more;
	}

	bool HasMore() const noexcept {
		return more;
	}

	bool IsEmpty() const noexcept {
		return list.empty();
	}

	bool IsFull() const noexcept {
		return list.full();
	}

	void Clear() noexcept {
		list.clear();
	}

	void Push(const IstreamBucket &bucket) noexcept {
		if (IsFull()) {
			SetMore();
			return;
		}

		list.push_back(bucket);
	}

	void Push(std::span<const std::byte> buffer) noexcept {
		if (IsFull()) {
			SetMore();
			return;
		}

		list.emplace_back(buffer);
	}

	List::const_iterator begin() const noexcept {
		return list.begin();
	}

	List::const_iterator end() const noexcept {
		return list.end();
	}

	[[gnu::pure]]
	bool HasNonBuffer() const noexcept {
		for (const auto &bucket : list)
			if (!bucket.IsBuffer())
				return true;
		return false;
	}

	[[gnu::pure]]
	size_t GetTotalBufferSize() const noexcept {
		size_t size = 0;
		for (const auto &bucket : list)
			if (bucket.IsBuffer())
				size += bucket.GetBuffer().size();
		return size;
	}

	[[gnu::pure]]
	bool IsDepleted(size_t consumed) const noexcept {
		return !HasMore() && consumed == GetTotalBufferSize();
	}

	void SpliceFrom(IstreamBucketList &&src) noexcept {
		if (src.HasMore())
			SetMore();

		for (const auto &bucket : src)
			Push(bucket);
	}

	/**
	 * Move buffer buckets from the given list, stopping at the first
	 * no-buffer bucket or after #max_size bytes have been moved.
	 *
	 * @return the number of bytes in all moved buffers
	 */
	size_t SpliceBuffersFrom(IstreamBucketList &&src,
				 size_t max_size) noexcept {
		if (src.HasMore())
			SetMore();

		size_t total_size = 0;
		for (const auto &bucket : src) {
			if (max_size == 0 ||
			    !bucket.IsBuffer()) {
				SetMore();
				break;
			}

			auto buffer = bucket.GetBuffer();
			if (buffer.size() > max_size) {
				buffer = buffer.first(max_size);
				SetMore();
			}

			Push(buffer);
			max_size -= buffer.size();
			total_size += buffer.size();
		}

		return total_size;
	}

	/**
	 * Move buffer buckets from the given list, stopping at the first
	 * no-buffer bucket.
	 *
	 * @return the number of bytes in all moved buffers
	 */
	size_t SpliceBuffersFrom(IstreamBucketList &&src) noexcept {
		if (src.HasMore())
			SetMore();

		size_t total_size = 0;
		for (const auto &bucket : src) {
			if (!bucket.IsBuffer()) {
				SetMore();
				break;
			}

			auto buffer = bucket.GetBuffer();
			Push(buffer);
			total_size += buffer.size();
		}

		return total_size;
	}

	/**
	 * Copy buffer buckets from the given list, stopping at the first
	 * no-buffer bucket.
	 *
	 * @param skip skip this number of bytes at the beginning
	 * @return the number of bytes in all moved buffers
	 */
	size_t CopyBuffersFrom(size_t skip,
			       const IstreamBucketList &src) noexcept {
		if (src.HasMore())
			SetMore();

		size_t total_size = 0;
		for (const auto &bucket : src) {
			if (!bucket.IsBuffer()) {
				SetMore();
				break;
			}

			auto buffer = bucket.GetBuffer();
			if (buffer.size() > skip) {
				buffer = buffer.subspan(skip);
				skip = 0;
				Push(buffer);
				total_size += buffer.size();
			} else
				skip -= buffer.size();
		}

		return total_size;
	}
};
