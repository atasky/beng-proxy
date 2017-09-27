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

#include "FailureManager.hxx"
#include "net/SocketAddress.hxx"
#include "util/djbhash.h"
#include "util/DeleteDisposer.hxx"

#include <assert.h>

inline size_t
FailureManager::Failure::Hash::operator()(const SocketAddress a) const noexcept
{
    assert(!a.IsNull());

    return djb_hash(a.GetAddress(), a.GetSize());
}

FailureManager::~FailureManager() noexcept
{
    failures.clear_and_dispose(DeleteDisposer());
}

bool
FailureManager::Failure::OverrideStatus(Expiry now,
                                        enum failure_status new_status,
                                        std::chrono::seconds duration) noexcept
{
    if (IsExpired()) {
        /* expired: override in any case */
    } else if (new_status == status) {
        /* same status: update expiry */
    } else if (new_status == FAILURE_FADE) {
        /* store "fade" expiry in special attribute, until the other
           failure status expires */
        fade_expires.Touch(now, duration);
        return true;
    } else if (status == FAILURE_FADE) {
        /* copy the "fade" expiry to the special attribute, and
           overwrite the FAILURE_FADE status */
        fade_expires = expires;
    } else if (new_status < status)
        return false;

    expires.Touch(now, duration);
    status = new_status;
    return true;
}

void
FailureManager::Set(SocketAddress address, enum failure_status status,
                    std::chrono::seconds duration) noexcept
{
    assert(!address.IsNull());
    assert(status > FAILURE_OK);

    const Expiry now = Expiry::Now();

    FailureSet::insert_commit_data hint;
    auto result = failures.insert_check(address, Failure::Hash(),
                                        Failure::Equal(), hint);
    if (result.second) {
        Failure *failure = new Failure(address, status,
                                       Expiry::Touched(now, duration));
        failures.insert_commit(*failure, hint);
    } else {
        Failure &failure = *result.first;
        failure.OverrideStatus(now, status, duration);
    }
}

void
FailureManager::Add(SocketAddress address) noexcept
{
    Set(address, FAILURE_FAILED, std::chrono::seconds(20));
}

static constexpr bool
match_status(enum failure_status current, enum failure_status match) noexcept
{
    /* FAILURE_OK is a catch-all magic value */
    return match == FAILURE_OK || current == match;
}

inline void
FailureManager::Unset(Failure &failure, enum failure_status status) noexcept
{
    if (status == FAILURE_FADE)
        failure.fade_expires = Expiry::AlreadyExpired();

    if (!match_status(failure.status, status) && !failure.IsExpired())
        /* don't update if the current status is more serious than the
           one to be removed */
        return;

    if (status != FAILURE_OK && failure.IsFade()) {
        failure.status = FAILURE_FADE;
        failure.expires = failure.fade_expires;
        failure.fade_expires = Expiry::AlreadyExpired();
    } else {
        failures.erase_and_dispose(failures.iterator_to(failure),
                                   DeleteDisposer());
    }
}

void
FailureManager::Unset(SocketAddress address,
                      enum failure_status status) noexcept
{
    assert(!address.IsNull());

    auto i = failures.find(address, Failure::Hash(), Failure::Equal());
    if (i != failures.end())
        Unset(*i, status);
}

enum failure_status
FailureManager::Get(SocketAddress address) noexcept
{
    assert(!address.IsNull());

    auto i = failures.find(address, Failure::Hash(), Failure::Equal());
    if (i == failures.end())
        return FAILURE_OK;

    return i->GetStatus();
}
