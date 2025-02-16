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

#include "Cluster.hxx"
#include "ClusterConfig.hxx"
#include "MemberHash.hxx"
#include "Context.hxx"
#include "MonitorStock.hxx"
#include "MonitorRef.hxx"
#include "fs/Stock.hxx"
#include "fs/Balancer.hxx"
#include "fs/Handler.hxx"
#include "cluster/StickyCache.hxx"
#include "cluster/ConnectBalancer.hxx"
#include "cluster/RoundRobinBalancer.cxx"
#include "stock/GetHandler.hxx"
#include "system/Error.hxx"
#include "event/Loop.hxx"
#include "net/PConnectSocket.hxx"
#include "net/FailureManager.hxx"
#include "net/ToString.hxx"
#include "util/ConstBuffer.hxx"
#include "util/DeleteDisposer.hxx"
#include "util/DereferenceIterator.hxx"
#include "AllocatorPtr.hxx"
#include "HttpMessageResponse.hxx"
#include "lease.hxx"
#include "stopwatch.hxx"

#ifdef HAVE_AVAHI
#include "lib/avahi/Client.hxx"
#include "lib/avahi/Explorer.hxx"

#include <net/if.h>
#endif

#ifdef HAVE_AVAHI

class LbCluster::StickyRing final
	: public MemberHashRing<ZeroconfMemberMap::pointer> {};

LbCluster::ZeroconfMember::ZeroconfMember(const std::string &_key,
					  SocketAddress _address,
					  ReferencedFailureInfo &_failure,
					  LbMonitorStock *monitors) noexcept
	:key(_key), address(_address), failure(_failure),
	 monitor(monitors != nullptr
		 ? std::make_unique<LbMonitorRef>(monitors->Add(key.c_str(),
								_address))
		 : std::unique_ptr<LbMonitorRef>())
{
}

LbCluster::ZeroconfMember::~ZeroconfMember() noexcept = default;

const char *
LbCluster::ZeroconfMember::GetLogName() const noexcept
{
	if (log_name.empty()) {
		if (address.IsNull())
			return key.c_str();

		log_name = key.c_str();

		char buffer[512];
		if (ToString(buffer, sizeof(buffer), address)) {
			log_name += " (";
			log_name += buffer;
			log_name += ")";
		}
	}

	return log_name.c_str();
}

#endif

LbCluster::LbCluster(const LbClusterConfig &_config,
		     const LbContext &context,
		     LbMonitorStock *_monitors)
	:config(_config), failure_manager(context.failure_manager),
	 tcp_balancer(context.tcp_balancer),
	 fs_stock(context.fs_stock),
	 fs_balancer(context.fs_balancer),
	 monitors(_monitors),
	 logger("cluster " + config.name)
{
#ifdef HAVE_AVAHI
	if (config.HasZeroConf()) {
		AvahiIfIndex interface = AVAHI_IF_UNSPEC;

		if (!config.zeroconf_interface.empty()) {
			int i = if_nametoindex(config.zeroconf_interface.c_str());
			if (i == 0)
				throw FormatErrno("Failed to find interface '%s'",
						  config.zeroconf_interface.c_str());

			interface = AvahiIfIndex(i);
		}

		if (!context.avahi_client)
			context.avahi_client = std::make_unique<Avahi::Client>(context.fs_stock.GetEventLoop(),
									       context.avahi_error_handler);

		explorer.reset(new Avahi::ServiceExplorer(*context.avahi_client,
							  *this,
							  interface,
							  AVAHI_PROTO_UNSPEC,
							  config.zeroconf_service.c_str(),
							  config.zeroconf_domain.empty()
							  ? nullptr
							  : config.zeroconf_domain.c_str(),
							  context.avahi_error_handler));
	}
#endif

	static_members.reserve(config.members.size());
	for (const auto &member : config.members) {
		AllocatedSocketAddress address(member.node->address);
		if (member.port > 0)
			address.SetPort(member.port);

		auto &failure = failure_manager.Make(address);

		static_members.emplace_back(std::move(address), failure);
	}

	if (monitors != nullptr)
		/* create monitors for "static" members */
		for (const auto &member : config.members)
			static_member_monitors.emplace_front(monitors->Add(*member.node,
									   member.port));
}

LbCluster::~LbCluster() noexcept
{
#ifdef HAVE_AVAHI
	zeroconf_members.clear_and_dispose(DeleteDisposer());
#endif
}

