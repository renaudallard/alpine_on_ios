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

#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "framebuffer.h"
#include "log.h"

/* Global framebuffer instance */
static framebuffer_t g_fb;

int
fb_init(uint32_t width, uint32_t height)
{
	size_t	size;

	if (g_fb.active)
		return (0);

	if (width == 0)
		width = FB_DEFAULT_WIDTH;
	if (height == 0)
		height = FB_DEFAULT_HEIGHT;

	g_fb.width = width;
	g_fb.height = height;
	g_fb.stride = width * FB_BYTES_PER_PIXEL;
	size = (size_t)g_fb.stride * height;
	g_fb.size = size;

	g_fb.pixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g_fb.pixels == MAP_FAILED) {
		LOG_ERR("fb_init: mmap failed");
		g_fb.pixels = NULL;
		return (-1);
	}

	memset(g_fb.pixels, 0, size);
	g_fb.active = 1;

	LOG_INFO("fb_init: %ux%u stride=%u size=%zu",
	    width, height, g_fb.stride, size);
	return (0);
}

void *
fb_get_pixels(void)
{
	if (!g_fb.active)
		return (NULL);
	return (g_fb.pixels);
}

void
fb_get_size(uint32_t *width, uint32_t *height)
{
	if (width != NULL)
		*width = g_fb.width;
	if (height != NULL)
		*height = g_fb.height;
}

void
fb_destroy(void)
{
	if (!g_fb.active)
		return;

	if (g_fb.pixels != NULL) {
		munmap(g_fb.pixels, g_fb.size);
		g_fb.pixels = NULL;
	}
	g_fb.active = 0;
}

framebuffer_t *
fb_get(void)
{
	return (&g_fb);
}

int
fb_get_fd(void)
{
	/*
	 * Return a dummy fd for the framebuffer. We use /dev/zero as the
	 * backing fd; the real pixel buffer is provided via the fb_get_pixels
	 * API.  The ioctl and mmap handling in sys_file.c detect the FD_FB
	 * type and route to framebuffer-specific code.
	 */
	return (open("/dev/zero", O_RDWR));
}
