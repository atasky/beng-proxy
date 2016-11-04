/*
 * Utilities for dealing with regular expressions.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "regex.hxx"
#include "expand.hxx"
#include "util/RuntimeError.hxx"

#include <assert.h>
#include <string.h>

void
UniqueRegex::Compile(const char *pattern, bool anchored, bool capture)
{
    constexpr int default_options = PCRE_DOTALL|PCRE_NO_AUTO_CAPTURE;

    int options = default_options;
    if (anchored)
        options |= PCRE_ANCHORED;
    if (capture)
        options &= ~PCRE_NO_AUTO_CAPTURE;

    const char *error_string;
    int error_offset;
    re = pcre_compile(pattern, options, &error_string, &error_offset, nullptr);
    if (re == nullptr)
        throw FormatRuntimeError("Error in regex at offset %d: %s",
                                 error_offset, error_string);

    int study_options = 0;
#ifdef PCRE_CONFIG_JIT
    study_options |= PCRE_STUDY_JIT_COMPILE;
#endif
    extra = pcre_study(re, study_options, &error_string);
    if (extra == nullptr && error_string != nullptr) {
        pcre_free(re);
        re = nullptr;
        throw FormatRuntimeError("Regex study error: %s", error_string);
    }

    int n;
    if (capture && pcre_fullinfo(re, extra, PCRE_INFO_CAPTURECOUNT, &n) == 0)
        n_capture = n;
}

size_t
ExpandStringLength(const char *src, MatchInfo match_info)
{
    struct Result {
        size_t result = 0;

        void Append(gcc_unused char ch) {
            ++result;
        }

        void Append(const char *p) {
            result += strlen(p);
        }

        void Append(gcc_unused const char *p, size_t length) {
            result += length;
        }

        void AppendValue(gcc_unused const char *p, size_t length) {
            result += length;
        }

        size_t Commit() const {
            return result;
        }
    };

    Result result;
    ExpandString(result, src, match_info);
    return result.Commit();
}
