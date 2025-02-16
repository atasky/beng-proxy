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

#include "util/IntrusiveList.hxx"

#include <cstdint>

/**
 * A job that shall be executed in a worker thread.
 */
class ThreadJob : public IntrusiveListHook {
public:
	enum class State : uint8_t {
		/**
		 * The job is not in any queue.
		 */
		INITIAL,

		/**
		 * The job has been added to the queue, but is not being worked on
		 * yet.
		 */
		WAITING,

		/**
		 * The job is being performed via Run().
		 */
		BUSY,

		/**
		 * The job has finished, but the Done() method has not been
		 * invoked yet.
		 */
		DONE,
	};

	State state = State::INITIAL;

	/**
	 * Shall this job be enqueued again instead of invoking its Done()
	 * method?
	 */
	bool again = false;

	/**
	 * Is this job currently idle, i.e. not being worked on by a
	 * worker thread?  This method may be called only from the main
	 * thread.  A "true" return value guarantees that no worker thread
	 * is and will be working on it, and its internal data structures
	 * may be accessed without mutex protection.  Use this method with
	 * caution.
	 */
	bool IsIdle() const noexcept {
		return state == State::INITIAL;
	}

	/**
	 * Invoked in a worker thread.
	 */
	virtual void Run() noexcept = 0;

	/**
	 * Invoked in the main thread after Run() has finished.
	 */
	virtual void Done() noexcept = 0;
};