void
LbCluster::ConnectHttp(AllocatorPtr alloc,
		       const StopwatchPtr &parent_stopwatch,
		       uint_fast64_t fairness_hash,
		       SocketAddress bind_address,
		       sticky_hash_t sticky_hash,
		       Event::Duration timeout,
		       SocketFilterFactory *filter_factory,
		       FilteredSocketBalancerHandler &handler,
		       CancellablePointer &cancel_ptr) noexcept
{
#ifdef HAVE_AVAHI
	if (config.HasZeroConf()) {
		ConnectZeroconfHttp(alloc, parent_stopwatch,
				    fairness_hash,
				    bind_address, sticky_hash,
				    timeout, filter_factory,
				    handler, cancel_ptr);
		return;
	}
#endif

	ConnectStaticHttp(alloc, parent_stopwatch,
			  fairness_hash,
			  bind_address, sticky_hash,
			  timeout, filter_factory,
			  handler, cancel_ptr);
}

void
LbCluster::ConnectTcp(AllocatorPtr alloc,
		      SocketAddress bind_address,
		      sticky_hash_t sticky_hash,
		      Event::Duration timeout,
		      ConnectSocketHandler &handler,
		      CancellablePointer &cancel_ptr) noexcept
{
#ifdef HAVE_AVAHI
	if (config.HasZeroConf()) {
		ConnectZeroconfTcp(alloc, bind_address, sticky_hash,
				   timeout, handler, cancel_ptr);
		return;
	}
#endif

	ConnectStaticTcp(alloc, bind_address, sticky_hash,
			 timeout, handler, cancel_ptr);
}

inline void
LbCluster::ConnectStaticHttp(AllocatorPtr alloc,
			     const StopwatchPtr &parent_stopwatch,
			     uint_fast64_t fairness_hash,
			     SocketAddress bind_address,
			     sticky_hash_t sticky_hash,
			     Event::Duration timeout,
			     SocketFilterFactory *filter_factory,
			     FilteredSocketBalancerHandler &handler,
			     CancellablePointer &cancel_ptr) noexcept
{
	assert(config.protocol == LbProtocol::HTTP);

	fs_balancer.Get(alloc, parent_stopwatch,
			fairness_hash,
			config.transparent_source,
			bind_address,
			sticky_hash,
			config.address_list,
			timeout,
			filter_factory,
			handler, cancel_ptr);
}

inline void
LbCluster::ConnectStaticTcp(AllocatorPtr alloc,
			    SocketAddress bind_address,
			    sticky_hash_t sticky_hash,
			    Event::Duration timeout,
			    ConnectSocketHandler &handler,
			    CancellablePointer &cancel_ptr) noexcept
{
	assert(config.protocol == LbProtocol::TCP);

	client_balancer_connect(fs_balancer.GetEventLoop(), alloc,
				tcp_balancer,
				failure_manager,
				config.transparent_source,
				bind_address,
				sticky_hash,
				config.address_list,
				timeout,
				handler,
				cancel_ptr);
}

#ifdef HAVE_AVAHI

struct LbCluster::ZeroconfListWrapper {
	const ZeroconfMemberList &active_members;

	using const_reference = const ZeroconfMember &;
	using const_iterator = DereferenceIterator<ZeroconfMemberList::const_iterator>;

	auto size() const noexcept {
		return active_members.size();
	}

	const_iterator begin() const noexcept {
		return active_members.begin();
	}

	const_iterator end() const noexcept {
		return active_members.end();
	}

	[[gnu::pure]]
	bool Check(const Expiry now, const_reference member,
		   bool allow_fade) const noexcept {
		return member.GetFailureInfo().Check(now, allow_fade);
	}
};

LbCluster::ZeroconfMemberMap::const_reference
LbCluster::PickNextGoodZeroconf(const Expiry now) noexcept
{
	assert(!active_zeroconf_members.empty());

	if (active_zeroconf_members.size() < 2)
		/* since RoundRobinBalancer expects at least 2
		   members, this special case returns the one and only
		   member without consulting RoundRobinBalancer */
		return *active_zeroconf_members.front();

	return round_robin_balancer.Get(now,
					ZeroconfListWrapper{active_zeroconf_members},
					false);
}

inline const LbCluster::ZeroconfMember &
LbCluster::PickZeroconfHashRing(Expiry now,
				sticky_hash_t sticky_hash) noexcept
{
	assert(!active_zeroconf_members.empty());
	assert(sticky_ring != nullptr);

	auto *i = sticky_ring->Pick(sticky_hash);
	assert(i != nullptr);

	unsigned retries = active_zeroconf_members.size();
	while (true) {
		if (--retries == 0 ||
		    i->GetFailureInfo().Check(now))
			return *i;

		/* the node is known-bad; pick the next one in the ring */
		const auto next = sticky_ring->FindNext(sticky_hash);
		sticky_hash = next.first;
		i = next.second;
	}
}

