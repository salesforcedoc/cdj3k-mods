// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/probe.h - what a track's own header says about it.
 *
 * USB Save needs three things before it can write a faithful file, and all
 * three are in the source file's first few kilobytes:
 *
 *   rate / channels / bits   so the decode is asked for the file's OWN rate and
 *                            no resampling happens, and so FLAC's sample depth
 *                            is the file's rather than a guess;
 *   the tags                 which the request was for -- "metadata from the
 *                            source file's tags".
 *
 * Read here rather than asked of the deck's reader, because the reader's
 * metadata is not something this codebase models (nothing resolves
 * AudioFormatReader::metadataValues, and adding it would be an offset discovery
 * against a binary none of us can see from a test). A header parser is dull,
 * self-contained and provable on a host, which is the trade this feature wants.
 *
 * Takes BYTES, not a path: it is pure, so the file handling stays in
 * track_save.c with the rest of the syscalls.
 */
#ifndef EP122_MOD_USBSAVE_PROBE_H
#define EP122_MOD_USBSAVE_PROBE_H

#include <stddef.h>
#include <stdint.h>

#define MOD_PROBE_TEXT 128

struct mod_probe {
    /* 0 when the format did not say. A caller must treat 0 as "unknown" and
     * fall back, never as a value. */
    int  rate;
    int  channels;
    int  bits;
    /* Empty when absent. Nothing is invented to fill these. */
    char title[MOD_PROBE_TEXT];
    char artist[MOD_PROBE_TEXT];
    char album[MOD_PROBE_TEXT];
};

/* Inspect the head of a file. Returns 0 when the container was recognised,
 * which is NOT the same as every field being filled: a `rate` of 0 means the
 * format did not say, and a caller must fall back rather than use it. Returns
 * non-zero when the container is not one this understands, with `out` left
 * zeroed and safe to use as defaults.
 *
 * `buf` need not be the whole file; a few kilobytes is enough for every format
 * here, and a caller that only has the head should pass what it has. */
int mod_probe_header(const uint8_t *buf, size_t len, struct mod_probe *out);

#endif /* EP122_MOD_USBSAVE_PROBE_H */
