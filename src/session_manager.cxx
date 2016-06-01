/*
 * Session management.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "session_manager.hxx"
#include "session.hxx"
#include "shm/shm.hxx"
#include "shm/dpool.hxx"
#include "shm/rwlock.hxx"
#include "random.hxx"
#include "expiry.h"
#include "crash.hxx"
#include "system/clock.h"
#include "event/TimerEvent.hxx"
#include "util/StaticArray.hxx"
#include "util/RefCount.hxx"

#include <daemon/log.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>

struct SessionHash {
    gcc_pure
    size_t operator()(const SessionId &id) const {
        return id.Hash();
    }

    gcc_pure
    size_t operator()(const Session &session) const {
        return session.id.Hash();
    }
};

struct SessionEqual {
    gcc_pure
    bool operator()(const Session &a, const Session &b) const {
        return a.id == b.id;
    }

    gcc_pure
    bool operator()(const SessionId &a, const Session &b) const {
        return a == b.id;
    }
};

struct SessionDisposer {
    void operator()(Session *session) {
        session_destroy(session);
    }
};

template<typename Container, typename Pred, typename Disposer>
static void
EraseAndDisposeIf(Container &container, Pred pred, Disposer disposer)
{
    for (auto i = container.begin(), end = container.end(); i != end;) {
        const auto next = std::next(i);

        if (pred(*i))
            container.erase_and_dispose(i, disposer);

        i = next;
    }
}

struct SessionManager {
    RefCount ref;

    /**
     * The idle timeout of sessions [seconds].
     */
    const unsigned idle_timeout;

    const unsigned cluster_size, cluster_node;

    struct shm *const shm;

    /** this lock protects the following hash table */
    ShmRwLock lock;

    /**
     * Has the session manager been abandoned after the crash of one
     * worker?  If this is true, then the session manager is disabled,
     * and the remaining workers will be shut down soon.
     */
    bool abandoned;

    typedef boost::intrusive::unordered_set<Session,
                                            boost::intrusive::member_hook<Session,
                                                                          Session::SetHook,
                                                                          &Session::set_hook>,
                                            boost::intrusive::hash<SessionHash>,
                                            boost::intrusive::equal<SessionEqual>,
                                            boost::intrusive::constant_time_size<true>> Set;
    Set sessions;

    static constexpr unsigned N_BUCKETS = 16381;
    Set::bucket_type buckets[N_BUCKETS];

    SessionManager(unsigned _idle_timeout,
                   unsigned _cluster_size, unsigned _cluster_node,
                   struct shm *_shm)
        :idle_timeout(_idle_timeout),
         cluster_size(_cluster_size),
         cluster_node(_cluster_node),
         shm(_shm),
         abandoned(false),
         sessions(Set::bucket_traits(buckets, N_BUCKETS)) {
        ref.Init();
    }

    ~SessionManager();

    void Ref() {
        ref.Get();
        shm_ref(shm);
    }

    void Unref() {
        if (ref.Put())
            this->~SessionManager();
    }

    void Abandon();

    Session *Find(SessionId id);

    Session *LockFind(SessionId id) {
        ScopeShmReadLock read_lock(lock);
        return Find(id);
    }

    void Insert(Session &session) {
        sessions.insert(session);
    }

    void LockInsert(Session &session);

    void EraseAndDispose(Session *session);
    void EraseAndDispose(SessionId id);

    /**
     * @return true if there is at least one session
     */
    bool Cleanup();

    /**
     * Forcefully deletes at least one session.
     */
    bool Purge();

    bool Visit(bool (*callback)(const Session *session,
                                void *ctx), void *ctx);
};

static constexpr size_t SHM_PAGE_SIZE = 4096;
static constexpr unsigned SHM_NUM_PAGES = 65536;
static constexpr unsigned SM_PAGES = (sizeof(SessionManager) + SHM_PAGE_SIZE - 1) / SHM_PAGE_SIZE;

/** clean up expired sessions every 60 seconds */
static const struct timeval cleanup_interval = {
    .tv_sec = 60,
    .tv_usec = 0,
};

/** the one and only session manager instance, allocated from shared
    memory */
static SessionManager *session_manager;

/* this must be a separate variable, because session_manager is
   allocated from shared memory, and each process must manage its own
   event struct */
static TimerEvent session_cleanup_event;

#ifndef NDEBUG
/**
 * A process must not lock more than one session at a time, or it will
 * risk deadlocking itself.  For the assertions in this source, this
 * variable holds a reference to the locked session.
 */
static const Session *locked_session;
#endif

void
SessionManager::EraseAndDispose(Session *session)
{
    assert(crash_in_unsafe());
    assert(lock.IsWriteLocked());
    assert(!sessions.empty());

    auto i = sessions.iterator_to(*session);
    sessions.erase_and_dispose(i, SessionDisposer());

    if (sessions.empty())
        session_cleanup_event.Cancel();
}

