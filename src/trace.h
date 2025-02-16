/*
 * Copyright 2007-2022 CM4all GmbH
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Trace parameters for functions.
 */

#ifndef __BENG_TRACE_H
#define __BENG_TRACE_H

#ifdef TRACE

#define TRACE_ARGS_DECL , const char *file, unsigned line
#define TRACE_ARGS_DECL_ , const char *_file, unsigned _line
#define TRACE_ARGS_DEFAULT , const char *file=__builtin_FILE(), unsigned line=__builtin_LINE()
#define TRACE_ARGS_DEFAULT_ , const char *_file=__builtin_FILE(), unsigned _line=__builtin_LINE()
#define TRACE_ARGS_FWD , file, line
#define TRACE_ARGS_IGNORE { (void)file; (void)line; }
#define TRACE_ARGS_INIT , file(_file), line(_line)
#define TRACE_ARGS_INIT_FROM(src) , file((src).file), line((src).line)

#else

#define TRACE_ARGS_DECL
#define TRACE_ARGS_DECL_
#define TRACE_ARGS_DEFAULT
#define TRACE_ARGS_DEFAULT_
#define TRACE_ARGS_FWD
#define TRACE_ARGS_IGNORE
#define TRACE_ARGS_INIT
#define TRACE_ARGS_INIT_FROM(src)

#endif

#endif
