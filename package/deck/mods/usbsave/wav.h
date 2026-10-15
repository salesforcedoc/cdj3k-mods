// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/wav.h - the WAV half of USB Save.
 *
 * The alternative to flac.c, and the one the deck's own reader is most certain
 * to accept, since there is nothing to decode: a 44-byte canonical header over
 * the same samples. It carries no tags -- a WAV has nowhere to put them that a
 * CDJ reads -- so a file saved this way is audio only.
 *
 * Same shape as the FLAC writer so track_save.c can pick between them without
 * knowing which it has, and pure for the same reason.
 */
#ifndef EP122_MOD_USBSAVE_WAV_H
#define EP122_MOD_USBSAVE_WAV_H

#include <stddef.h>
#include <stdint.h>

/* 4096 frames of stereo is the quantisation batch, and 2 channels at 3 bytes is
 * the widest sample this writes. */
#define MOD_WAV_BLOCK   4096
#define MOD_WAV_MAX_CH  2
#define MOD_WAV_BUF     (MOD_WAV_BLOCK * MOD_WAV_MAX_CH * 3)

/* What to write into the file's LIST/INFO chunk. Any field may be empty, and a
 * file with nothing to say gets no INFO chunk at all rather than an empty one.
 *
 * The lengths are supplied rather than measured here for the same reason
 * flac.c's are: this module is in the pure set, where a length loop is folded
 * into a call to strlen, and strlen is not in the allowed libc subset. */
struct mod_wav_info {
    const char *title;
    size_t      title_len;
    const char *artist;
    size_t      artist_len;
    const char *album;
    size_t      album_len;
};

/* The canonical PCM header, and the ceiling on a header that carries tags.
 * Named rather than written as a literal at the two places that need it,
 * because one of those is inside the writer where the buffer is a pointer --
 * and `sizeof(pointer)` is not the header length. */
#define MOD_WAV_HEADER  44
#define MOD_WAV_HEADER_MAX 1024

typedef int (*mod_wav_sink_fn)(void *user, const void *buf, size_t len);

struct mod_wav {
    mod_wav_sink_fn sink;
    void           *user;
    int             channels;
    int             bits;      /* 16 or 24 */
    int             rate;
    int64_t         total;
    int64_t         written;
    int             failed;
    uint8_t         buf[MOD_WAV_BUF];
};

/* Write the RIFF header. `total_samples` sizes the data chunk, so it must be
 * known up front exactly as it must for STREAMINFO. `info` may be NULL. */
int mod_wav_begin(struct mod_wav *w, mod_wav_sink_fn sink, void *user,
                  int channels, int bits, int rate, int64_t total_samples,
                  const struct mod_wav_info *info);

/* Convert and append. Samples are clipped, never wrapped, as in flac.c. */
int mod_wav_write(struct mod_wav *w, const float *interleaved, int64_t frames);

/* Nothing is left to write -- the header was complete at the start -- but this
 * checks the sample count the header promised actually arrived. */
int mod_wav_finish(struct mod_wav *w);

#endif /* EP122_MOD_USBSAVE_WAV_H */
