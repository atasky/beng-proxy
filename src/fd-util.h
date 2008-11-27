/*
 * Utilities for file descriptors.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_FD_UTIL_H
#define __BENG_FD_UTIL_H

#include <stdbool.h>

int
fd_mask_descriptor_flags(int fd, int and_mask, int xor_mask);

int
fd_set_cloexec(int fd);

int
fd_mask_status_flags(int fd, int and_mask, int xor_mask);

bool
fd_ready_for_writing(int fd);

#endif
