/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef SPAWN_HXX
#define SPAWN_HXX

#include <sys/types.h>

struct PreparedChildProcess;

/**
 * @return the process id, or a negative errno value
 */
pid_t
SpawnChildProcess(PreparedChildProcess &&params);

#endif
