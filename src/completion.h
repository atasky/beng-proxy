/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_COMPLETION_H
#define BENG_PROXY_COMPLETION_H

/**
 * A return value type which indicates the state of the desired
 * operation.
 */
enum completion {
    /**
     * The operation completed.
     */
    C_DONE,

    /**
     * Partial completion.
     */
    C_PARTIAL,

    /**
     * No progress this time.
     */
    C_NONE,

    /**
     * An error has occurred.  Details are returned in a GError object.
     */
    C_ERROR,

    /**
     * The object has been closed/destroyed.  Details have been
     * supplied to the handler.
     */
    C_CLOSED,
};

#endif