inline bool
SessionManager::Cleanup()
{
    assert(!crash_in_unsafe());
    assert(locked_session == nullptr);

    const unsigned now = now_s();

    const ScopeCrashUnsafe crash_unsafe;
    ScopeShmWriteLock write_lock(lock);

    if (abandoned) {
        assert(!crash_in_unsafe());
        return false;
    }

    EraseAndDisposeIf(sessions, [now](const Session &session){
            return now >= unsigned(session.expires);
        }, SessionDisposer());

    return !sessions.empty();
}

static void
cleanup_event_callback(int fd gcc_unused, short event gcc_unused,
                       void *ctx gcc_unused)
{
    if (session_manager->Cleanup())
        session_cleanup_event.Add(cleanup_interval);

    assert(!crash_in_unsafe());
}

static SessionManager *
session_manager_new(unsigned idle_timeout,
                    unsigned cluster_size, unsigned cluster_node)
{
    struct shm *shm = shm_new(SHM_PAGE_SIZE, SHM_NUM_PAGES);
    return NewFromShm<SessionManager>(shm, SM_PAGES,
                                      idle_timeout,
                                      cluster_size, cluster_node,
                                      shm);
}

void
session_manager_init(unsigned idle_timeout,
                     unsigned cluster_size, unsigned cluster_node)
{
    assert((cluster_size == 0 && cluster_node == 0) ||
           cluster_node < cluster_size);

    random_seed();

    if (session_manager == nullptr) {
        session_manager = session_manager_new(idle_timeout,
                                              cluster_size, cluster_node);
        if (session_manager == nullptr)
            throw std::runtime_error("shm allocation failed");
    } else {
        session_manager->Ref();
    }

    session_cleanup_event.Init(cleanup_event_callback, nullptr);
}

inline
SessionManager::~SessionManager()
{
    const ScopeCrashUnsafe crash_unsafe;
    ScopeShmWriteLock write_lock(lock);

    sessions.clear_and_dispose(SessionDisposer());
}

void
session_manager_deinit()
{
    assert(session_manager != nullptr);
    assert(session_manager->shm != nullptr);
    assert(locked_session == nullptr);

    session_cleanup_event.Cancel();

    struct shm *shm = session_manager->shm;

    session_manager->Unref();
    session_manager = nullptr;

    /* we always destroy the SHM section, because it is not used
       anymore by this process; other processes may still use it */
    shm_close(shm);
}

inline void
SessionManager::Abandon()
{
    assert(shm != nullptr);

    abandoned = true;

    /* XXX move the "shm" pointer out of the shared memory */
    shm_close(shm);
}

void
session_manager_abandon()
{
    assert(session_manager != nullptr);

    session_cleanup_event.Cancel();

    session_manager->Abandon();
    session_manager = nullptr;
}

void
session_manager_event_add()
{
    if (!session_manager->sessions.empty())
        session_cleanup_event.Add(cleanup_interval);
}

void
session_manager_event_del()
{
    session_cleanup_event.Cancel();
}

unsigned
session_manager_get_count()
{
    return session_manager->sessions.size();
}

struct dpool *
session_manager_new_dpool()
{
    return dpool_new(*session_manager->shm);
}

bool
SessionManager::Purge()
{
    /* collect at most 256 sessions */
    StaticArray<Session *, 256> purge_sessions;
    unsigned highest_score = 0;

    assert(locked_session == nullptr);

    const ScopeCrashUnsafe crash_unsafe;
    ScopeShmWriteLock write_lock(lock);

    for (auto &session : sessions) {
        unsigned score = session_purge_score(&session);
        if (score > highest_score) {
            purge_sessions.clear();
            highest_score = score;
        }

        if (score == highest_score)
            purge_sessions.checked_append(&session);
    }

    if (purge_sessions.empty())
        return false;

    daemon_log(3, "purging %u sessions (score=%u)\n",
               (unsigned)purge_sessions.size(), highest_score);

    for (auto session : purge_sessions) {
        lock_lock(&session->lock);
        EraseAndDispose(session);
    }

    /* purge again if the highest score group has only very few items,
       which would lead to calling this (very expensive) function too
       often */
    bool again = purge_sessions.size() < 16 &&
        session_manager->sessions.size() > SHM_NUM_PAGES - 256;

    write_lock.Unlock();

    if (again)
        Purge();

    return true;
}

inline void
SessionManager::LockInsert(Session &session)
{
    {
        ScopeShmWriteLock write_lock(lock);
        Insert(session);
    }

    if (!session_cleanup_event.IsPending())
        session_cleanup_event.Add(cleanup_interval);
}

void
session_manager_add(Session &session)
{
    session_manager->LockInsert(session);
}

static void
session_generate_id(SessionId *id_r)
{
    id_r->Generate();

    if (session_manager != nullptr && session_manager->cluster_size > 0)
        id_r->SetClusterNode(session_manager->cluster_size,
                             session_manager->cluster_node);
}

