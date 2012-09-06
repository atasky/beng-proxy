/*
 * Session management.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_SESSION_MANAGER_H
#define BENG_PROXY_SESSION_MANAGER_H

#include <inline/compiler.h>

#include <stdbool.h>

struct session;

/**
 * Initialize the global session manager or increase the reference
 * counter.
 *
 * @param idle_timeout the idle timeout of sessions [seconds]
 * @param cluster_size the number of nodes in the cluster
 * @param cluster_node the index of this node in the cluster
 */
bool
session_manager_init(unsigned idle_timeout,
                     unsigned cluster_size, unsigned cluster_node);

/**
 * Decrease the reference counter and destroy the global session
 * manager if it has become zero.
 */
void
session_manager_deinit(void);

/**
 * Release the session manager and try not to access the shared
 * memory, because we assume it may be corrupted.
 */
void
session_manager_abandon(void);

/**
 * Re-add all libevent events after session_manager_event_del().
 */
void
session_manager_event_add(void);

/**
 * Removes all libevent events.  Call this before fork(), or before
 * creating a new event base.  Don't forget to call
 * session_manager_event_add() afterwards.
 */
void
session_manager_event_del(void);

/**
 * Returns the number of sessions.
 */
gcc_pure
unsigned
session_manager_get_count(void);

/**
 * Create a new #dpool object.  The caller is responsible for
 * destroying it or adding a new session with this #dpool, see
 * session_manager_add().
 */
struct dpool *
session_manager_new_dpool(void);

/**
 * Add an initialized #session object to the session manager.  Its
 * #dpool will be destroyed automatically when the session expires.
 * After returning from this function, the session is protected and
 * the pointer must not be used, unless it is looked up (and thus
 * locked).
 */
void
session_manager_add(struct session *session);

/**
 * Create a new session with a random session id.
 *
 * The returned session object is locked and must be unlocked with
 * session_put().
 */
struct session * gcc_malloc
session_new(void);

/**
 * Invoke the callback for each session.  The session and the session
 * manager will be locked during the callback.
 */
bool
session_manager_visit(bool (*callback)(const struct session *session,
                                       void *ctx), void *ctx);

#endif
