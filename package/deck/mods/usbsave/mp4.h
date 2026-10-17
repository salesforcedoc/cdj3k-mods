// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/mp4.h - write iTunes-style tags into an MP4/M4A, for USB Save.
 *
 * WHY THIS EXISTS. USB Save copies a streamed track's file to the stick
 * verbatim, and a Beatport streaming cache usually carries no tags at all: the
 * saved files land as `29191181.m4a` with nothing inside saying what they are.
 * The deck knows -- it shows the title and artist -- so the metadata is put back
 * into the copy on the way through.
 *
 * The shape of the job is a splice, not an encode. The container is a chain of
 * boxes; the tags live in `moov > udta > meta > ilst`, and adding to them means
 * growing `moov`. Two things make that delicate, and both are the reason this is
 * a module rather than twenty lines in the caller:
 *
 *   - `moov` may sit BEFORE `mdat`, which is what a streamed file wants so a
 *     player can start before the whole thing has arrived. Growing `moov` then
 *     moves every audio sample, and every absolute offset in `stco`/`co64`
 *     (and in a fragmented file's `tfhd`/`trun`) has to move with it. Get that
 *     wrong and the file plays as silence, or as noise, from the first byte.
 *   - muxers leave a `free` box inside `udta` precisely so tags can be rewritten
 *     without any of that. When the slack is there, the whole problem is a memcpy
 *     and nothing moves.
 *
 * So the caller buffers `moov`, hands it here with the tags it has, and gets back
 * a replacement `moov` plus the number of bytes the container grew by. The
 * caller does the file work: it already streams the source through a sink.
 *
 * WHAT IT REFUSES. Anything it does not fully understand. A box whose size does
 * not add up, a `moov` without a readable `trak`, an atom that would not fit in
 * the capacity the caller offered: all return -1 having written nothing, and the
 * caller leaves the plain verbatim copy in place. A file that saves with no tags
 * is a disappointment; a file that saves with a corrupt index is a broken track.
 *
 * Pure: it reads and writes caller-owned memory and names nothing outside the
 * mods' libc subset, so lengths are passed in rather than measured here -- a
 * hand-written length loop folds into a call to strlen, which is not in that set.
 */
#ifndef EP122_MOD_USBSAVE_MP4_H
#define EP122_MOD_USBSAVE_MP4_H

#include <stddef.h>
#include <stdint.h>

/* The ilst `data` atom's type, which is what says how to read the payload. These
 * are the four this module writes; anything else in the file is left alone. */
#define MOD_MP4_TEXT   1    /* UTF-8                                  */
#define MOD_MP4_JPEG   13
#define MOD_MP4_PNG    14
#define MOD_MP4_INT16  21   /* big-endian signed, which is what tmpo is */

struct mod_mp4_tag {
    /* The atom id inside ilst: "\xA9nam" (title), "\xA9ART" (artist),
     * "\xA9alb" (album), "\xA9gen" (genre), "tmpo" (BPM), "covr" (artwork), or
     * "----" for a freeform field. Four bytes, never NUL-terminated -- 0xA9 is not
     * a valid character in a C string literal's ASCII, and the format treats these
     * as raw bytes for the same reason. */
    char        name[4];
    int         type;           /* one of the MOD_MP4_* above                 */
    const void *value;
    size_t      value_len;

    /* A FREEFORM field, which is where a value iTunes has no atom for lives --
     * a key, for instance. Its `name` above is the four dashes, and these two say
     * which freeform field it is: the "mean" (whose namespace) and the "name"
     * within it. Both NULL for an ordinary atom, which the id in `name` states in
     * full. Lengths are passed in rather than measured, for the same reason as
     * everywhere else here: strlen is not in the mods' libc subset. */
    const char *free_mean;
    size_t      free_mean_len;
    const char *free_name;
    size_t      free_name_len;
};

/* Worst-case bytes one tag needs in the new moov: the ilst atom, its `data`
 * child, the name, and the payload rounded up. A freeform tag needs its `mean`
 * and `name` children too -- 12 bytes and the string for each -- which this does
 * not count; size one as MOD_MP4_TAG_MAX(vlen + mean_len + name_len + 24). */
#define MOD_MP4_TAG_MAX(vlen) (8 + 16 + 8 + (vlen) + 8)

/* How many tags one splice may be handed. THE CALLER'S LIST AND THIS MODULE'S
 * CAPACITY ARE THE SAME NUMBER, in one place, because they were not.
 *
 * Measured 2026-10-16: track_save.c grew a ninth tag -- the artwork's URL, in the
 * comment field -- and this module still held eight, so every track complete enough
 * to produce all nine was refused ("add_tags refused: bad arguments at 9") and
 * copied VERBATIM: no tags, and no cover either, because the cover ships in the same
 * splice. The failure landed on exactly the best-populated tracks, and the log said
 * only "no tag rewrite" for months of rounds. A shared constant is the fix; a comment
 * is not, so the caller sizes its array with this too.
 *
 * 12 rather than 10, i.e. TWO MORE THAN THE CALLER BUILDS (ten: ©nam ©ART ©alb ©gen
 * ©day tmpo the freeform key ©cmt ©wrt covr). Sizing it to the exact count is what
 * broke: the tenth tag was added while this held eight, and a capacity with no
 * headroom turns the next field into the same silent refusal. */
#define MOD_MP4_MAX_TAGS 12

/* ---- locating a top-level box --------------------------------------------- */

struct mod_mp4_box {
    uint64_t off;       /* file offset of the box header      */
    uint64_t size;      /* whole box, header included         */
    uint64_t body;      /* file offset of its body            */
};

/* Find the first top-level box of this type in `buf` (as much of the file's head
 * as the caller has). Returns 0 and fills `out`, or -1 when the chain is not
 * readable, a box size does not add up, or the type is not there. `type` is four
 * raw bytes, e.g. "moov". */
int mod_mp4_find(const uint8_t *buf, size_t len, const char type[4],
                 struct mod_mp4_box *out);

/* ---- the splice ------------------------------------------------------------ */

/* Build a replacement for `moov` with `tags` added, gap-filling only: a field the
 * file already carries is left exactly as it is.
 *
 * `moov` points at the box header (as mod_mp4_find returned it). `out` receives
 * the new box, which is the original with `udta` grown or created -- every other
 * byte preserved, including the free slack a muxer left behind, which is used
 * before anything is allowed to move.
 *
 * `grew` receives the number of bytes the container grew by. When `moov_precedes_mdat`
 * is non-zero AND `grew` is non-zero, this also rewrites every `stco`/`co64`
 * offset inside `out` by that amount, because the media it points at has moved.
 *
 * Returns the new `moov` length in bytes, 0 when nothing needed adding, or -1 on
 * any of the refusals above (nothing written). */
int64_t mod_mp4_add_tags(const uint8_t *moov, size_t moov_len,
                         const struct mod_mp4_tag *tags, int ntags,
                         int moov_precedes_mdat,
                         uint8_t *out, size_t out_cap, int64_t *grew);

/* Which gate refused. -1 answers "did it work"; it does not answer "why not", and
 * every refusal ends the same way at the caller -- a verbatim copy with no tags and
 * no cover. Measured 2026-10-16: a save reported `no tag rewrite (add_tags refused
 * (-1))` on a track whose box walk was clean, and there was no way to tell a source
 * whose EXISTING ilst will not walk from one whose chunk offsets will not rewrite.
 * The two want opposite fixes, so the module says which. */
enum mod_mp4_why {
    MOD_MP4_WHY_NONE = 0,       /* it did not refuse */
    MOD_MP4_WHY_ARGS,           /* bad arguments, or more tags than it carries */
    MOD_MP4_WHY_MOOV_BOX,       /* the moov box header would not walk */
    MOD_MP4_WHY_NOT_MOOV,       /* not a moov, or its size disagrees with moov_len */
    MOD_MP4_WHY_ILST_CHILD,     /* a child of the ilst already in the file */
    MOD_MP4_WHY_OUT_CAP,        /* the caller's buffer is too small */
    MOD_MP4_WHY_SHIFT,          /* the stco/co64 offsets would not rewrite */
    MOD_MP4_WHY_LANDING         /* the writer did not land on the end of the hole */
};

/* The same splice, reporting the gate. `why` and `where` may be NULL. `where` is the
 * offset the refusal is about -- the ilst child that would not walk, or the length
 * that did not fit -- and 0 when the gate has no offset of its own. */
int64_t mod_mp4_add_tags_why(const uint8_t *moov, size_t moov_len,
                             const struct mod_mp4_tag *tags, int ntags,
                             int moov_precedes_mdat,
                             uint8_t *out, size_t out_cap, int64_t *grew,
                             int *why, uint64_t *where);

/* A short name for one of the codes above, for a log line. Never NULL. */
const char *mod_mp4_why_name(int why);

#endif /* EP122_MOD_USBSAVE_MP4_H */
