/*
 * Common string utilities.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "strutil.h"

void
str_to_lower(char *s)
{
    for (; *s != 0; ++s)
        char_to_lower_inplace(s);
}
