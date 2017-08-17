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

#include "lb_stats.hxx"
#include "lb/Instance.hxx"
#include "stock/MapStock.hxx"
#include "stock/Stats.hxx"
#include "fb_pool.hxx"
#include "SlicePool.hxx"
#include "AllocatorStats.hxx"
#include "beng-proxy/control.h"
#include "util/ByteOrder.hxx"

void
lb_get_stats(const LbInstance *instance,
             struct beng_control_stats *data)
{
    StockStats tcp_stock_stats = {
        .busy = 0,
        .idle = 0,
    };

    instance->tcp_stock->AddStats(tcp_stock_stats);

    data->incoming_connections = ToBE32(instance->http_connections.size()
                                        + instance->tcp_connections.size());
    data->outgoing_connections = ToBE32(tcp_stock_stats.busy
                                        + tcp_stock_stats.idle
                                        + instance->tcp_connections.size());
    data->children = 0;
    data->sessions = 0;
    data->http_requests = ToBE64(instance->http_request_counter);
    data->translation_cache_size = 0;
    data->http_cache_size = 0;
    data->filter_cache_size = 0;
    data->translation_cache_brutto_size = 0;
    data->http_cache_brutto_size = 0;
    data->filter_cache_brutto_size = 0;
    data->nfs_cache_size = data->nfs_cache_brutto_size = 0;

    const auto io_buffers_stats = slice_pool_get_stats(fb_pool_get());
    data->io_buffers_size = ToBE64(io_buffers_stats.netto_size);
    data->io_buffers_brutto_size = ToBE64(io_buffers_stats.brutto_size);
}