const LbCluster::ZeroconfMember *
LbCluster::PickZeroconf(const Expiry now, sticky_hash_t sticky_hash) noexcept
{
	if (dirty) {
		dirty = false;
		FillActive();
	}

	if (active_zeroconf_members.empty())
		return nullptr;

	if (sticky_hash != 0) {
		assert(config.sticky_mode != StickyMode::NONE);

		if (!config.sticky_cache)
			/* use consistent hashing */
			return &PickZeroconfHashRing(now, sticky_hash);

		/* look up the sticky_hash in the StickyCache */
		if (sticky_cache == nullptr)
			/* lazy cache allocation */
			sticky_cache = std::make_unique<StickyCache>();

		const auto *cached = sticky_cache->Get(sticky_hash);
		if (cached != nullptr) {
			/* cache hit */
			auto i = zeroconf_members.find(*cached,
						       zeroconf_members.key_comp());
			if (i != zeroconf_members.end() &&
			    // TODO: allow FAILURE_FADE here?
			    i->GetFailureInfo().Check(now))
				/* the node is active, we can use it */
				return &*i;

			sticky_cache->Remove(sticky_hash);
		}

		/* cache miss or cached node not active: fall back to
		   round-robin and remember the new pick in the cache */
	}

	auto &i = PickNextGoodZeroconf(now);

	if (sticky_hash != 0)
		sticky_cache->Put(sticky_hash, i.GetKey());

	return &i;
}

void
LbCluster::FillActive() noexcept
{
	round_robin_balancer.Reset();

	active_zeroconf_members.clear();
	active_zeroconf_members.reserve(zeroconf_members.size());

	for (auto &i : zeroconf_members)
		active_zeroconf_members.push_back(&i);

	if (!config.sticky_cache) {
		if (sticky_ring == nullptr)
			/* lazy allocation */
			sticky_ring = std::make_unique<StickyRing>();

		BuildMemberHashRing(*sticky_ring, active_zeroconf_members,
				    [](ZeroconfMemberMap::const_pointer member) noexcept {
					    return member->GetAddress();
				    });
	}
}

class LbCluster::ZeroconfHttpConnect final : StockGetHandler, Lease, Cancellable {
	LbCluster &cluster;

	AllocatorPtr alloc;

	const uint_least64_t fairness_hash;

	const SocketAddress bind_address;
	const sticky_hash_t sticky_hash;
	const Event::Duration timeout;
	SocketFilterFactory *const filter_factory;

	FilteredSocketBalancerHandler &handler;

	FailurePtr failure;

	CancellablePointer cancel_ptr;

	StockItem *stock_item;

	/**
	 * The number of remaining connection attempts.  We give up when
	 * we get an error and this attribute is already zero.
	 */
	unsigned retries;

public:
	ZeroconfHttpConnect(LbCluster &_cluster, AllocatorPtr _alloc,
			    const uint_fast64_t _fairness_hash,
			    SocketAddress _bind_address,
			    sticky_hash_t _sticky_hash,
			    Event::Duration _timeout,
			    SocketFilterFactory *_filter_factory,
			    FilteredSocketBalancerHandler &_handler,
			    CancellablePointer &caller_cancel_ptr) noexcept
		:cluster(_cluster), alloc(_alloc),
		 fairness_hash(_fairness_hash),
		 bind_address(_bind_address),
		 sticky_hash(_sticky_hash),
		 timeout(_timeout),
		 filter_factory(_filter_factory),
		 handler(_handler),
		 retries(CalculateRetries(cluster.GetZeroconfCount()))
	{
		caller_cancel_ptr = *this;
	}

	void Destroy() noexcept {
		this->~ZeroconfHttpConnect();
	}

	auto &GetEventLoop() const noexcept {
		return cluster.fs_balancer.GetEventLoop();
	}

	void Start() noexcept;

private:
	/* code copied from generic_balancer.hxx */
	static constexpr unsigned CalculateRetries(size_t size) noexcept {
		if (size <= 1)
			return 0;
		else if (size == 2)
			return 1;
		else if (size == 3)
			return 2;
		else
			return 3;
	}

	/* virtual methods from class StockGetHandler */
	void OnStockItemReady(StockItem &item) noexcept override;
	void OnStockItemError(std::exception_ptr ep) noexcept override;

	/* virtual methods from class Lease */
	void ReleaseLease(bool reuse) noexcept final;

