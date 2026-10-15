// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/png.c - the writer described in png.h.
 *
 * One pass and no buffer of its own: a PNG's chunk CRC and its zlib Adler are both
 * computable as the bytes go past, so the image data is written straight into the
 * caller's buffer with the two checksums accumulated beside it.
 *
 * Two things are deliberate rather than incidental:
 *
 *   NO LOCAL ARRAYS. A local array of more than a few bytes makes the compiler
 *   emit __stack_chk_fail, which is not in the purity gate's allowed set and would
 *   fail the build of a module that has nothing to do with stack protection. Every
 *   multi-byte field is therefore written into the OUTPUT and folded into the CRC
 *   from there, one byte at a time.
 *
 *   THE BLOCK WALK IS BY INDEX. Stored blocks may split anywhere, including mid-row,
 *   so walking the raw stream by absolute index costs a division per byte and buys
 *   not having to carry a partially-emitted row across a block boundary.
 */
#include "usbsave/png.h"

#include <string.h>

/* ---- the two checksums ------------------------------------------------------ */

/* CRC-32, bitwise: eight shifts per byte instead of a 1 KB table. Over a 320x240
 * cover that is a few hundred thousand operations, and the table would be the
 * largest thing in the file. */
static uint32_t crc_byte(uint32_t crc, uint8_t b)
{
    int i;

    crc ^= b;
    for (i = 0; i < 8; i++)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    return crc;
}

static uint32_t crc_run(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--)
        crc = crc_byte(crc, *p++);
    return crc;
}

static uint32_t crc_be32(uint32_t crc, uint32_t v)
{
    crc = crc_byte(crc, (uint8_t)(v >> 24));
    crc = crc_byte(crc, (uint8_t)(v >> 16));
    crc = crc_byte(crc, (uint8_t)(v >> 8));
    return crc_byte(crc, (uint8_t)v);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Write one byte and fold it into the running chunk CRC. */
static size_t put_fold(uint8_t *out, size_t at, uint32_t *crc, uint8_t v)
{
    out[at] = v;
    *crc = crc_byte(*crc, v);
    return at + 1;
}

/* ---- the writer ------------------------------------------------------------- */

int64_t mod_png_rgb(const uint8_t *rgb, int32_t w, int32_t h, uint8_t *out,
                    size_t cap)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    size_t rowbytes, raw_total, nblocks, zlib_len, need, at, pos, k;
    uint32_t crc, a = 1, b = 0;

    if (!rgb || !out || w <= 0 || h <= 0)
        return -1;

    /* A row is a filter byte and then the pixels. Filter 0 is "none": the stored
     * blocks are not compressing anything, so there is nothing for a filter to
     * help with. */
    rowbytes = (size_t)w * 3 + 1;
    raw_total = rowbytes * (size_t)h;
    nblocks = (raw_total + 65534) / 65535;
    zlib_len = 2 + nblocks * 5 + raw_total + 4;
    need = 8 + 25 + (12 + zlib_len) + 12;
    if (need > cap)
        return -1;

    memcpy(out, sig, 8);
    at = 8;

    /* ---- IHDR ---- */
    put_be32(out + at, 13);
    memcpy(out + at + 4, "IHDR", 4);
    at += 8;
    crc = crc_run(0xffffffffu, (const uint8_t *)"IHDR", 4);
    /* Written AND folded: the CRC covers these bytes, so folding without writing
     * gives a checksum over a header that is not there. */
    put_be32(out + at, (uint32_t)w);
    put_be32(out + at + 4, (uint32_t)h);
    at += 8;
    crc = crc_be32(crc, (uint32_t)w);
    crc = crc_be32(crc, (uint32_t)h);
    at = put_fold(out, at, &crc, 8);        /* bit depth     */
    at = put_fold(out, at, &crc, 2);        /* truecolour    */
    at = put_fold(out, at, &crc, 0);        /* deflate       */
    at = put_fold(out, at, &crc, 0);        /* filter method */
    at = put_fold(out, at, &crc, 0);        /* not interlaced */
    put_be32(out + at, crc ^ 0xffffffffu);
    at += 4;

    /* ---- IDAT ---- */
    put_be32(out + at, (uint32_t)zlib_len);
    memcpy(out + at + 4, "IDAT", 4);
    at += 8;
    crc = crc_run(0xffffffffu, (const uint8_t *)"IDAT", 4);
    /* zlib header: deflate, 32K window, no dictionary, fastest. The check bits are
     * what make 0x78 0x01 the canonical pair. */
    at = put_fold(out, at, &crc, 0x78);
    at = put_fold(out, at, &crc, 0x01);

    pos = 0;
    while (pos < raw_total) {
        size_t blen = raw_total - pos > 65535 ? 65535 : raw_total - pos;
        uint8_t final = (pos + blen >= raw_total) ? 1 : 0;
        uint16_t nlen = (uint16_t)~blen;

        at = put_fold(out, at, &crc, final);            /* BFINAL, BTYPE=00 */
        at = put_fold(out, at, &crc, (uint8_t)blen);
        at = put_fold(out, at, &crc, (uint8_t)(blen >> 8));
        at = put_fold(out, at, &crc, (uint8_t)nlen);
        at = put_fold(out, at, &crc, (uint8_t)(nlen >> 8));

        for (k = 0; k < blen; k++) {
            size_t idx = pos + k;
            size_t col = idx % rowbytes;
            uint8_t v = (col == 0)
                            ? 0
                            : rgb[(idx / rowbytes) * (size_t)w * 3 + (col - 1)];

            at = put_fold(out, at, &crc, v);
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
        }
        pos += blen;
    }

    /* Adler-32 over exactly the bytes the blocks carried -- filter bytes included,
     * because that is the stream deflate was handed. */
    {
        uint32_t sum = (b << 16) | a;

        at = put_fold(out, at, &crc, (uint8_t)(sum >> 24));
        at = put_fold(out, at, &crc, (uint8_t)(sum >> 16));
        at = put_fold(out, at, &crc, (uint8_t)(sum >> 8));
        at = put_fold(out, at, &crc, (uint8_t)sum);
    }
    put_be32(out + at, crc ^ 0xffffffffu);
    at += 4;

    /* ---- IEND ---- */
    put_be32(out + at, 0);
    memcpy(out + at + 4, "IEND", 4);
    at += 8;
    crc = crc_run(0xffffffffu, (const uint8_t *)"IEND", 4);
    put_be32(out + at, crc ^ 0xffffffffu);
    at += 4;

    return (int64_t)at;
}
