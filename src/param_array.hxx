/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_PARAM_ARRAY_HXX
#define BENG_PROXY_PARAM_ARRAY_HXX

#include "util/ConstBuffer.hxx"

#include <inline/compiler.h>

#include <assert.h>
#include <stddef.h>

struct pool;
class MatchInfo;
class Error;

/**
 * An array of parameter strings.
 */
struct param_array {
    static constexpr size_t CAPACITY = 32;

    unsigned n;

    /**
     * Command-line arguments.
     */
    const char *values[CAPACITY];

    bool expand_values[CAPACITY];

    param_array() = default;

    param_array(struct pool &pool, const struct param_array &src);

    void Init() {
        n = 0;
    }

    constexpr bool IsFull() const {
        return n == CAPACITY;
    }

    const char *const*begin() const {
        return &values[0];
    }

    const char *const*end() const {
        return &values[n];
    }

    void CopyFrom(struct pool *pool, const struct param_array &src);

    void Append(const char *value) {
        assert(n <= CAPACITY);

        const unsigned i = n++;

        values[i] = value;
        expand_values[i] = false;
    }

    bool CanSetExpand() const {
        assert(n <= CAPACITY);

        return n > 0 && !expand_values[n - 1];
    }

    void SetExpand(const char *value) {
        assert(CanSetExpand());

        values[n - 1] = value;
        expand_values[n - 1] = true;
    }

    gcc_pure
    bool IsExpandable() const;

    bool Expand(struct pool *pool,
                const MatchInfo &match_info, Error &error_r);

    constexpr operator ConstBuffer<const char *>() const {
        return {values, n};
    }
};

#endif
