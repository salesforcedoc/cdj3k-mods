// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * tests/test_png.c - the PNG writer in usbsave/png.c.
 *
 * The reader below shares nothing with the writer: its own CRC-32, its own
 * Adler-32, and a chunk walk that only knows that a chunk is a length, a type, the
 * data and a CRC. A test that used the writer's checksums would pass just as
 * happily with the wrong polynomial -- the reason test_flac.c carries its own.
 *
 * What it checks is that a real reader would accept the file: every chunk's CRC
 * verified from scratch, the zlib stream's stored blocks reassembled, the Adler
 * checked, and the pixels that come back out identical to the ones that went in.
 * That last part is also what catches a channel swap, which is the failure a
 * signature-only test would miss entirely.
 */
#include "test.h"

#include <string.h>

#include "usbsave/png.h"

/* ---- an independent PNG reader ---------------------------------------------- */

static uint32_t t_crc(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static uint32_t g32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t adler(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

struct png {
    uint32_t w, h;
    size_t   pixels_at;      /* where the decoded RAW stream starts, in g_raw */
    size_t   raw_len;
};

static uint8_t g_raw[400000];      /* the decoded zlib stream */
static uint8_t g_img[400000];      /* its pixels, filter bytes stripped */

/* Walk the whole file, verifying every CRC. Decodes the IDAT stored blocks and
 * leaves the raw (filtered) stream in g_raw. Returns 0 on success. */
static int read_png(const uint8_t *b, size_t n, struct png *out)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    size_t at = 8, raw = 0;
    int seen_ihdr = 0, seen_idat = 0, seen_iend = 0;

    if (n < 8 || memcmp(b, sig, 8) != 0)
        return -1;
    while (at + 12 <= n) {
        uint32_t len = g32(b + at);
        const uint8_t *type = b + at + 4;
        const uint8_t *data = b + at + 8;
        uint32_t want;

        if (at + 12 + len > n)
            return -1;
        want = t_crc(0xffffffffu, type, 4);
        want = t_crc(want, data, len) ^ 0xffffffffu;
        if (want != g32(data + len))
            return -1;                          /* a chunk checksum is wrong */

        if (memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) return -1;
            out->w = g32(data);
            out->h = g32(data + 4);
            if (data[8] != 8 || data[9] != 2 || data[10] != 0 ||
                data[11] != 0 || data[12] != 0)
                return -1;                      /* 8-bit truecolour, no interlace */
            seen_ihdr = 1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            size_t p = 0;

            if (!seen_ihdr || len < 6 || data[0] != 0x78)
                return -1;
            p = 2;                              /* zlib header */
            for (;;) {
                uint32_t blen, nlen;

                if (p + 5 > len) return -1;
                if ((data[p] & 6) != 0) return -1;      /* only stored blocks */
                blen = (uint32_t)data[p + 1] | ((uint32_t)data[p + 2] << 8);
                nlen = (uint32_t)data[p + 3] | ((uint32_t)data[p + 4] << 8);
                if ((blen ^ nlen) != 0xffffu) return -1;
                p += 5;
                if (p + blen > len) return -1;
                memcpy(g_raw + raw, data + p, blen);
                raw += blen;
                p += blen;
                if (data[p - blen - 5] & 1)
                    break;                      /* BFINAL */
            }
            if (p + 4 != len) return -1;
            {
                uint32_t got = g32(data + p);

                if (got != adler(g_raw, raw)) return -1;
            }
            seen_idat = 1;
        } else if (memcmp(type, "IEND", 4) == 0) {
            seen_iend = 1;
        }
        at += 12 + len;
    }
    if (!seen_idat || !seen_iend)
        return -1;
    out->raw_len = raw;
    return 0;
}

/* Strip the per-row filter byte into g_img; returns the pixel bytes. */
static size_t unfilter(const struct png *p)
{
    size_t rowbytes = (size_t)p->w * 3 + 1, y, got = 0;

    if (p->raw_len != rowbytes * p->h)
        return 0;
    for (y = 0; y < p->h; y++) {
        if (g_raw[y * rowbytes] != 0)           /* filter 0 = none */
            return 0;
        memcpy(g_img + got, g_raw + y * rowbytes + 1, rowbytes - 1);
        got += rowbytes - 1;
    }
    return got;
}

/* ---- the tests -------------------------------------------------------------- */

/* Distinct, recognisable per-channel values: a swapped channel cannot survive the
 * round trip. */
static void fill_image(uint8_t *rgb, int32_t w, int32_t h)
{
    int32_t x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;

            p[0] = (uint8_t)(x * 7 + 1);        /* red   */
            p[1] = (uint8_t)(y * 11 + 2);       /* green */
            p[2] = (uint8_t)(x * y + 3);        /* blue  */
        }
    }
}

static void test_round_trip(void)
{
    static uint8_t rgb[300 * 3 * 200], out[MOD_PNG_MAX(300, 200)];
    struct png p;
    int64_t n;
    size_t got;

    T_CASE("png: a small cover round-trips through an independent reader");
    fill_image(rgb, 300, 200);
    n = mod_png_rgb(rgb, 300, 200, out, sizeof(out));
    CHECK(n > 0);
    CHECK_INT(read_png(out, (size_t)n, &p), 0);
    CHECK_U32(p.w, 300);
    CHECK_U32(p.h, 200);
    got = unfilter(&p);
    CHECK_INT((int)got, 300 * 3 * 200);
    /* The pixels, byte for byte: this is what a channel swap would break. */
    CHECK(memcmp(g_img, rgb, got) == 0);
}

static void test_spans_several_stored_blocks(void)
{
    /* A row wider than the 65535 a stored block can hold, so the writer has to
     * split a block mid-row and the reader has to reassemble it. */
    static uint8_t rgb[23000 * 3 * 2], out[MOD_PNG_MAX(23000, 2)];
    struct png p;
    int64_t n;
    size_t got;

    T_CASE("png: a row wider than one stored block");
    fill_image(rgb, 23000, 2);
    n = mod_png_rgb(rgb, 23000, 2, out, sizeof(out));
    CHECK(n > 0);
    CHECK_INT(read_png(out, (size_t)n, &p), 0);
    CHECK_U32(p.w, 23000);
    CHECK_U32(p.h, 2);
    got = unfilter(&p);
    CHECK_INT((int)got, 23000 * 3 * 2);
    CHECK(memcmp(g_img, rgb, got) == 0);
}

static void test_refuses_what_it_cannot_hold(void)
{
    static uint8_t rgb[4 * 3 * 4], out[MOD_PNG_MAX(4, 4)];
    int64_t n;

    T_CASE("png: refuses rather than truncates");
    fill_image(rgb, 4, 4);
    n = mod_png_rgb(rgb, 4, 4, out, sizeof(out));
    CHECK(n > 0);
    /* One byte short of what the image needs. */
    CHECK_INT((int)mod_png_rgb(rgb, 4, 4, out, (size_t)n - 1), -1);
    /* Nothing to write. */
    CHECK_INT((int)mod_png_rgb(rgb, 0, 4, out, sizeof(out)), -1);
    CHECK_INT((int)mod_png_rgb(rgb, 4, -1, out, sizeof(out)), -1);
    CHECK_INT((int)mod_png_rgb(NULL, 4, 4, out, sizeof(out)), -1);
}

int main(void)
{
    test_round_trip();
    test_spans_several_stored_blocks();
    test_refuses_what_it_cannot_hold();
    return t_done("png");
}
