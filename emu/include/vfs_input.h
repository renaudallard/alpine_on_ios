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

#ifndef VFS_INPUT_H
#define VFS_INPUT_H

/* Initialize the input device (pipe-backed evdev emulation) */
int	 input_init(void);

/* Destroy the input device */
void	 input_destroy(void);

/* Get the read fd for the guest process */
int	 input_get_fd(void);

/* Send a touch event: x, y in framebuffer coordinates, pressed 1/0 */
void	 input_send_touch(int x, int y, int pressed);

/* Send a key event: Linux keycode, pressed 1/0 */
void	 input_send_key(int keycode, int pressed);

#endif /* VFS_INPUT_H */