static Session *
session_new_unsafe(const char *realm)
{
    assert(crash_in_unsafe());
    assert(locked_session == nullptr);

    if (session_manager->abandoned)
        return nullptr;

    struct dpool *pool = dpool_new(*session_manager->shm);
    if (pool == nullptr) {
        if (!session_manager->Purge())
            return nullptr;

        /* at least one session has been purged: try again */
        pool = dpool_new(*session_manager->shm);
        if (pool == nullptr)
            /* nope. fail. */
            return nullptr;
    }

    Session *session = session_allocate(pool, realm);
    if (session == nullptr) {
        dpool_destroy(pool);
        return nullptr;
    }

    session_generate_id(&session->id);

#ifndef NDEBUG
    locked_session = session;
#endif
    lock_lock(&session->lock);

    session_manager->LockInsert(*session);

    return session;
}

Session *
session_new(const char *realm)
{
    crash_unsafe_enter();
    Session *session = session_new_unsafe(realm);
    if (session == nullptr)
        crash_unsafe_leave();
    return session;
}

/**
 * After a while the dpool may have fragmentations, and memory is
 * wasted.  This function duplicates the session into a fresh dpool,
 * and frees the old session instance.  Of course, this requires that
 * there is enough free shared memory.
 */
static Session * gcc_malloc
session_defragment(Session *src)
{
    assert(crash_in_unsafe());

    struct dpool *pool = dpool_new(*session_manager->shm);
    if (pool == nullptr)
        return nullptr;

    Session *dest = session_dup(pool, src);
    if (dest == nullptr) {
        dpool_destroy(pool);
        return src;
    }

    session_manager->sessions.insert(*dest);
    session_manager->EraseAndDispose(src);
    return dest;
}

Session *
SessionManager::Find(SessionId id)
{
    if (abandoned)
        return nullptr;

    assert(crash_in_unsafe());
    assert(locked_session == nullptr);

    auto i = sessions.find(id, SessionHash(), SessionEqual());
    if (i == sessions.end())
        return nullptr;

    Session &session = *i;

#ifndef NDEBUG
    locked_session = &session;
#endif
    lock_lock(&session.lock);

    session.expires = expiry_touch(idle_timeout);
    ++session.counter;
    return &session;
}

Session *
session_get(SessionId id)
{
    assert(locked_session == nullptr);

    crash_unsafe_enter();

    Session *session = session_manager->LockFind(id);

    if (session == nullptr)
        crash_unsafe_leave();

    return session;
}

static void
session_put_internal(Session *session)
{
    assert(crash_in_unsafe());
    assert(session == locked_session);

    lock_unlock(&session->lock);

#ifndef NDEBUG
    locked_session = nullptr;
#endif
}

static void
session_defragment_id(SessionId id)
{
    assert(crash_in_unsafe());

    Session *session = session_manager->Find(id);
    if (session == nullptr)
        return;

    /* unlock the session, because session_defragment() may call
       SessionManager::EraseAndDispose(), and
       SessionManager::EraseAndDispose() expects the session to be
       unlocked.  This is ok, because we're holding the session
       manager lock at this point. */
    session_put_internal(session);

    session_defragment(session);
}

void
session_put(Session *session)
{
    SessionId defragment;

    if ((session->counter % 1024) == 0 &&
        dpool_is_fragmented(*session->pool))
        defragment = session->id;
    else
        defragment.Clear();

    session_put_internal(session);

    if (defragment.IsDefined()) {
        /* the shared memory pool has become too fragmented;
           defragment the session by duplicating it into a new shared
           memory pool */

        ScopeShmWriteLock write_lock(session_manager->lock);
        session_defragment_id(defragment);
    }

    crash_unsafe_leave();
}

void
SessionManager::EraseAndDispose(SessionId id)
{
    assert(locked_session == nullptr);

    const ScopeCrashUnsafe crash_unsafe;
    ScopeShmWriteLock write_lock(lock);

    Session *session = session_manager->Find(id);
    if (session != nullptr) {
        session_put_internal(session);
        EraseAndDispose(session);
    }
}

void
session_delete(SessionId id)
{
    session_manager->EraseAndDispose(id);
}

inline bool
SessionManager::Visit(bool (*callback)(const Session *session,
                                       void *ctx), void *ctx)
{
    const ScopeCrashUnsafe crash_unsafe;
    ScopeShmReadLock read_lock(lock);

    if (abandoned) {
        return false;
    }

    const unsigned now = now_s();

    for (auto &session : sessions) {
        if (now >= (unsigned)session.expires)
            continue;

        lock_lock(&session.lock);
        bool result = callback(&session, ctx);
        lock_unlock(&session.lock);

        if (!result)
            return false;
    }

    return true;
}

bool
session_manager_visit(bool (*callback)(const Session *session,
                                       void *ctx), void *ctx)
{
    return session_manager->Visit(callback, ctx);
}
