/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_EXPAND_HXX
#define BENG_PROXY_EXPAND_HXX

#include <inline/compiler.h>

#include <glib.h>

#include <assert.h>
#include <string.h>

gcc_const
static inline GQuark
expand_quark(void)
{
    return g_quark_from_static_string("expand");
}

template<typename Result, typename MatchInfo>
bool
ExpandString(Result &result, const char *src,
             MatchInfo &&match_info, GError **error_r)
{
    assert(src != nullptr);

    while (true) {
        const char *backslash = strchr(src, '\\');
        if (backslash == nullptr) {
            /* append the remaining input string and return */
            result.Append(src);
            return true;
        }

        /* copy everything up to the backslash */
        result.Append(src, backslash - src);

        /* now evaluate the escape */
        src = backslash + 1;
        const char ch = *src++;
        if (ch == '\\')
            result.Append(ch);
        else if (ch >= '0' && ch <= '9') {
            auto c = match_info.GetCapture(ch - '0');
            if (!c.IsEmpty())
                result.AppendValue(c.data, c.size);
        } else {
            g_set_error(error_r, expand_quark(), 0,
                        "Invalid backslash escape (0x%02x)", ch);
            return false;
        }
    }
}

#endif
