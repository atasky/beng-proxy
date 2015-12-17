/*
 * The StockMap class is a hash table of any number of Stock objects,
 * each with a different URI.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "MapStock.hxx"
#include "Stock.hxx"
#include "Item.hxx"
#include "pool.hxx"
#include "util/djbhash.h"
#include "util/DeleteDisposer.hxx"

#include <daemon/log.h>

#include <assert.h>
#include <string.h>

#include <boost/intrusive/unordered_set.hpp>

struct StockMap final : StockHandler {
    struct Item
        : boost::intrusive::unordered_set_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>> {
        const char *const uri;

        Stock &stock;

        Item(const char *_uri, Stock &_stock):uri(_uri), stock(_stock) {}

        Item(const Item &) = delete;

        ~Item() {
            delete &stock;
        }

        gcc_pure
        static size_t KeyHasher(const char *key) {
            assert(key != nullptr);

            return djb_hash_string(key);
        }

        gcc_pure
        static size_t ValueHasher(const Item &value) {
            return KeyHasher(value.uri);
        }

        gcc_pure
        static bool KeyValueEqual(const char *a, const Item &b) {
            assert(a != nullptr);

            return strcmp(a, b.uri) == 0;
        }

        struct Hash {
            gcc_pure
            size_t operator()(const Item &value) const {
                return ValueHasher(value);
            }
        };

        struct Equal {
            gcc_pure
            bool operator()(const Item &a, const Item &b) const {
                return KeyValueEqual(a.uri, b);
            }
        };
    };

    typedef boost::intrusive::unordered_set<Item,
                                            boost::intrusive::hash<Item::Hash>,
                                            boost::intrusive::equal<Item::Equal>,
                                            boost::intrusive::constant_time_size<false>> Map;

    struct pool &pool;
    const StockClass &cls;
    void *const class_ctx;

    /**
     * The maximum number of items in each stock.
     */
    const unsigned limit;

    /**
     * The maximum number of permanent idle items in each stock.
     */
    const unsigned max_idle;

    Map map;

    static constexpr size_t N_BUCKETS = 251;
    Map::bucket_type buckets[N_BUCKETS];

    StockMap(struct pool &_pool, const StockClass &_cls, void *_class_ctx,
             unsigned _limit, unsigned _max_idle)
        :pool(*pool_new_libc(&_pool, "hstock")),
         cls(_cls), class_ctx(_class_ctx),
         limit(_limit), max_idle(_max_idle),
         map(Map::bucket_traits(buckets, N_BUCKETS)) {}

    ~StockMap() {
        map.clear_and_dispose(DeleteDisposer());
        pool_unref(&pool);
    }

    void Erase(gcc_unused Stock &stock, const char *uri) {
#ifndef NDEBUG
        auto i = map.find(uri, Item::KeyHasher, Item::KeyValueEqual);
        assert(i != map.end());
        assert(&i->stock == &stock);
#endif

        map.erase_and_dispose(uri, Item::KeyHasher, Item::KeyValueEqual,
                              DeleteDisposer());
    }

    void FadeAll() {
        for (auto &i : map)
            i.stock.FadeAll();
    }

    void AddStats(StockStats &data) const {
        for (const auto &i : map)
            i.stock.AddStats(data);
    }

    Stock &GetStock(const char *uri);

    void Get(struct pool &caller_pool,
             const char *uri, void *info,
             StockGetHandler &handler,
             struct async_operation_ref &async_ref) {
        Stock &stock = GetStock(uri);
        stock.Get(caller_pool, info, handler, async_ref);
    }

    StockItem *GetNow(struct pool &caller_pool, const char *uri, void *info,
                      GError **error_r) {
        Stock &stock = GetStock(uri);
        return stock.GetNow(caller_pool, info, error_r);
    }

    void Put(gcc_unused const char *uri, StockItem &object, bool destroy) {
#ifndef NDEBUG
        auto i = map.find(uri, Item::KeyHasher, Item::KeyValueEqual);
        assert(i != map.end());
        assert(&i->stock == &object.stock);
#endif

        object.Put(destroy);
    }

    /* virtual methods from class StockHandler */
    void OnStockEmpty(Stock &stock, const char *uri) override;
};

void
StockMap::OnStockEmpty(Stock &stock, const char *uri)
{
    daemon_log(5, "hstock(%p) remove empty stock(%p, '%s')\n",
               (const void *)this, (const void *)&stock, uri);

    Erase(stock, uri);
}

StockMap *
hstock_new(struct pool &pool, const StockClass &cls, void *class_ctx,
           unsigned limit, unsigned max_idle)
{
    assert(max_idle > 0);

    return new StockMap(pool, cls, class_ctx,
                        limit, max_idle);
}

void
hstock_free(StockMap *hstock)
{
    delete hstock;
}

void
hstock_fade_all(StockMap &hstock)
{
    hstock.FadeAll();
}

void
hstock_add_stats(const StockMap &stock, StockStats &data)
{
    stock.AddStats(data);
}

inline Stock &
StockMap::GetStock(const char *uri)
{
    Map::insert_commit_data hint;
    auto i = map.insert_check(uri, Item::KeyHasher, Item::KeyValueEqual, hint);
    if (i.second) {
        auto *stock = new Stock(pool, cls, class_ctx,
                                uri, limit, max_idle,
                                this);
        map.insert_commit(*new Item(stock->GetUri(), *stock), hint);
        return *stock;
    } else
        return i.first->stock;

}

void
hstock_get(StockMap &hstock, struct pool &pool,
           const char *uri, void *info,
           StockGetHandler &handler,
           struct async_operation_ref &async_ref)
{
    return hstock.Get(pool, uri, info, handler, async_ref);
}

StockItem *
hstock_get_now(StockMap &hstock, struct pool &pool,
               const char *uri, void *info,
               GError **error_r)
{
    return hstock.GetNow(pool, uri, info, error_r);
}

void
hstock_put(StockMap &hstock, const char *uri, StockItem &object, bool destroy)
{
    hstock.Put(uri, object, destroy);
}
