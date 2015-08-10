/*
 * Utilities for dealing with regular expressions.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "regex.hxx"
#include "expand.hxx"
#include "pool.hxx"
#include "uri_escape.hxx"
#include "util/Domain.hxx"
#include "util/Error.hxx"

#include <glib.h>

#include <assert.h>
#include <string.h>

static constexpr Domain regex_domain("regex");

bool
UniqueRegex::Compile(const char *pattern, bool capture, Error &error)
{
    GError *gerror = nullptr;
    bool success = Compile(pattern, capture, &gerror);
    if (!success) {
        error.Set(regex_domain, gerror->code, gerror->message);
        g_error_free(gerror);
    }

    return success;
}

const char *
expand_string(struct pool *pool, const char *src,
              const GMatchInfo *match_info, GError **error_r)
{
    assert(pool != nullptr);
    assert(src != nullptr);
    assert(match_info != nullptr);

    char *p = g_match_info_expand_references(match_info, src, error_r);
    if (p == nullptr)
        return nullptr;

    /* move result to the memory pool */
    char *q = p_strdup(pool, p);
    g_free(p);
    return q;
}

const char *
expand_string_unescaped(struct pool *pool, const char *src,
                        const GMatchInfo *match_info,
                        GError **error_r)
{
    assert(pool != nullptr);
    assert(src != nullptr);
    assert(match_info != nullptr);

    struct Result {
        GString *result = g_string_sized_new(256);

        ~Result() {
            g_string_free(result, true);
        }

        void Append(char ch) {
            g_string_append_c(result, ch);
        }

        void Append(const char *p) {
            g_string_append(result, p);
        }

        void Append(const char *p, size_t length) {
            g_string_append_len(result, p, length);
        }

        void AppendValue(char *p, size_t length) {
            Append(p, uri_unescape_inplace(p, length));
        }

        const char *Commit(struct pool &p) {
            return p_strndup(&p, result->str, result->len);
        }
    };

    Result result;
    return ExpandString(result, src, match_info, error_r)
        ? result.Commit(*pool)
        : nullptr;
}