	/* virtual methods from class Cancellable */
	void Cancel() noexcept override {
		cancel_ptr.Cancel();
		Destroy();
	}
};

void
LbCluster::ZeroconfHttpConnect::Start() noexcept
{
	auto *member = cluster.PickZeroconf(GetEventLoop().SteadyNow(),
					    sticky_hash);
	if (member == nullptr) {
		auto &_handler = handler;
		Destroy();
		_handler.OnFilteredSocketError(std::make_exception_ptr(HttpMessageResponse(HTTP_STATUS_SERVICE_UNAVAILABLE,
											   "Zeroconf cluster is empty")));
		return;
	}

	failure = member->GetFailureRef();

	cluster.fs_stock.Get(alloc,
			     nullptr,
			     member->GetLogName(),
			     fairness_hash,
			     cluster.config.transparent_source,
			     bind_address,
			     member->GetAddress(),
			     timeout, filter_factory,
			     *this, cancel_ptr);
}

void
LbCluster::ZeroconfHttpConnect::OnStockItemReady(StockItem &item) noexcept
{
	failure->UnsetConnect();

	stock_item = &item;

	handler.OnFilteredSocketReady(*this, fs_stock_item_get(item),
				      fs_stock_item_get_address(item),
				      item.GetStockName(),
				      *failure);
}

void
LbCluster::ZeroconfHttpConnect::OnStockItemError(std::exception_ptr ep) noexcept
{
	failure->SetConnect(GetEventLoop().SteadyNow(),
			    std::chrono::seconds(20));

	if (retries-- > 0) {
		/* try the next Zeroconf member */
		Start();
		return;
	}

	auto &_handler = handler;
	Destroy();
	_handler.OnFilteredSocketError(std::move(ep));
}

void
LbCluster::ZeroconfHttpConnect::ReleaseLease(bool reuse) noexcept
{
	stock_item->Put(!reuse);
	Destroy();
}

inline void
LbCluster::ConnectZeroconfHttp(AllocatorPtr alloc,
			       const StopwatchPtr &,
			       uint_fast64_t fairness_hash,
			       SocketAddress bind_address,
			       sticky_hash_t sticky_hash,
			       Event::Duration timeout,
			       SocketFilterFactory *filter_factory,
			       FilteredSocketBalancerHandler &handler,
			       CancellablePointer &cancel_ptr) noexcept
{
	assert(config.HasZeroConf());

	auto *c = alloc.New<ZeroconfHttpConnect>(*this, alloc,
						 fairness_hash,
						 bind_address,
						 sticky_hash, timeout,
						 filter_factory,
						 handler, cancel_ptr);
	c->Start();
}

inline void
LbCluster::ConnectZeroconfTcp(AllocatorPtr alloc,
			      SocketAddress bind_address,
			      sticky_hash_t sticky_hash,
			      Event::Duration timeout,
			      ConnectSocketHandler &handler,
			      CancellablePointer &cancel_ptr) noexcept
{
	assert(config.HasZeroConf());
	assert(config.protocol == LbProtocol::TCP);

	auto &event_loop = fs_balancer.GetEventLoop();

	const auto *member = PickZeroconf(event_loop.SteadyNow(), sticky_hash);
	if (member == nullptr) {
		handler.OnSocketConnectError(std::make_exception_ptr(std::runtime_error("Zeroconf cluster is empty")));
		return;
	}

	const auto address = member->GetAddress();
	assert(address.IsDefined());

	client_socket_new(event_loop, alloc, nullptr,
			  address.GetFamily(), SOCK_STREAM, 0,
			  config.transparent_source, bind_address,
			  address,
			  timeout,
			  handler, cancel_ptr);
}

void
LbCluster::OnAvahiNewObject(const std::string &key,
			    SocketAddress address) noexcept
{
	ZeroconfMemberMap::insert_commit_data hint;
	auto result = zeroconf_members.insert_check(key,
						    zeroconf_members.key_comp(),
						    hint);
	if (result.second) {
		auto *member = new ZeroconfMember(key, address, failure_manager.Make(address),
						  monitors);
		zeroconf_members.insert_commit(*member, hint);
	} else {
		/* update existing member */
		result.first->SetAddress(address);
	}

	dirty = true;
}

void
LbCluster::OnAvahiRemoveObject(const std::string &key) noexcept
{
	auto i = zeroconf_members.find(key, zeroconf_members.key_comp());
	if (i == zeroconf_members.end())
		return;

	/* TODO: purge entry from the "failure" map, because it
	   will never be used again anyway */

	zeroconf_members.erase_and_dispose(i, DeleteDisposer());
	dirty = true;
}

#endif
