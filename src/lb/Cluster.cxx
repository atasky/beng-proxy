/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk.com>
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

#include "Cluster.hxx"
#include "ClusterConfig.hxx"
#include "avahi/Explorer.hxx"
#include "StickyCache.hxx"
#include "failure.hxx"
#include "net/ToString.hxx"
#include "util/HashRing.hxx"
#include "util/ConstBuffer.hxx"

#include <sodium/crypto_generichash.h>

class LbCluster::StickyRing final : public HashRing<const MemberMap::value_type *,
                                                    sticky_hash_t,
                                                    4096, 8> {};

const char *
LbCluster::Member::GetLogName(const char *key) const noexcept
{
    if (log_name.empty()) {
        if (address.IsNull())
            return key;

        log_name = key;

        char buffer[512];
        if (ToString(buffer, sizeof(buffer), address)) {
            log_name += " (";
            log_name += buffer;
            log_name += ")";
        }
    }

    return log_name.c_str();
}

LbCluster::LbCluster(const LbClusterConfig &_config,
                     MyAvahiClient &avahi_client)
    :config(_config),
     logger("cluster " + config.name)
{
    if (config.HasZeroConf())
        explorer.reset(new AvahiServiceExplorer(avahi_client, *this,
                                                AVAHI_IF_UNSPEC,
                                                AVAHI_PROTO_UNSPEC,
                                                config.zeroconf_service.c_str(),
                                                config.zeroconf_domain.empty()
                                                ? nullptr
                                                : config.zeroconf_domain.c_str()));
}

LbCluster::~LbCluster()
{
    delete sticky_cache;
    delete sticky_ring;
}

const LbCluster::MemberMap::value_type &
LbCluster::PickNextZeroconf()
{
    assert(!active_members.empty());

    ++last_pick;
    if (last_pick >= active_members.size())
        last_pick = 0;

    return *active_members[last_pick];
}

const LbCluster::MemberMap::value_type &
LbCluster::PickNextGoodZeroconf()
{
    assert(!active_members.empty());

    unsigned remaining = active_members.size();

    while (true) {
        const auto &m = PickNextZeroconf();
        if (--remaining == 0 ||
            failure_get_status(m.second.GetAddress()) == FAILURE_OK)
            return m;
    }
}

std::pair<const char *, SocketAddress>
LbCluster::Pick(sticky_hash_t sticky_hash)
{
    if (dirty) {
        dirty = false;
        FillActive();
    }

    if (active_members.empty())
        return std::make_pair(nullptr, nullptr);

    if (sticky_hash != 0 && config.sticky_cache) {
        /* look up the sticky_hash in the StickyCache */

        assert(config.sticky_mode != StickyMode::NONE);

        if (sticky_cache == nullptr)
            /* lazy cache allocation */
            sticky_cache = new StickyCache();

        const auto *cached = sticky_cache->Get(sticky_hash);
        if (cached != nullptr) {
            /* cache hit */
            auto i = members.find(*cached);
            if (i != members.end() &&
                // TODO: allow FAILURE_FADE here?
                failure_get_status(i->second.GetAddress()) == FAILURE_OK)
                /* the node is active, we can use it */
                return std::make_pair(i->second.GetLogName(i->first.c_str()),
                                      i->second.GetAddress());

            sticky_cache->Remove(sticky_hash);
        }

        /* cache miss or cached node not active: fall back to
           round-robin and remember the new pick in the cache */
    } else if (sticky_hash != 0) {
        /* use consistent hashing */

        assert(sticky_ring != nullptr);

        auto *i = sticky_ring->Pick(sticky_hash);
        assert(i != nullptr);

        unsigned retries = active_members.size();
        while (true) {
            if (--retries == 0 ||
                failure_get_status(i->second.GetAddress()) == FAILURE_OK)
                return std::make_pair(i->second.GetLogName(i->first.c_str()),
                                      i->second.GetAddress());

            /* the node is known-bad; pick the next one in the ring */
            const auto next = sticky_ring->FindNext(sticky_hash);
            sticky_hash = next.first;
            i = next.second;
        }
    }

    const auto &i = PickNextGoodZeroconf();

    if (sticky_hash != 0)
        sticky_cache->Put(sticky_hash, i.first);

    return std::make_pair(i.second.GetLogName(i.first.c_str()),
                          i.second.GetAddress());
}

static void
UpdateHash(crypto_generichash_state &state, ConstBuffer<void> p)
{
    crypto_generichash_update(&state, (const unsigned char *)p.data, p.size);
}

static void
UpdateHash(crypto_generichash_state &state, SocketAddress address)
{
    assert(!address.IsNull());

    UpdateHash(state, address.GetSteadyPart());
}

template<typename T>
static void
UpdateHashT(crypto_generichash_state &state, const T &data)
{
    crypto_generichash_update(&state,
                              (const unsigned char *)&data, sizeof(data));
}

void
LbCluster::FillActive()
{
    active_members.clear();
    active_members.reserve(members.size());

    for (const auto &i : members)
        active_members.push_back(&i);

    if (!config.sticky_cache) {
        if (sticky_ring == nullptr)
            /* lazy allocation */
            sticky_ring = new StickyRing();

        /**
         * Functor class which generates a #HashRing hash for a
         * cluster member combined with a replica number.
         */
        struct MemberHasher {
            gcc_pure
            sticky_hash_t operator()(const MemberMap::value_type *member,
                                     size_t replica) const {
                /* use libsodium's "generichash" (BLAKE2b) which is
                   secure enough for class HashRing */
                union {
                    unsigned char hash[crypto_generichash_BYTES];
                    sticky_hash_t result;
                } u;

                crypto_generichash_state state;
                crypto_generichash_init(&state, nullptr, 0, sizeof(u.hash));
                UpdateHash(state, member->second.GetAddress());
                UpdateHashT(state, replica);
                crypto_generichash_final(&state, u.hash, sizeof(u.hash));

                return u.result;
            }
        };

        sticky_ring->Build(active_members, MemberHasher());
    }
}

void
LbCluster::OnAvahiNewObject(const std::string &key, SocketAddress address)
{
    auto i = members.emplace(std::piecewise_construct,
                             std::forward_as_tuple(key),
                             std::forward_as_tuple(address));
    if (!i.second)
        /* update existing member */
        i.first->second.SetAddress(address);

    dirty = true;
}

void
LbCluster::OnAvahiRemoveObject(const std::string &key)
{
    auto i = members.find(key);
    if (i == members.end())
        return;

    /* purge this entry from the "failure" map, because it
       will never be used again anyway */
    failure_unset(i->second.GetAddress(), FAILURE_OK);

    members.erase(i);
    dirty = true;
}
