// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * theme/artwork.h - the album art the deck draws, for whoever needs it.
 *
 * Public because it is wanted outside the theme mod: USB Save puts a cover in the
 * file it writes, and this is the only place a cover is reachable. See artwork.c
 * for why it is a copy, why it is the rendered bitmap rather than the cover file,
 * and why the fingerprint rather than the buffer's address.
 */
#ifndef EP122_MOD_THEME_ARTWORK_H
#define EP122_MOD_THEME_ARTWORK_H

#include <stddef.h>
#include <stdint.h>

/* Called from the draw path with the ImagePixelData of an image about to be
 * drawn; keeps it if it is the album art. Cheap when it is not, and a no-op once
 * this image has already been taken. */
void theme_artwork_note(void *pd);                                          /* [render] */

/* 0 when no cover has been drawn yet. */
int  theme_artwork_size(int32_t *w, int32_t *h);                            /* [any] */

/* Packed RGB out, three bytes per pixel, row-major, no padding -- the layout
 * usbsave/png.c encodes. Bytes written, 0 when there is no cover, -1 on a bad
 * argument or a buffer too short. */
int  theme_artwork_rgb(uint8_t *dst, size_t cap, int32_t *w, int32_t *h);   /* [any] */

#endif /* EP122_MOD_THEME_ARTWORK_H */
