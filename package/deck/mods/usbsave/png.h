// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/png.h - a PNG writer, for USB Save's cover art.
 *
 * STORED BLOCKS, NO COMPRESSION. A PNG's image data is a zlib stream, and zlib
 * allows a "stored" block: raw bytes with a five-byte header, no Huffman coding
 * and no match search. That is a legal PNG, every reader accepts it, and it costs
 * none of the complexity of a deflate implementation -- which for one 320x240
 * cover per saved track would be a great deal of code for a few tens of KB.
 *
 * The image goes in as truecolour (RGB, 8 bits per channel) with no alpha and no
 * interlacing, which is what a cover is: the deck's own artwork is opaque.
 *
 * Pure: caller-owned buffers, nothing outside the mods' libc subset. The two
 * checksums a PNG needs (CRC-32 per chunk, Adler-32 over the zlib stream) are
 * written out here rather than taken, for the same reason the FLAC writer carries
 * its own CRC -- a table would be the biggest thing in the file.
 */
#ifndef EP122_MOD_USBSAVE_PNG_H
#define EP122_MOD_USBSAVE_PNG_H

#include <stddef.h>
#include <stdint.h>

/* An upper bound for encoding w*h packed RGB. Counted rather than estimated:
 * signature (8) + IHDR (4+4+13+4) + IDAT's length, type and CRC (12) + IEND
 * (12), then the zlib stream -- its two header bytes, five per stored block, the
 * filter byte and pixels of every row, and four for Adler. */
#define MOD_PNG_MAX(w, h) \
    (63 + ((size_t)(w) * 3 + 1) * (size_t)(h) + \
     ((((size_t)(w) * 3 + 1) * (size_t)(h)) / 65535 + 1) * 5)

/* Encode `w`x`h` packed RGB -- three bytes per pixel, row-major, no padding -- as
 * a PNG into `out`. Returns the bytes written, or -1 when the arguments do not
 * describe an image or `cap` cannot hold the result. */
int64_t mod_png_rgb(const uint8_t *rgb, int32_t w, int32_t h, uint8_t *out,
                    size_t cap);

#endif /* EP122_MOD_USBSAVE_PNG_H */
