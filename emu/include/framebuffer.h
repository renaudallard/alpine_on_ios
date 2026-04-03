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

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>

#define FB_DEFAULT_WIDTH	1280
#define FB_DEFAULT_HEIGHT	720
#define FB_BPP			32
#define FB_BYTES_PER_PIXEL	4

/* Linux framebuffer ioctl numbers */
#define FBIOGET_VSCREENINFO	0x4600
#define FBIOPUT_VSCREENINFO	0x4601
#define FBIOGET_FSCREENINFO	0x4602

/* Linux fb_var_screeninfo (simplified) */
struct fb_var_screeninfo {
	uint32_t xres, yres;
	uint32_t xres_virtual, yres_virtual;
	uint32_t xoffset, yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct {
		uint32_t offset, length, msb_right;
	} red, green, blue, transp;
	uint32_t nonstd;
	uint32_t activate;
	uint32_t height, width;		/* mm */
	uint32_t accel_flags;
	uint32_t pixclock, left_margin, right_margin;
	uint32_t upper_margin, lower_margin;
	uint32_t hsync_len, vsync_len;
	uint32_t sync, vmode;
	uint32_t rotate, colorspace;
	uint32_t reserved[4];
};

/* Linux fb_fix_screeninfo (simplified) */
struct fb_fix_screeninfo {
	char		id[16];
	uint64_t	smem_start;
	uint32_t	smem_len;
	uint32_t	type;
	uint32_t	type_aux;
	uint32_t	visual;
	uint16_t	xpanstep, ypanstep, ywrapstep;
	uint16_t	__pad;
	uint32_t	line_length;
	uint64_t	mmio_start;
	uint32_t	mmio_len;
	uint32_t	accel;
	uint16_t	capabilities;
	uint16_t	reserved[2];
};

/* Global framebuffer state */
typedef struct framebuffer {
	uint32_t	width, height;
	uint32_t	stride;		/* bytes per row = width * 4 */
	uint8_t		*pixels;	/* BGRA pixel buffer */
	size_t		size;		/* total buffer size */
	int		active;		/* 1 if fb is initialized */
} framebuffer_t;

/* Initialize the framebuffer with given dimensions */
int		 fb_init(uint32_t width, uint32_t height);

/* Get pointer to the pixel buffer (for iOS to read) */
void		*fb_get_pixels(void);

/* Get framebuffer dimensions */
void		 fb_get_size(uint32_t *width, uint32_t *height);

/* Destroy the framebuffer */
void		 fb_destroy(void);

/* Get the global framebuffer (for ioctl handlers) */
framebuffer_t	*fb_get(void);

/* Get the framebuffer fd for use by the emulated process */
int		 fb_get_fd(void);

#endif /* FRAMEBUFFER_H */
