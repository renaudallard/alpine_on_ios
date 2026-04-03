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

#include <sys/time.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "vfs_input.h"
#include "log.h"

/* Linux input event types */
#define EV_SYN		0x00
#define EV_KEY		0x01
#define EV_ABS		0x03

/* Sync codes */
#define SYN_REPORT	0x00

/* Key codes */
#define BTN_LEFT	0x110

/* Absolute axis codes */
#define ABS_X		0x00
#define ABS_Y		0x01

/* Linux input_event (as seen by AArch64 processes) */
struct input_event {
	uint64_t	time_sec;
	uint64_t	time_usec;
	uint16_t	type;
	uint16_t	code;
	int32_t		value;
};

/* Pipe-based input device */
static int	g_pipe[2] = {-1, -1};
static int	g_active;

int
input_init(void)
{
	if (g_active)
		return (0);

	if (pipe(g_pipe) < 0) {
		LOG_ERR("input_init: pipe failed");
		return (-1);
	}

	g_active = 1;
	LOG_INFO("input_init: fd_read=%d fd_write=%d",
	    g_pipe[0], g_pipe[1]);
	return (0);
}

void
input_destroy(void)
{
	if (!g_active)
		return;

	if (g_pipe[0] >= 0)
		close(g_pipe[0]);
	if (g_pipe[1] >= 0)
		close(g_pipe[1]);
	g_pipe[0] = -1;
	g_pipe[1] = -1;
	g_active = 0;
}

int
input_get_fd(void)
{
	return (g_pipe[0]);
}

/* Write a single input_event to the pipe. */
static void
emit_event(uint16_t type, uint16_t code, int32_t value)
{
	struct input_event	ev;
	struct timeval		tv;

	if (!g_active || g_pipe[1] < 0)
		return;

	gettimeofday(&tv, NULL);
	memset(&ev, 0, sizeof(ev));
	ev.time_sec = (uint64_t)tv.tv_sec;
	ev.time_usec = (uint64_t)tv.tv_usec;
	ev.type = type;
	ev.code = code;
	ev.value = value;

	(void)write(g_pipe[1], &ev, sizeof(ev));
}

/* Emit SYN_REPORT to terminate an event batch. */
static void
emit_syn(void)
{
	emit_event(EV_SYN, SYN_REPORT, 0);
}

void
input_send_touch(int x, int y, int pressed)
{
	emit_event(EV_ABS, ABS_X, x);
	emit_event(EV_ABS, ABS_Y, y);
	emit_event(EV_KEY, BTN_LEFT, pressed);
	emit_syn();
}

void
input_send_key(int keycode, int pressed)
{
	emit_event(EV_KEY, (uint16_t)keycode, pressed);
	emit_syn();
}
