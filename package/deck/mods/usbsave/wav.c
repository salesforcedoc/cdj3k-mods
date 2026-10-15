// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/wav.c - the WAV writer. See usbsave/wav.h.
 *
 * Canonical 44-byte PCM WAV: RIFF/WAVE, a 16-byte fmt chunk, then data. The
 * header is complete before any audio is written because the caller knows the
 * frame count, so there is no seek-back and no placeholder to patch -- which
 * matters, because the destination is a file on a stick that can be pulled.
 *
 * Everything here is little-endian. That is the opposite axis from FLAC's
 * metadata and the same as its Vorbis comments, which is exactly the kind of
 * thing worth saying out loud rather than remembering.
 */
#include "usbsave/wav.h"

#include <string.h>   /* memcpy */

static int emit(struct mod_wav *w, const void *buf, size_t len)
{
    if (w->failed || !w->sink)
        return -1;
    if (w->sink(w->user, buf, len) != 0) {
        w->failed = 1;
        return -1;
    }
    return 0;
}

static void put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* One INFO subchunk: id, size, the text and its terminating NUL, then a pad byte
 * if that came out odd. RIFF requires every chunk to be word-aligned, and one
 * missing pad byte desynchronises every chunk after it. */
static size_t info_put(uint8_t *hdr, size_t at, const char *id,
                       const char *value, size_t len)
{
    size_t i;

    if (!value || len == 0)
        return at;
    memcpy(hdr + at, id, 4); at += 4;
    put_le32(hdr + at, (uint32_t)(len + 1)); at += 4;   /* NUL counts */
    for (i = 0; i < len; i++)
        hdr[at++] = (uint8_t)value[i];
    hdr[at++] = 0;
    if ((len + 1) & 1)
        hdr[at++] = 0;
    return at;
}

/* Bytes a field costs on the wire, and the whole chunk's size -- 0 when there is
 * nothing to write, so a file with no tags gets no INFO chunk at all. */
static size_t info_entry(size_t len)
{
    size_t n = len + 1;

    return 8 + n + (n & 1);
}

static size_t info_total(const struct mod_wav_info *info)
{
    size_t n = 0;

    if (!info)
        return 0;
    if (info->title_len)
        n += info_entry(info->title_len);
    if (info->artist_len)
        n += info_entry(info->artist_len);
    if (info->album_len)
        n += info_entry(info->album_len);
    return n ? 4 + n : 0;          /* 4 = the "INFO" form tag */
}

int mod_wav_begin(struct mod_wav *w, mod_wav_sink_fn sink, void *user,
                  int channels, int bits, int rate, int64_t total_samples,
                  const struct mod_wav_info *info)
{
    /* Built into the struct's own buffer rather than a local: a 44-byte local
     * array is enough to make the compiler emit __stack_chk_fail, which is not
     * in the pure set, and no samples have been written yet so the buffer is
     * free. */
    uint8_t *hdr;
    uint32_t data_bytes;
    size_t list_bytes, at;

    if (!w || !sink || channels < 1 || channels > MOD_WAV_MAX_CH ||
        (bits != 16 && bits != 24) || rate <= 0 || total_samples < 0)
        return -1;

    w->sink     = sink;
    w->user     = user;
    w->channels = channels;
    w->bits     = bits;
    w->rate     = rate;
    w->total    = total_samples;
    w->written  = 0;
    w->failed   = 0;

    /* A file this size has no business being a plain WAV; RF64 would be the
     * answer and this is not it. Refusing is better than writing a header whose
     * length field wraps. */
    if (total_samples > (int64_t)0xffffffffu / (channels * (bits / 8)))
        return -1;
    data_bytes = (uint32_t)(total_samples * channels * (bits / 8));

    hdr = w->buf;

    /* The LIST/INFO chunk is what lets a WAV carry the same tags FLAC does. It
     * sits between fmt and data, so both sizes below depend on whether there is
     * one -- which is why the whole header is laid out here rather than written
     * as a fixed 44 bytes. */
    list_bytes = info_total(info);
    if (MOD_WAV_HEADER + list_bytes > MOD_WAV_HEADER_MAX ||
        MOD_WAV_HEADER + list_bytes > sizeof(w->buf))
        return -1;

    at = 0;
    memcpy(hdr + at, "RIFF", 4); at += 4;
    at += 4;                                   /* RIFF size, patched at the end */
    memcpy(hdr + at, "WAVE", 4); at += 4;

    memcpy(hdr + at, "fmt ", 4); at += 4;
    put_le32(hdr + at, 16); at += 4;        /* PCM fmt chunk size */
    put_le16(hdr + at, 1);  at += 2;        /* format: PCM */
    put_le16(hdr + at, (uint32_t)channels); at += 2;
    put_le32(hdr + at, (uint32_t)rate); at += 4;
    put_le32(hdr + at, (uint32_t)rate * (uint32_t)channels * (uint32_t)(bits / 8));
    at += 4;
    put_le16(hdr + at, (uint32_t)(channels * (bits / 8))); at += 2;
    put_le16(hdr + at, (uint32_t)bits); at += 2;

    if (list_bytes) {
        memcpy(hdr + at, "LIST", 4); at += 4;
        put_le32(hdr + at, (uint32_t)list_bytes); at += 4;
        memcpy(hdr + at, "INFO", 4); at += 4;
        at = info_put(hdr, at, "INAM", info->title, info->title_len);
        at = info_put(hdr, at, "IART", info->artist, info->artist_len);
        at = info_put(hdr, at, "IPRD", info->album, info->album_len);
    }

    memcpy(hdr + at, "data", 4); at += 4;
    put_le32(hdr + at, data_bytes); at += 4;

    /* RIFF size counts everything after its own field. */
    put_le32(hdr + 4, (uint32_t)(at - 8 + data_bytes));

    return emit(w, hdr, at);
}

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

int mod_wav_write(struct mod_wav *w, const float *interleaved, int64_t frames)
{
    int64_t done = 0;

    if (!w || w->failed || frames < 0 || (!interleaved && frames > 0))
        return -1;

    while (done < frames) {
        int64_t n = frames - done;
        size_t at = 0;
        int64_t i;
        int ch;

        if (n > MOD_WAV_BLOCK)
            n = MOD_WAV_BLOCK;

        for (i = 0; i < n; i++) {
            for (ch = 0; ch < w->channels; ch++) {
                int32_t q = quantise(interleaved[(done + i) * w->channels + ch],
                                     w->bits);
                if (w->bits == 16) {
                    uint32_t u = (uint32_t)q & 0xffffu;
                    w->buf[at++] = (uint8_t)u;
                    w->buf[at++] = (uint8_t)(u >> 8);
                } else {
                    uint32_t u = (uint32_t)q & 0xffffffu;
                    w->buf[at++] = (uint8_t)u;
                    w->buf[at++] = (uint8_t)(u >> 8);
                    w->buf[at++] = (uint8_t)(u >> 16);
                }
            }
        }

        if (emit(w, w->buf, at) != 0)
            return -1;
        done += n;
    }

    w->written += frames;
    return 0;
}

int mod_wav_finish(struct mod_wav *w)
{
    if (!w || w->failed)
        return -1;
    if (w->written != w->total) {
        /* The header already stated the data length. A file that disagrees with
         * its own header is worse than a missing one: it plays and then stops
         * somewhere wrong. */
        w->failed = 1;
        return -1;
    }
    w->sink = NULL;
    return 0;
}
