/*
 * Copyright (c) 2026 Alpine on iOS contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>

#include "log.h"

static int log_level = LOG_LVL_INFO;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_prefix[] = {
	"[ERR]",
	"[WARN]",
	"[INFO]",
	"[DBG]",
	"[TRACE]"
};

void
log_init(int level)
{
	log_level = level;
}

void
log_set_level(int level)
{
	log_level = level;
}

void
log_msg(int level, const char *fmt, ...)
{
	va_list	ap;

	if (level > log_level)
		return;

	pthread_mutex_lock(&log_lock);

	if (level >= 0 && level <= LOG_LVL_TRACE)
		fprintf(stderr, "%s ", level_prefix[level]);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fputc('\n', stderr);

	pthread_mutex_unlock(&log_lock);
}
