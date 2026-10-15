// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/flac.h - a FLAC writer, for USB Save.
 *
 * The deck READS FLAC (audio_format::FileReadFlac) but cannot write it: it has
 * no encoder, and the only FLAC in the system arrives from a remote stemd
 * server as a finished file. So USB Save has to carry its own.
 *
 * This one is deliberately the smallest thing that is still a real FLAC
 * stream: VERBATIM subframes only, no fixed or LPC prediction and no Rice
 * coding. That is a legal encoding -- the format's subframe types exist so a
 * simple encoder is possible -- and it is lossless, because every sample is
 * stored exactly. What it does not do is compress, so a file comes out close
 * to the size of the WAV it would otherwise have been. Trading size for a few
 * hundred lines that can be proved correct against the deck's own reader is
 * the right trade here; an LPC encoder would be a project.
 *
 * Pure: it writes through a sink and names nothing outside the mods' libc
 * subset. File handling, naming and the stick live in track_save.c.
 */
#ifndef EP122_MOD_USBSAVE_FLAC_H
#define EP122_MOD_USBSAVE_FLAC_H

#include <stddef.h>
#include <stdint.h>

/* One frame's worth. 4096 keeps a frame far under the 65535 the format allows
 * while staying large enough that per-frame overhead disappears. */
#define MOD_FLAC_BLOCK   4096
#define MOD_FLAC_MAX_CH  2

/* Worst case in bytes for one frame: header + two verbatim subframes at 32
 * bits/sample + CRCs. Used to size the frame buffer, so it must be an upper
 * bound and not a good guess. */
#define MOD_FLAC_FRAME_MAX \
    ((MOD_FLAC_BLOCK * MOD_FLAC_MAX_CH * 4) + 64)

/* Bytes out. Return 0 to continue, non-zero to abort the encode -- which is
 * how a pulled stick stops a save without unwinding anything. */
typedef int (*mod_flac_sink_fn)(void *user, const void *buf, size_t len);

/* One tag, with both lengths supplied by the caller.
 *
 * The lengths are not a convenience: this module is compiled into the pure set,
 * where the only libc allowed is a handful of mem* and libm calls. A length loop
 * written here is folded by the optimiser into a call to strlen, which is NOT in
 * that set, so the purity gate fails on a build nobody runs while developing.
 * The caller (track_save.c, which is impure and may use libc freely) measures
 * them instead. */
struct mod_flac_tag {
    const char *name;
    size_t      name_len;
    const char *value;
    size_t      value_len;
};

struct mod_flac_tags {
    /* NULL selects the built-in vendor string. */
    const char                *vendor;
    size_t                     vendor_len;
    const struct mod_flac_tag *items;
    size_t                     count;
};

/* Total samples is needed in STREAMINFO, which is the FIRST block, so it has
 * to be known before any audio is written: stem_decode_pull() with a NULL sink
 * returns it without decoding. `md5` may be NULL, which writes the all-zero
 * signature the format defines as "not computed" -- the alternative is a
 * second full decode purely to hash, and nothing reads the field. */
struct mod_flac {
    mod_flac_sink_fn sink;   /* NULL once finished or after a failure */
    void             *user;
    int               channels;
    int               bits;      /* 16 or 24 */
    int               rate;
    int64_t           total;     /* frames the caller promised */
    int64_t           written;   /* frames encoded so far */
    uint32_t          frame_no;
    int               failed;

    /* Frames accepted but not yet a full block. STREAMINFO declares a fixed
     * block size, so only the LAST frame of a track may be short: emitting one
     * per write() would make a stream that contradicts its own header the first
     * time a caller handed over an odd-sized chunk. */
    int64_t           pending;
    float             stage[MOD_FLAC_BLOCK * MOD_FLAC_MAX_CH];

    uint8_t           frame[MOD_FLAC_FRAME_MAX];
};

/* Write the marker, STREAMINFO and the tags. Returns 0 on success. */
int mod_flac_begin(struct mod_flac *f, mod_flac_sink_fn sink, void *user,
                   int channels, int bits, int rate, int64_t total_samples,
                   const uint8_t md5[16], const struct mod_flac_tags *tags);

/* Encode `frames` frames of interleaved f32 in [-1, 1]. Blocks may be any size;
 * they are split across FLAC frames internally. Samples are quantised to the
 * declared bit depth and CLIPPED rather than wrapped -- a decoder that produced
 * a sample slightly outside [-1, 1] must not turn into full-scale noise. */
int mod_flac_write(struct mod_flac *f, const float *interleaved, int64_t frames);

/* Pad the last frame and finish. Returns 0 only if every frame went out. */
int mod_flac_finish(struct mod_flac *f);

#endif /* EP122_MOD_USBSAVE_FLAC_H */
