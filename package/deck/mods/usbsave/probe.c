// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/probe.c - the header reader behind usbsave/probe.h.
 *
 * Three containers, which between them cover what a rekordbox stick holds:
 * FLAC (STREAMINFO + Vorbis comments), MP3 (ID3v2 + the first MPEG frame) and
 * WAV (RIFF/fmt). Anything else returns non-zero and the caller falls back to
 * defaults plus a resample.
 *
 * Every field is bounds-checked against `len` before it is read, because the
 * buffer is the head of a file that may be truncated, and a header parser that
 * trusts its input is a crash inside EP122 rather than a wrong tag.
 */
#include "usbsave/probe.h"

#include <string.h>   /* memcpy, memset */

/* ---- small helpers -------------------------------------------------------- */

static int le32(const uint8_t *p)
{
    return (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static int le16(const uint8_t *p)
{
    return (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static void copy_text(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t i;

    if (cap == 0)
        return;
    if (n > cap - 1)
        n = cap - 1;
    for (i = 0; i < n; i++) {
        /* A tag with a newline or a NUL in it would otherwise truncate the
         * line it is written into. */
        dst[i] = (src[i] == '\n' || src[i] == '\r' || src[i] == 0) ? ' ' : (char)src[i];
    }
    dst[n] = '\0';
}

/* Match an upper-case key at the start of `p`, and return the value after the
 * '='. Used for both Vorbis comments and ID3 frames, which differ only in case
 * and in how the text is encoded. */
static int key_is(const uint8_t *p, size_t n, const char *key)
{
    size_t k = 0;

    while (key[k]) {
        if (k >= n)
            return 0;
        if ((char)p[k] != key[k])
            return 0;
        k++;
    }
    return n > k && p[k] == '=';
}

/* One "KEY=value" item, from either a Vorbis comment or an ID3 text frame. The
 * value starts after the '='; copying the whole item would put "TITLE=" into
 * the title. */
static void store_tag(struct mod_probe *out, const uint8_t *text, size_t n)
{
    size_t eq = 0;

    while (eq < n && text[eq] != '=')
        eq++;
    if (eq + 1 >= n)
        return;

    if (key_is(text, n, "TITLE"))
        copy_text(out->title, sizeof(out->title), text + eq + 1, n - eq - 1);
    else if (key_is(text, n, "ARTIST"))
        copy_text(out->artist, sizeof(out->artist), text + eq + 1, n - eq - 1);
    else if (key_is(text, n, "ALBUM"))
        copy_text(out->album, sizeof(out->album), text + eq + 1, n - eq - 1);
}

/* ---- FLAC ----------------------------------------------------------------- */

static int probe_flac(const uint8_t *buf, size_t len, struct mod_probe *out)
{
    size_t at = 4;   /* past "aC" of fLaC already checked by the caller */

    while (at + 4 <= len) {
        uint8_t header = buf[at];
        int last = (header & 0x80) != 0;
        int type = header & 0x7f;
        size_t blen = ((size_t)buf[at + 1] << 16) | ((size_t)buf[at + 2] << 8) |
                      (size_t)buf[at + 3];
        const uint8_t *body = buf + at + 4;

        if (at + 4 + blen > len)
            return -1;                        /* truncated head */

        if (type == 0 && blen >= 34) {
            /* sample rate(20) channels-1(3) bits-1(5) total(36) as one 64-bit
             * field at offset 10 of the block. */
            uint64_t packed = ((uint64_t)body[10] << 56) |
                              ((uint64_t)body[11] << 48) |
                              ((uint64_t)body[12] << 40) |
                              ((uint64_t)body[13] << 32) |
                              ((uint64_t)body[14] << 24) |
                              ((uint64_t)body[15] << 16) |
                              ((uint64_t)body[16] << 8) |
                              (uint64_t)body[17];

            out->rate     = (int)(packed >> 44);
            out->channels = (int)((packed >> 41) & 0x7) + 1;
            out->bits     = (int)((packed >> 36) & 0x1f) + 1;
        } else if (type == 4 && blen >= 8) {
            /* Vorbis comments: little-endian lengths, "KEY=value" in UTF-8. */
            size_t p = 0;
            size_t vendor = (size_t)le32(body);
            size_t count, i;

            if (vendor > blen)
                return -1;
            p = 4 + vendor;
            if (p + 4 > blen)
                return -1;
            count = (size_t)le32(body + p);
            p += 4;
            for (i = 0; i < count && p + 4 <= blen; i++) {
                size_t clen = (size_t)le32(body + p);

                p += 4;
                if (p + clen > blen)
                    break;
                if (clen >= 2)
                    store_tag(out, body + p, clen);
                p += clen;
            }
        }

        if (last)
            break;
        at += 4 + blen;
    }

    return out->rate ? 0 : -1;
}

/* ---- WAV ------------------------------------------------------------------ */

static int probe_wav(const uint8_t *buf, size_t len, struct mod_probe *out)
{
    size_t at = 12;   /* past "RIFF....WAVE" */

    while (at + 8 <= len) {
        size_t clen = (size_t)le32(buf + at + 4);
        const uint8_t *body = buf + at + 8;

        if (at + 8 + clen > len)
            return -1;
        if (memcmp(buf + at, "fmt ", 4) == 0 && clen >= 16) {
            out->channels = le16(body + 2);
            out->rate     = le32(body + 4);
            out->bits     = le16(body + 14);
            return out->rate ? 0 : -1;
        }
        if (memcmp(buf + at, "data", 4) == 0)
            break;
        at += 8 + clen + (clen & 1);          /* chunks are word-aligned */
    }
    return -1;
}

/* ---- MP3 ------------------------------------------------------------------ */

/* The MPEG audio header, so at least rate and channel mode are real rather than
 * assumed. Bit depth is not in there: an MP3 decodes to 16-bit and only 16-bit,
 * which is why the encoder is told 16 whatever the file says. */
static void probe_mpeg_frame(const uint8_t *buf, size_t len, size_t from,
                             struct mod_probe *out)
{
    static const int rates[4][3] = {
        { 11025, 12000, 8000 },     /* MPEG 2.5 */
        { 0, 0, 0 },                /* reserved */
        { 22050, 24000, 16000 },    /* MPEG 2 */
        { 44100, 48000, 32000 },    /* MPEG 1 */
    };
    size_t i;

    /* `from` is the start of the AUDIO, not of the file. An ID3 tag -- and the
     * album art inside it -- is full of 0xFF 0xEx byte pairs, so a scan from
     * zero finds a false sync a few dozen bytes in and reports the sample rate
     * of a JPEG. That is not a cosmetic error: the rate is what the decode is
     * asked for, so a wrong one resamples the whole track. */
    for (i = from; i + 4 <= len; i++) {
        uint8_t b0 = buf[i], b1 = buf[i + 1];
        int version, layer, rate_idx, bitrate_idx, channel_mode;
        uint8_t b2;

        if (b0 != 0xff || (b1 & 0xe0) != 0xe0)
            continue;
        version = (b1 >> 3) & 0x3;
        layer = (b1 >> 1) & 0x3;
        if (version == 1 || layer == 0)
            continue;                          /* reserved */
        b2 = buf[i + 2];
        rate_idx = (b2 >> 2) & 0x3;
        bitrate_idx = (b2 >> 4) & 0xf;
        if (rate_idx == 3 || bitrate_idx == 0 || bitrate_idx == 0xf)
            continue;                          /* reserved / free / bad */
        channel_mode = (buf[i + 3] >> 6) & 0x3;
        out->rate = rates[version][rate_idx];
        out->channels = (channel_mode == 3) ? 1 : 2;
        out->bits = 16;
        return;
    }
}

/* UTF-16 text to the byte strings the tag fields hold.
 *
 * This is the common case on a rekordbox stick, not an exotic one: every track
 * on the reference stick tags itself UTF-16 with a BOM, so a probe that only
 * reads the single-byte encodings finds NO title and NO artist on a stick full
 * of them. Surrogate pairs are written as '?' rather than reconstructed --
 * a tag is a label, and half a character is worse than a placeholder. */
static void copy_text_utf16(char *dst, size_t cap, const uint8_t *src, size_t n,
                            int big_endian)
{
    size_t i = 0, o = 0;

    if (cap == 0)
        return;
    if (n >= 2) {
        if (src[0] == 0xff && src[1] == 0xfe)
            big_endian = 0, i = 2;
        else if (src[0] == 0xfe && src[1] == 0xff)
            big_endian = 1, i = 2;
    }

    while (i + 1 < n && o + 4 < cap) {
        unsigned c = big_endian ? ((unsigned)src[i] << 8 | src[i + 1])
                                 : ((unsigned)src[i + 1] << 8 | src[i]);

        i += 2;
        if (c == 0)
            break;
        if (c < 0x20)
            c = ' ';                        /* a newline would break the line */
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            dst[o++] = (char)(0xc0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3f));
        } else if (c >= 0xd800 && c <= 0xdfff) {
            dst[o++] = '?';
        } else {
            dst[o++] = (char)(0xe0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            dst[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    dst[o] = '\0';
}

/* One ID3 text frame into the field it names, whatever encoding it uses. */
static void store_id3_text(struct mod_probe *out, const uint8_t *id,
                           const uint8_t *body, size_t fsize)
{
    char *dst;
    size_t cap;

    if (fsize < 2)
        return;
    if (memcmp(id, "TIT2", 4) == 0) {
        dst = out->title, cap = sizeof(out->title);
    } else if (memcmp(id, "TPE1", 4) == 0) {
        dst = out->artist, cap = sizeof(out->artist);
    } else if (memcmp(id, "TALB", 4) == 0) {
        dst = out->album, cap = sizeof(out->album);
    } else {
        return;
    }

    switch (body[0]) {
    case 0:                                 /* Latin-1 */
    case 3:                                 /* UTF-8 */
        copy_text(dst, cap, body + 1, fsize - 1);
        break;
    case 1:                                 /* UTF-16, BOM decides the order */
        copy_text_utf16(dst, cap, body + 1, fsize - 1, 1);
        break;
    case 2:                                 /* UTF-16BE, no BOM */
        copy_text_utf16(dst, cap, body + 1, fsize - 1, 1);
        break;
    default:
        break;                              /* UTF-16BE-with-BOM is not a thing */
    }
}

/* ID3v2 text frames. Returns where the tag ends, which is where the audio --
 * and so the first real MPEG frame -- begins. */
static size_t probe_id3v2(const uint8_t *buf, size_t len, struct mod_probe *out)
{
    size_t size, tag_end, at;

    if (len < 10)
        return 0;
    if (buf[5] == 0xff)
        return 0;                              /* unsynchronised */
    size = ((size_t)(buf[6] & 0x7f) << 21) | ((size_t)(buf[7] & 0x7f) << 14) |
           ((size_t)(buf[8] & 0x7f) << 7) | (size_t)(buf[9] & 0x7f);
    /* The length counts the tag BODY, starting after this 10-byte header -- so
     * the end is 10 + size, not size. Bounding frames against `size` directly
     * rejects every one of them. */
    if (size > len - 10)
        size = len - 10;
    tag_end = 10 + size;

    at = 10;
    while (at + 10 <= tag_end) {
        const uint8_t *id = buf + at;
        size_t fsize = ((size_t)buf[at + 4] << 24) | ((size_t)buf[at + 5] << 16) |
                       ((size_t)buf[at + 6] << 8) | (size_t)buf[at + 7];
        const uint8_t *body = buf + at + 10;

        if (id[0] == 0)
            break;                             /* padding */
        if (at + 10 + fsize > tag_end)
            break;
        if (fsize >= 1)
            store_id3_text(out, id, body, fsize);
        at += 10 + fsize;
    }
    return tag_end;
}

/* ---- entry ----------------------------------------------------------------- */

int mod_probe_header(const uint8_t *buf, size_t len, struct mod_probe *out)
{
    if (!buf || !out || len < 12)
        return -1;

    memset(out, 0, sizeof(*out));
    /* out->title/artist/album are arrays inside *out and memset already cleared
     * them; nothing else to do here. */

    if (memcmp(buf, "fLaC", 4) == 0)
        return probe_flac(buf, len, out);
    if (memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0)
        return probe_wav(buf, len, out);
    if (memcmp(buf, "ID3", 3) == 0) {
        size_t audio = probe_id3v2(buf, len, out);

        probe_mpeg_frame(buf, len, audio, out);
        /* Tags without a frame header still beat nothing. */
        return 0;
    }
    /* A bare MPEG stream, with no ID3 tag at all. */
    probe_mpeg_frame(buf, len, 0, out);
    return out->rate ? 0 : -1;
}
