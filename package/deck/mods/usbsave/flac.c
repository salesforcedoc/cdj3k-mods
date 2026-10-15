// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/flac.c - the verbatim FLAC writer. See usbsave/flac.h for why it is
 * verbatim and not compressed.
 *
 * Layout produced, in the order it is written:
 *
 *     "fLaC"
 *     STREAMINFO            block type 0, 34 bytes
 *     VORBIS_COMMENT        block type 4, the tags
 *     one frame per MOD_FLAC_BLOCK frames of audio
 *
 * A frame is a header, one verbatim subframe per channel, and a footer:
 *
 *     sync 0x3ffe | reserved | blocking | block size | sample rate
 *     channels | sample size | reserved | frame number | [block size - 1]
 *     | CRC-8
 *     ...subframes...
 *     CRC-16 over everything above
 *
 * Three deliberate choices, each of which removes a branch:
 *
 *   - the block size code is always 0b0111, "the real size follows as 16 bits",
 *     which fits the last, short frame of a track without a special case;
 *   - the sample rate and sample size codes are both 0, "as STREAMINFO says",
 *     so the header carries no second copy of either;
 *   - the frame number is coded fixed-block, so it counts frames rather than
 *     samples and never needs the sample position.
 */
#include "usbsave/flac.h"

#include <string.h>   /* memcpy, for staging a block */

/* ---- checksums ------------------------------------------------------------ */

/* CRC-8, polynomial x^8 + x^2 + x + 1, over the frame header only. */
static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

/* CRC-16, polynomial x^16 + x^15 + x^2 + 1, over the whole frame. */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

/* ---- bit writer ----------------------------------------------------------- */

struct bitw {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    uint64_t acc;
    unsigned n;      /* pending bits, held in the low end of acc */
    int      over;   /* set when a write ran past cap; the frame is abandoned */
};

/* Append the low `n` bits of `v`, most significant first. */
static void bw_put(struct bitw *w, uint32_t v, unsigned n)
{
    if (w->over || n == 0)
        return;
    if (n > 32)
        n = 32;
    w->acc = (w->acc << n) | (uint64_t)(v & (uint32_t)((n == 32) ? 0xffffffffu
                                                               : ((1u << n) - 1)));
    w->n += n;
    while (w->n >= 8) {
        w->n -= 8;
        if (w->len >= w->cap) {
            w->over = 1;
            return;
        }
        w->buf[w->len++] = (uint8_t)(w->acc >> w->n);
    }
    w->acc &= (w->n ? ((1u << w->n) - 1) : 0);
}

/* Zero-pad to the next byte boundary, which is where a checksum can be
 * appended behind the bit writer's back. */
static void bw_align(struct bitw *w)
{
    if (w->over || w->n == 0) {
        w->n = 0;
        w->acc = 0;
        return;
    }
    if (w->len < w->cap)
        w->buf[w->len++] = (uint8_t)(w->acc << (8 - w->n));
    else
        w->over = 1;
    w->n = 0;
    w->acc = 0;
}

/* ---- frame number --------------------------------------------------------- */

/* FLAC codes the frame number the way UTF-8 codes a code point, extended to
 * seven bytes: a lead byte whose high bits count the total, then continuation
 * bytes carrying six bits each. The 7-byte form leads with 0xfe -- there is no
 * 0xff form -- so it carries 36 bits and a 32-bit frame number leaves its top
 * four clear. */
static void bw_frame_number(struct bitw *w, uint32_t v)
{
    unsigned n, i, lead_bits;

    if (v < 0x80u) {
        bw_put(w, v, 8);
        return;
    }
    if (v < 0x800u)
        n = 2;
    else if (v < 0x10000u)
        n = 3;
    else if (v < 0x200000u)
        n = 4;
    else if (v < 0x4000000u)
        n = 5;
    else if (v < 0x80000000u)
        n = 6;
    else
        n = 7;

    lead_bits = 7 - n;
    bw_put(w, ((0xffu << (8 - n)) & 0xffu) |
              (lead_bits ? (v >> (6 * (n - 1))) : 0u), 8);
    for (i = 0; i < n - 1; i++)
        bw_put(w, 0x80u | ((v >> (6 * (n - 2 - i))) & 0x3fu), 8);
}

/* ---- public API ----------------------------------------------------------- */

static int emit(struct mod_flac *f, const void *buf, size_t len)
{
    if (f->failed || !f->sink)
        return -1;
    if (f->sink(f->user, buf, len) != 0) {
        f->failed = 1;
        return -1;
    }
    return 0;
}

