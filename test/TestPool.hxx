/*
 * Copyright 2007-2017 Content Management AG
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

#ifndef TEST_POOL_HXX
#define TEST_POOL_HXX

#include "pool.hxx"

#include <assert.h>

class TestPool {
    struct pool *const root_pool, *the_pool;

public:
    TestPool()
        :root_pool(pool_new_libc(nullptr, "root")),
         the_pool(pool_new_libc(root_pool, "test")) {}

    ~TestPool() {
        pool_unref(root_pool);
        if (the_pool != nullptr)
            pool_unref(the_pool);
        pool_commit();
        pool_recycler_clear();
    }

    TestPool(const TestPool &) = delete;
    TestPool &operator=(const TestPool &) = delete;

    operator struct pool &() {
        assert(the_pool != nullptr);

        return *the_pool;
    }

    operator struct pool *() {
        assert(the_pool != nullptr);

        return the_pool;
    }

    struct pool &Steal() {
        assert(the_pool != nullptr);

        return *std::exchange(the_pool, nullptr);
    }
};

#endif