static void put_be(uint8_t *p, uint32_t v, int bytes)
{
    int i;

    for (i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
}

/* Vorbis comments are the one little-endian thing in the file: STREAMINFO, the
 * metadata block headers and every bit of a frame are big-endian (or MSB-first
 * within a byte), but this block's lengths and count are written from the low
 * byte up. Getting this axis wrong is silent -- a decoder reads a length in the
 * wrong byte order and either stops early or runs off the buffer. */
static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    int i;

    for (i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

int mod_flac_begin(struct mod_flac *f, mod_flac_sink_fn sink, void *user,
                   int channels, int bits, int rate, int64_t total_samples,
                   const uint8_t md5[16], const struct mod_flac_tags *tags)
{
    uint8_t *block;
    size_t at;
    int i;

    if (!f || !sink || channels < 1 || channels > MOD_FLAC_MAX_CH ||
        (bits != 16 && bits != 24) || rate <= 0 || total_samples < 0)
        return -1;

    f->sink     = sink;
    f->user     = user;
    f->channels = channels;
    f->bits     = bits;
    f->rate     = rate;
    f->total    = total_samples;
    f->written  = 0;
    f->frame_no = 0;
    f->failed   = 0;
    f->pending  = 0;

    if (emit(f, "fLaC", 4) != 0)
        return -1;

    /* Scratch for the metadata blocks, borrowed from the frame buffer: no
     * samples have been written yet, and a local array this size would be the
     * one thing in this file to pull __stack_chk_fail into the pure set. */
    block = f->frame;

    /* ---- STREAMINFO ---- */
    put_be(block, (uint32_t)MOD_FLAC_BLOCK, 2);          /* min block */
    put_be(block + 2, (uint32_t)MOD_FLAC_BLOCK, 2);      /* max block */
    put_be(block + 4, 0, 3);                             /* min frame: unknown */
    put_be(block + 7, 0, 3);                             /* max frame: unknown */
    {
        uint64_t packed = ((uint64_t)rate << 44) |
                          ((uint64_t)(channels - 1) << 41) |
                          ((uint64_t)(bits - 1) << 36) |
                          (uint64_t)total_samples;

        put_be(block + 10, (uint32_t)(packed >> 32), 4);
        put_be(block + 14, (uint32_t)packed, 4);
    }
    for (i = 0; i < 16; i++)
        block[18 + i] = md5 ? md5[i] : 0;

    {
        uint8_t hdr[4];

        hdr[0] = 0x00;                       /* STREAMINFO, not last yet */
        put_be(hdr + 1, 34, 3);
        if (emit(f, hdr, 4) != 0 || emit(f, block, 34) != 0)
            return -1;
    }

    /* ---- VORBIS_COMMENT ---- */
    {
        static const char vendor[] = "cdj3k-emu usb-save";
        const char *vname = (tags && tags->vendor) ? tags->vendor : vendor;
        size_t      vlen  = (tags && tags->vendor) ? tags->vendor_len
                                                   : sizeof(vendor) - 1;
        uint32_t    count = 0;
        size_t      k;

        at = 4 + vlen + 4;                  /* vendor length + vendor + count */
        if (at > MOD_FLAC_FRAME_MAX)
            return -1;

        /* Count first: an empty value is omitted from the wire, so the count is
         * not the number of items offered. */
        for (k = 0; tags && k < tags->count; k++)
            if (tags->items[k].value_len)
                count++;

        for (k = 0; tags && k < tags->count; k++) {
            const struct mod_flac_tag *t = &tags->items[k];
            size_t need = 4 + t->name_len + 1 + t->value_len;

            if (!t->value_len)
                continue;
            if (at + need > MOD_FLAC_FRAME_MAX)
                return -1;
            put_le(block + at, (uint32_t)(t->name_len + 1 + t->value_len), 4);
            at += 4;
            if (t->name_len) {
                memcpy(block + at, t->name, t->name_len);
                at += t->name_len;
            }
            block[at++] = '=';
            memcpy(block + at, t->value, t->value_len);
            at += t->value_len;
        }

        /* The vendor sits at a fixed 4-byte offset, so the body was laid out
         * after it; fill it in once the total length is known. */
        put_le(block, (uint32_t)vlen, 4);
        memcpy(block + 4, vname, vlen);
        put_le(block + 4 + vlen, count, 4);

        {
            uint8_t hdr[4];

            hdr[0] = 0x84;                  /* VORBIS_COMMENT, last block */
            put_be(hdr + 1, (uint32_t)at, 3);
            if (emit(f, hdr, 4) != 0 || emit(f, block, at) != 0)
                return -1;
        }
    }

    return 0;
}

/* Round half away from zero, then clamp into the declared depth. Clamping and
 * not wrapping is the point: a decoder that hands back a sample a shade past
 * full scale must not become full-scale noise. */
static int32_t quantise(float v, int bits)
{
    double scaled = (double)v * (double)(1u << (bits - 1));
    int64_t s = (int64_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
    int64_t lo = -((int64_t)1 << (bits - 1));
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1;

    if (s < lo)
        s = lo;
    if (s > hi)
        s = hi;
    return (int32_t)s;
}

/* Encode one frame of at most MOD_FLAC_BLOCK frames, read from `in` with
 * `stride` floats between successive frames of the same channel. */
static int encode_frame(struct mod_flac *f, const float *in, int64_t frames,
                        int64_t stride)
{
    struct bitw w;
    size_t header_len;
    int ch;
    int64_t i;

    w.buf = f->frame;
    w.cap = sizeof(f->frame);
    w.len = 0;
    w.acc = 0;
    w.n = 0;
    w.over = 0;

    /* Header. 0x3ffe sync, fixed blocking, block size = 16-bit literal,
     * sample rate and sample size from STREAMINFO, channels - 1. */
    bw_put(&w, 0x3ffeu, 14);
    bw_put(&w, 0, 1);
    bw_put(&w, 0, 1);
    bw_put(&w, 0x7u, 4);
    bw_put(&w, 0x0u, 4);
    bw_put(&w, (uint32_t)(f->channels - 1), 4);
    bw_put(&w, 0x0u, 3);
    bw_put(&w, 0, 1);
    bw_frame_number(&w, f->frame_no);
    bw_put(&w, (uint32_t)(frames - 1), 16);
    bw_align(&w);
    if (w.over)
        return -1;
    header_len = w.len;
    if (w.len >= w.cap)
        return -1;
    f->frame[w.len++] = crc8(f->frame, header_len);
    header_len = w.len;

    /* Subframes: verbatim, one per channel. */
    {
        struct bitw s;

        s.buf = f->frame + header_len;
        s.cap = sizeof(f->frame) - header_len;
        s.len = 0;
        s.acc = 0;
        s.n = 0;
        s.over = 0;

        for (ch = 0; ch < f->channels; ch++) {
            bw_put(&s, 0, 1);           /* zero bit */
            bw_put(&s, 0x1u, 6);        /* verbatim */
            bw_put(&s, 0, 1);          /* no wasted bits */
            for (i = 0; i < frames; i++) {
                int32_t q = quantise(in[i * stride + ch], f->bits);
                uint32_t raw = (uint32_t)q & (uint32_t)((f->bits == 32)
                                                        ? 0xffffffffu
                                                        : ((1u << f->bits) - 1));
                bw_put(&s, raw, (unsigned)f->bits);
            }
        }
        bw_align(&s);
        if (s.over)
            return -1;
        w.len = header_len + s.len;
    }

    {
        uint16_t crc = crc16(f->frame, w.len);

        if (w.len + 2 > w.cap)
            return -1;
        f->frame[w.len++] = (uint8_t)(crc >> 8);
        f->frame[w.len++] = (uint8_t)crc;
    }

    f->frame_no++;
    return emit(f, f->frame, w.len);
}

int mod_flac_write(struct mod_flac *f, const float *interleaved, int64_t frames)
{
    int64_t done = 0;

    if (!f || f->failed || frames < 0 || (!interleaved && frames > 0))
        return -1;

    while (done < frames) {
        int64_t room = (int64_t)MOD_FLAC_BLOCK - f->pending;
        int64_t n = frames - done;

        if (n > room)
            n = room;

        memcpy(&f->stage[f->pending * f->channels],
               interleaved + (done * f->channels),
               (size_t)(n * f->channels) * sizeof(float));
        f->pending += n;
        done += n;

        if (f->pending == MOD_FLAC_BLOCK) {
            if (encode_frame(f, f->stage, MOD_FLAC_BLOCK, f->channels) != 0)
                return -1;
            f->pending = 0;
        }
    }

    f->written += frames;
    return 0;
}

int mod_flac_finish(struct mod_flac *f)
{
    if (!f || f->failed)
        return -1;
    if (f->written != f->total) {
        /* STREAMINFO already promised a sample count. A stream whose frames do
         * not add up to it is a file that will not seek, so say so and let the
         * caller delete it rather than leave it on the stick. */
        f->failed = 1;
        return -1;
    }
    /* The one frame allowed to be short is the track's last. */
    if (f->pending > 0) {
        if (encode_frame(f, f->stage, f->pending, f->channels) != 0)
            return -1;
        f->pending = 0;
    }
    f->sink = NULL;
    return 0;
}
