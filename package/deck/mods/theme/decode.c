// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * theme/decode.c - the cover's own bytes, where they are still whole.
 *
 * USB Save has two ways to a cover today and both are compromises. It can copy the
 * Beatport client's cached image file -- but the cache has never held one: measured
 * 2026-10-16, Beatport/Artwork/ is empty and the whole userdata partition carries no
 * .jpg or .png at all. Or it can keep the bitmap the deck DREW, which is 320x240
 * stretched to 4:3 for the player view and 90x90 square for the top-left corner of
 * the Info screen. Neither is the artwork; the deck's own record names it at
 * 1400x1400.
 *
 * The artwork is fetched over HTTPS from geo-media.beatport.com and lives only in
 * EP122's address space -- never on disk -- so its decoder is the one place it can
 * be had, and it is where the bytes are still the ORIGINAL ones rather than a
 * re-render. `juce::ImageFileFormat::decodeImage(InputStream&)` receives the stream
 * they arrived on.
 *
 * A C prototype for that slot would be a guess. The vtable says it returns through
 * x8, the indirect result register, writing fields out to +0x68 of that buffer --
 * which is not upstream juce::Image (8 bytes) -- and this build's juce::String is 8
 * bytes while its getFormatName still returns through x8, so the return convention
 * is not settled. A wrapper declaring the wrong one would corrupt the return value
 * the deck depends on. The stub below therefore declares nothing: it saves the
 * registers the callee is entitled to, calls a C helper, puts them back, and
 * tail-branches to the stock function. That is correct whatever the return type is.
 *
 * This file only MEASURES. It logs what the decoder was handed and what it could
 * find behind it, so a capture can be written against what is really there instead
 * of against a guess. Nothing here writes to EP122's memory.
 */
#include "kit/mod.h"
#include "core/mod_core.h"
#include "core/resolve.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

/* decodeImage is at vtable+0x38, and the spec's slot offsets are from the RTTI
 * address point (vtable+0x10). */
#define DECODE_SLOT 0x28

/* How many calls are worth reporting. A decode is not a hot path, but the deck
 * decodes icons too, and this must go quiet rather than fill the console. */
#define PROBE_MAX 40

/* How much of the stream object is worth looking at for a pointer to the payload. */
#define PROBE_WORDS 32

/* What a cover is worth reading. The largest thing named in the cache is 1500x250;
 * a 1400x1400 JPEG runs to a few hundred KB. */
#define PROBE_SCAN_MAX (4u << 20)

/* Reached by name from the asm below, which the compiler cannot see -- so not
 * `static`: GCC is free to clone or rename a static nothing in C appears to touch,
 * and it did, leaving the linker with an undefined `g_probe_args` and a
 * "dangerous relocation: unsupported relocation" for the adrp/add pair. Hidden
 * visibility, because a symbol the shim exports interposes that name in EP122. */
#define STUB_SYM __attribute__((visibility("hidden")))

STUB_SYM uintptr_t g_orig_jpeg;
STUB_SYM uintptr_t g_orig_png;

/* Both are entered from asm / the theme tick, and neither takes arguments. */
void theme_decode_probe(void);
void theme_decode_report(void);


/* ---- the stub ------------------------------------------------------------- *
 *
 * Two entry points, one shape, and NO ARGUMENTS. x0/x1 are this and the stream, x8
 * is the indirect result register, x30 the deck's return address -- but how many
 * arguments the slot really takes is exactly what is not known here, so the stub
 * assumes the worst and preserves everything a callee may rely on: x0-x8 (every
 * register argument plus the result pointer), x18 (the platform register, which a
 * build using -ffixed-x18 is entitled to keep across a call) and x30. Only then does
 * it call the helper and tail-branch to stock.
 *
 * The first version of this saved x0/x1/x8/x30 and passed its two extra values in
 * w2/x3. If the slot takes a third argument that is a live one, and EP122 dies --
 * measured the hard way: the deck segfaulted, apl_start.sh read that as a firmware
 * fault and rebooted the machine, and the guest looped. Nothing here writes an
 * argument register.
 *
 * The stock function is reached through a global rather than a literal so that the
 * branch target is the one mod_patch_vslot actually displaced.
 */
#define DECODE_STUB(label, orig, which_value)                                          \
    ".text\n"                                                                          \
    ".p2align 2\n"                                                                     \
    label ":\n"                                                                        \
    "  sub  sp, sp, #0x70\n"                                                           \
    "  stp  x0, x1, [sp]\n"                                                            \
    "  stp  x2, x3, [sp, #0x10]\n"                                                     \
    "  stp  x4, x5, [sp, #0x20]\n"                                                     \
    "  stp  x6, x7, [sp, #0x30]\n"                                                     \
    "  str  x8, [sp, #0x40]\n"                                                         \
    "  str  x18, [sp, #0x48]\n"                                                        \
    "  str  x30, [sp, #0x50]\n"                                                        \
    "  adrp x9, g_probe_args\n"                                                       \
    "  add  x9, x9, :lo12:g_probe_args\n"                                              \
    "  stp  x0, x1, [x9]\n"                                                            \
    "  stp  x2, x3, [x9, #16]\n"                                                       \
    "  stp  x4, x5, [x9, #32]\n"                                                       \
    "  str  x8, [x9, #48]\n"                                                           \
    "  mov  w10, #" which_value "\n"                                                   \
    "  str  w10, [x9, #56]\n"                                                          \
    "  bl   theme_decode_probe\n"                                                     \
    "  ldp  x0, x1, [sp]\n"                                                            \
    "  ldp  x2, x3, [sp, #0x10]\n"                                                     \
    "  ldp  x4, x5, [sp, #0x20]\n"                                                     \
    "  ldp  x6, x7, [sp, #0x30]\n"                                                     \
    "  ldr  x8, [sp, #0x40]\n"                                                         \
    "  ldr  x18, [sp, #0x48]\n"                                                        \
    "  ldr  x30, [sp, #0x50]\n"                                                        \
    "  add  sp, sp, #0x70\n"                                                           \
    "  adrp x9, " orig "\n"                                                            \
    "  add  x9, x9, :lo12:" orig "\n"                                                  \
    "  ldr  x16, [x9]\n"                                                               \
    "  br   x16\n"

/* What the stub saw. Written by it, read by the helper -- so the helper needs no
 * arguments and the stub never touches an argument register to talk to it. */
struct probe_args {
    void *x[6];        /* x0..x5 as the deck had them */
    uintptr_t sret;    /* x8 */
    int which;         /* 1 jpeg, 2 png */
};

STUB_SYM struct probe_args g_probe_args;

__asm__(
    DECODE_STUB("theme_decode_stub_jpeg", "g_orig_jpeg", "1")
    DECODE_STUB("theme_decode_stub_png",  "g_orig_png",  "2")
);

extern void theme_decode_stub_jpeg(void);
extern void theme_decode_stub_png(void);

/* ---- what the decoder was handed ------------------------------------------ */

/* A pointer that could be one. Every candidate is read through /proc/self/mem, which
 * fails rather than faulting, so this only has to keep obvious non-pointers out.
 *
 * The ceiling was 0x8000000000 -- 39 bits -- and that was WRONG: this deck runs in a
 * 48-bit address space, so its stack sits at 0xfffe... and its heap at 0xffff..., both
 * ABOVE that line. Every real pointer in the stream object was therefore skipped, which
 * is why the probe reported "nothing whole behind it" for images whose buffer it had
 * just been handed, and printed sixteen zeros as if that were the buffer's contents.
 * The 39-bit layout is the aarch64 default and is what it was assumed from; the deck's
 * is not that. Nothing here may assume a ceiling again -- a bad address is a failed
 * read, and a failed read is what the callers already handle. */
#define USER_VA_MAX 0x0001000000000000ull   /* 48-bit user space */

static int plausible(uintptr_t p)
{
    return p > 0x10000u && p < USER_VA_MAX;
}

/* Width and height out of a JPEG at `base` in the deck's memory, by walking its
 * markers to the frame header. 0 unless it really is one.
 *
 * Headers are read one at a time rather than out of one buffer, because a segment
 * between here and the frame header can be arbitrarily long -- an ICC profile in an
 * APP2 is routinely 64 KB -- and a cover that carries one must not read as "not a
 * JPEG". Only the four header bytes of each segment are needed to step over it. */
static int jpeg_dims(uintptr_t base, unsigned *w, unsigned *h)
{
    unsigned i = 2, steps = 0;

    *w = *h = 0;
    while (steps++ < 64) {
        uint8_t m[4];
        unsigned marker, len;

        if (mod_safe_read(base + i, m, sizeof(m)) != 0 || m[0] != 0xff)
            return 0;
        marker = m[1];
        if (marker == 0xff) {              /* fill byte */
            i++;
            continue;
        }
        if (marker == 0xd8) {              /* a second SOI: malformed */
            i += 2;
            continue;
        }
        if (marker == 0xd9 || marker == 0x01)
            return 0;                      /* EOI before any frame, or a TEM */
        len = ((unsigned)m[2] << 8) | m[3];
        if (len < 2)
            return 0;
        /* SOF0..SOF15, minus the two that are not frame headers. */
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8) {
            uint8_t f[9];

            if (len < 7 || mod_safe_read(base + i + 4, f, sizeof(f)) != 0)
                return 0;
            *h = ((unsigned)f[1] << 8) | f[2];
            *w = ((unsigned)f[3] << 8) | f[4];
            return *w > 0 && *h > 0;
        }
        /* An entropy-coded scan starts at SOS; past it nothing is walkable without
         * decoding, and a frame header would have come first anyway. */
        if (marker == 0xda)
            return 0;
        i += 2 + len;
    }
    return 0;
}

static int png_dims(const uint8_t *b, unsigned n, unsigned *w, unsigned *h)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

    if (n < 24 || memcmp(b, sig, 8) != 0 || memcmp(b + 12, "IHDR", 4) != 0)
        return 0;
    *w = ((unsigned)b[16] << 24) | ((unsigned)b[17] << 16) |
         ((unsigned)b[18] << 8) | b[19];
    *h = ((unsigned)b[20] << 24) | ((unsigned)b[21] << 16) |
         ((unsigned)b[22] << 8) | b[23];
    return *w > 0 && *h > 0;
}

/* Is there an image at `p`, and which kind? 1 jpeg, 2 png, 0 neither.
 *
 * BOTH formats are tried regardless of which slot fired, because the slot does not
 * tell you: a load tries the formats in turn on the SAME stream and the SAME result
 * register, so the first attempt is what gets recorded, and a PNG reported itself
 * through the JPEG slot. Format is a property of the bytes, and the magic is right
 * there -- guessing it from the caller was the mistake. */
static int image_at(uintptr_t p, unsigned *w, unsigned *h)
{
    uint8_t head[24];

    if (mod_safe_read(p, head, sizeof(head)) != 0)
        return 0;
    if (png_dims(head, sizeof(head), w, h))
        return 2;
    if (jpeg_dims(p, w, h))
        return 1;
    return 0;
}

/* Walk down from an object for something that begins like an image. A stream does NOT
 * have to hold the bytes directly -- a holder, a ReferenceCountedObjectPtr or a vector
 * puts one more indirection in the way, and the first version of this only looked at
 * the object's own words, which is why every decode reported "nothing whole behind
 * it". Depth-bounded and every read goes through /proc/self/mem, so a word that is not
 * a pointer costs one failed read and nothing else. */
#define CHASE_WORDS 8
#define CHASE_DEPTH 3

static uintptr_t chase(uintptr_t obj, unsigned *w, unsigned *h, int *fmt,
                       unsigned depth, unsigned *at, unsigned *lvl)
{
    uintptr_t words[CHASE_WORDS];
    unsigned i;

    if (depth == 0 || mod_safe_read(obj, words, sizeof(words)) != 0)
        return 0;

    for (i = 0; i < CHASE_WORDS; i++) {
        uintptr_t p = words[i];
        uintptr_t found;
        int f;

        if (!plausible(p))
            continue;
        f = image_at(p, w, h);
        if (f) {
            *at = i;
            *lvl = depth;
            *fmt = f;
            return p;
        }
        found = chase(p, w, h, fmt, depth - 1, at, lvl);
        if (found)
            return found;
    }
    return 0;
}

/* How big the thing is: from here to its end marker, bounded. Only ever an estimate
 * -- FF D9 can occur inside entropy-coded data -- but the difference between a 4 KB
 * icon and a 300 KB cover is not subtle. */
static uintptr_t image_len(uintptr_t p)
{
    unsigned off = 0;
    uint8_t chunk[4096];

    while (off < PROBE_SCAN_MAX) {
        unsigned k, take = sizeof(chunk);

        if (PROBE_SCAN_MAX - off < take)
            take = PROBE_SCAN_MAX - off;
        if (mod_safe_read(p + off, chunk, take) != 0)
            break;
        for (k = 0; k + 1 < take; k++)
            if (chunk[k] == 0xff && chunk[k + 1] == 0xd9)
                return off + k + 2;
        off += take;
    }
    return PROBE_SCAN_MAX;
}

static uintptr_t find_payload(void *stream, unsigned *w, unsigned *h,
                              unsigned *scanned, unsigned *lvl, int *fmt,
                              uintptr_t *payload)
{
    uintptr_t p;

    *w = *h = *scanned = *lvl = *fmt = 0;
    *payload = 0;
    p = chase((uintptr_t)stream, w, h, fmt, CHASE_DEPTH, scanned, lvl);
    if (!p)
        return 0;
    *payload = p;
    /* The end marker only means something for a JPEG; a PNG's length is whatever the
     * stream says it is, and that is already known. */
    return *fmt == 1 ? image_len(p) : 0;
}

/* ---- what the probe saw ---------------------------------------------------- *
 *
 * One entry per DISTINCT call site, not per call, and NOTHING IS LOGGED HERE. That
 * is the whole design, and it is the second version: the first logged each call as
 * it happened, and EP122 died on exactly the 24th -- which was its own cap, so the
 * crash tracked the logging rather than the hook. Startup decodes a missing logo in
 * a retry loop (`gui::jog_display::ArtworkWidget` complaining that logoImage is not
 * valid), and a console write per iteration is enough to change how that loop ends.
 * A dead EP122 makes apl_end.sh reboot the machine, so the guest then loops. MDBG
 * from anywhere near this path is the same mistake the mods' own notes record for
 * the player state tick.
 *
 * What is left here is a register snapshot, a linear scan of at most PROBE_SITES
 * entries, and one look at the stream per distinct site. The report happens later,
 * from the throttled setFill tick the other mods already report from.
 */
#define PROBE_SITES 24

/* ---- staged copies ---------------------------------------------------------- *
 *
 * The image's bytes are copied HERE, in the decode path, and written out later from
 * the tick. Writing the file directly at decode time is a console-write-sized problem
 * on a path that must stay cheap, and reading the buffer later does not work at all:
 * measured 2026-10-16, a dump taken from the tick contained the right file length and
 * JPEG content from offset 0x20 on, with something else in the first 32 bytes -- the
 * caller's buffer is freed and partially reused the moment the decode returns, so by
 * report time it is not the image any more. A few hundred KB of .bss buys a copy that
 * is.
 */
#define COPY_SLOTS  4
#define COPY_MAX    (192u * 1024)
#define COPY_CHUNK  32768

/* ---- the cover, for USB Save ------------------------------------------------- *
 *
 * The one image the deck FETCHED for the track, kept as it arrived. USB Save used to
 * take whichever bitmap the deck drew last, and that is how a numeral badge or the
 * Beatport logo ended up in the saved files: the logo is 90x90, which passes the
 * draw path's 64 px floor and its "squarish" test, so it wins whenever it is drawn
 * after the cover. Measured on the live deck 2026-10-16, a streamed track decodes
 * exactly two images -- the cover as a 240x240 JPEG (about 13 KB) and the Beatport
 * logo as a 90x90 PNG (about 1 KB) -- so pixel area tells them apart with room to
 * spare. 128x128 sits between them and well above every badge the deck draws.
 *
 * This is a copy of the encoded image, so what USB Save writes is the picture that
 * came off the wire rather than anything re-rendered. */
#define COVER_MIN_PX  (128 * 128)

/* AND IT MUST BE A JPEG. Size alone is not enough: the deck also decodes badge
 * graphics -- the player-number numerals, the Beatport logo -- and a large one passes
 * any pixel count we could set. Measured 2026-10-16, a saved cover came out as a
 * numeral "6". Format is the discriminator that actually holds: a streamed track's
 * cover arrives as a JPEG (the store's release art is a .jpg), while every badge seen
 * so far is a PNG. */
#define COVER_WANT_FMT 1

/* And it must be the size the deck fetches a cover at: 240x240, square. Measured
 * repeatedly on the live deck, every streamed track's artwork arrives as a 240x240
 * JPEG and every badge as something else, so this plus the format is as tight a
 * description of "the cover" as the evidence supports. */
#define COVER_WANT_W   240
#define COVER_WANT_H   240
#define COVER_MAX     (512u * 1024)

/* How old a capture may be and still be taken as the current track's cover.
 *
 * The deck decodes a track's artwork once, when it first shows the track -- at the
 * load, not repeatedly -- and a deck that has the picture already does not decode it
 * again at all. So a capture CANNOT be tied to the save: an earlier attempt cleared it
 * when each save began and the clear raced the decode, losing a good cover and falling
 * back to the re-render. What is left is the age.
 *
 * The age is a WEAK test, and 2026-10-16 showed what it costs. The deck decodes a
 * release's artwork once, not once per track: in one boot a 240x240 arrived at t=20 s
 * for the first track of a release and that track's save at t=33 s took it, while a
 * second track of the SAME release saved at t=148 s was handed nothing but 90x90
 * icons in between -- so the only 240x240 on hand was 128 s old and the limit threw
 * away the right cover. The limit is still here as the last resort for a capture with
 * no release to be matched against, but USB Save no longer relies on it: it keys the
 * cover by the hash the deck's own record names the artwork by, and a capture taken
 * for one track of a release serves the rest of that release whatever its age. */
#define COVER_MAX_AGE_S 120

static uint8_t  g_cover[COVER_MAX];
static unsigned g_cover_len;              /* 0 = nothing captured yet */
static int      g_cover_fmt;              /* 1 jpeg, 2 png */
static int32_t  g_cover_w, g_cover_h;
static time_t   g_cover_at;              /* when it was captured */

struct probe_site {
    void      *self, *stream;
    void      *arg[4];       /* x2..x5 as the deck had them */
    uintptr_t  sret;
    int        which;
    unsigned   hits;
    unsigned   w, h, at;     /* where the payload pointer sat, in words */
    unsigned   lvl;          /* how many hops down it was */
    int        fmt;         /* what the bytes turned out to be: 1 jpeg, 2 png */
    uintptr_t  payload;     /* the image's own bytes, as found */
    int        stage;       /* which staged copy holds them, -1 for none */
    uintptr_t  len;
    uintptr_t  words[6];     /* the stream object's own words, vptr first */
    /* The first bytes of the buffer the stream holds, taken WHILE IT IS LIVE. The
     * buffer of a MemoryInputStream handed to a decoder is usually the caller's, and
     * by the time this is reported -- from a tick, up to seconds later -- that memory
     * belongs to someone else. Reading it at report time shows whatever landed there
     * since, which is worse than showing nothing. */
    uint8_t    head[16];
};

STUB_SYM struct probe_site g_sites[PROBE_SITES];
STUB_SYM unsigned g_sites_n;
STUB_SYM unsigned g_probe_total;

static uint8_t  g_stage[COPY_SLOTS][COPY_MAX];
static unsigned g_stage_len[COPY_SLOTS];
static unsigned g_stage_n;

/* Called from the stub, with NO arguments -- everything it needs it reads out of
 * g_probe_args, which the stub filled in from the deck's own registers. Runs on
 * whatever thread decodes an image, including the message thread, so it does no
 * I/O and allocates nothing. */
void theme_decode_probe(void)
{
    struct probe_args a = g_probe_args;   /* one read: the deck may call again at once */
    struct probe_site *s;
    unsigned i;

    g_probe_total++;
    if (!MLOG_AT(MOD_LOG_DEBUG))
        return;

    for (i = 0; i < g_sites_n; i++) {
        if (g_sites[i].stream == a.x[1] && g_sites[i].sret == a.sret) {
            g_sites[i].hits++;
            return;
        }
    }
    if (g_sites_n >= PROBE_SITES)
        return;

    s = &g_sites[g_sites_n++];
    s->self = a.x[0];
    s->stream = a.x[1];
    s->arg[0] = a.x[2];
    s->arg[1] = a.x[3];
    s->arg[2] = a.x[4];
    s->arg[3] = a.x[5];
    s->sret = a.sret;
    s->which = a.which;
    s->hits = 1;
    /* The object's own words, so the class can be named from its vptr afterwards --
     * the load slide is 0, so a logged address is a file offset. */
    if (mod_safe_read((uintptr_t)s->stream, s->words, sizeof(s->words)) != 0)
        memset(s->words, 0, sizeof(s->words));
    /* juce::MemoryInputStream is {vptr, data, size, position}, read off its own
     * setPosition: word 1 is the buffer. Snapshot its first bytes now, while the
     * decode that owns it is still on this thread's stack. */
    if (plausible(s->words[1]) && mod_safe_read(s->words[1], s->head, sizeof(s->head)) != 0)
        memset(s->head, 0, sizeof(s->head));
    s->len = find_payload(s->stream, &s->w, &s->h, &s->at, &s->lvl, &s->fmt,
                          &s->payload);
    s->stage = -1;

    /* And the cover: the biggest image this decode produced, taken now. Copied from
     * the same live pointer the staging above uses, but into its own buffer, because
     * this one outlives the report. */
    if (s->payload && s->fmt == COVER_WANT_FMT &&
        (int32_t)s->w == COVER_WANT_W && (int32_t)s->h == COVER_WANT_H) {
        unsigned long long want = s->words[2];

        if (want == 0 || want > COVER_MAX)
            want = (s->len && s->len <= COVER_MAX) ? s->len : 0;
        if (want > 0) {
            unsigned long long done = 0;
            int ok = 1;

            while (done < want) {
                unsigned take = (unsigned)((want - done) > COPY_CHUNK
                                           ? COPY_CHUNK : (want - done));

                if (mod_safe_read(s->payload + done, g_cover + done, take) != 0) {
                    ok = 0;
                    break;
                }
                done += take;
            }
            if (ok) {
                g_cover_fmt = s->fmt;
                g_cover_w = (int32_t)s->w;
                g_cover_h = (int32_t)s->h;
                g_cover_at = time(NULL);
                __atomic_store_n(&g_cover_len, (unsigned)done, __ATOMIC_RELEASE);
            }
        }
    }

    /* Copy the image out now -- see the staged-copies comment. Length comes from the
     * stream, which knows it exactly, and only falls back to the JPEG end-marker scan
     * when the stream's own size is not usable. */
    if (s->payload && s->fmt && g_stage_n < COPY_SLOTS) {
        unsigned long long want = s->words[2];

        if (want == 0 || want > COPY_MAX)
            want = (s->len && s->len <= COPY_MAX) ? s->len : COPY_MAX;
        if (want > 0) {
            unsigned long long done = 0;
            uint8_t *dst = g_stage[g_stage_n];
            int ok = 1;

            while (done < want) {
                unsigned take = (unsigned)((want - done) > COPY_CHUNK
                                           ? COPY_CHUNK : (want - done));

                if (mod_safe_read(s->payload + done, dst + done, take) != 0) {
                    ok = 0;
                    break;
                }
                done += take;
            }
            if (ok && done > 0) {
                g_stage_len[g_stage_n] = (unsigned)done;
                s->stage = (int)g_stage_n;
                g_stage_n++;
            }
        }
    }
}

/* ---- arming, and the check that the patch is on the right thing ------------- *
 *
 * The hook is NOT installed at boot. Installed there, it killed EP122 twice: the deck
 * spends its first second in a retry loop decoding a logo it cannot decode
 * (`gui::jog_display::ArtworkWidget` logging that logoImage is not valid -- the same
 * stream and result register over and over), and both a console write in that loop and
 * a saved register frame in every iteration are enough to finish it. The deck's own
 * apl_end.sh treats a dead EP122 as a firmware fault and reboots the machine, so the
 * guest then loops. Arming here instead -- from the tick, once the deck has settled --
 * leaves that whole stretch stock, and the artwork decode this is FOR happens minutes
 * later, when a track is loaded.
 *
 * Before displacing anything, the slot is read back and checked. resolve.c stores what
 * it peeked out of the slot, so this is the stock decodeImage's own address, and on a
 * deck whose load slide is 0 it must be exactly the address the spec's reference build
 * resolved (the comment beside the row in ep122_sym_slots). A mismatch means the offset
 * is not pointing where this file thinks, and then nothing is patched at all. */
#define ARM_DELAY_S 45

static int    g_armed;
static int    g_arm_declined;
static time_t g_t0;

/* Seconds since this mod installed. The guest's clock is set before EP122 starts, so
 * wall clock is enough to tell "the deck has settled" from "the deck is booting". */
static long decode_uptime_s(void)
{
    time_t now = time(NULL);

    if (g_t0 == 0)
        g_t0 = now;
    return (long)(now - g_t0);
}

static void decode_arm(void)
{
    uintptr_t jpeg = ep122_sym(EP122_JPEG_DECODE);
    uintptr_t png = ep122_sym(EP122_PNG_DECODE);

    g_armed = 1;
    MDBG("decode: arming late. slot holds jpeg=%#lx png=%#lx\n",
         (unsigned long)jpeg, (unsigned long)png);

    /* Both must be real code pointers. EP122's .text starts well above this floor and
     * nothing in the image is that small a number. */
    if (jpeg < 0x10000 || png < 0x10000) {
        g_arm_declined = 1;
        MWARN("decode: a decoder slot holds %#lx/%#lx -- not patching anything\n",
              (unsigned long)jpeg, (unsigned long)png);
        return;
    }

    if (mod_patch_vslot("jpeg decodeImage", EP122_JPEG_IMAGE_FORMAT, DECODE_SLOT,
                        (void *)theme_decode_stub_jpeg, &g_orig_jpeg) != 0)
        MWARN("decode: jpeg decodeImage not hooked\n");
    if (mod_patch_vslot("png decodeImage", EP122_PNG_IMAGE_FORMAT, DECODE_SLOT,
                        (void *)theme_decode_stub_png, &g_orig_png) != 0)
        MWARN("decode: png decodeImage not hooked\n");
    MDBG("decode: armed (jpeg orig=%#lx png orig=%#lx)\n",
         (unsigned long)g_orig_jpeg, (unsigned long)g_orig_png);
}

/* Write a decoded image's own bytes out, so it can be LOOKED at.
 *
 * Everything this probe has reported so far has been a description -- a size, a
 * format, a pair of dimensions -- and a description cannot say whether an image is
 * the album art or a numeral badge, which is exactly the question. The bytes can.
 *
 * /mnt is the deck's own userdata partition: EP122 already writes streamed tracks
 * there, and it is the one place a mod can put something that the host can read back
 * without touching the live USB stick (which must not be mounted). The host reads it
 * from a copy of emmc.qcow2. */
#define DUMP_DIR   "/mnt"
#define DUMP_MAX   (512u * 1024)

static void dump_image(unsigned i, struct probe_site *s)
{
    char path[64];
    uint8_t chunk[8192];
    unsigned long long left;
    int fd;

    if (s->stage < 0 || !s->fmt)
        return;
    left = g_stage_len[s->stage];
    if (left > DUMP_MAX)
        left = DUMP_MAX;
    snprintf(path, sizeof(path), "%s/cdj3k-img-%u.%s", DUMP_DIR, i,
             s->fmt == 1 ? "jpg" : "png");

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        MWARN("decode: cannot write %s\n", path);
        return;
    }
    {
        unsigned long long done = 0;

        while (done < left) {
            unsigned take = (unsigned)((left - done) > sizeof(chunk)
                                       ? sizeof(chunk) : (left - done));

            memcpy(chunk, g_stage[s->stage] + done, take);
            if (write(fd, chunk, take) != (ssize_t)take)
                break;
            done += take;
        }
        close(fd);
        MDBG("decode: wrote %llu bytes to %s\n", done, path);
    }
}

/* How long ago the cover on hand was captured, in seconds, or -1 when there is none.
 *
 * Separate from the accessor below because the two questions are different. Whether a
 * capture may be WRITTEN into the track being saved is the age rule in the accessor --
 * generous, because a deck that already has the picture never decodes it again. Whether
 * it is young enough to be evidence about WHICH release it came from is USB Save's
 * question before it keeps one for the rest of the boot, where a guess would outlive
 * the save that made it. */
long theme_decoded_cover_age(void)
{
    unsigned n = __atomic_load_n(&g_cover_len, __ATOMIC_ACQUIRE);

    if (n == 0 || n > COVER_MAX)
        return -1;
    return (long)(time(NULL) - g_cover_at);
}

/* The cover the deck fetched, for USB Save. 1 when there is one, with `bytes` pointing
 * at the module's own copy of the encoded image -- valid until the next cover is
 * captured, so a caller that needs it later copies it. */
int theme_decoded_cover(const uint8_t **bytes, size_t *len, int *fmt,
                        int32_t *w, int32_t *h)
{
    unsigned n = __atomic_load_n(&g_cover_len, __ATOMIC_ACQUIRE);
    long age = theme_decoded_cover_age();

    if (!bytes || !len || !fmt || !w || !h || n == 0 || n > COVER_MAX)
        return 0;
    if (age < 0 || age > COVER_MAX_AGE_S)
        return 0;                       /* too old to belong to the track being saved */
    *bytes = g_cover;
    *len = n;
    *fmt = g_cover_fmt;
    *w = g_cover_w;
    *h = g_cover_h;
    return 1;
}

/* Printed from the throttled setFill tick, next to the mods' other reports. Also the
 * only place the hook is ever installed -- see decode_arm. */
void theme_decode_report(void)
{
    static unsigned printed;
    unsigned i;

    if (!g_armed && !g_arm_declined && decode_uptime_s() >= ARM_DELAY_S)
        decode_arm();

    if (!MLOG_AT(MOD_LOG_DEBUG) || printed >= g_sites_n)
        return;

    if (printed == 0)
        MDBG("decode: %u decode calls so far; %u distinct sites\n",
             g_probe_total, g_sites_n);

    for (i = printed; i < g_sites_n; i++) {
        struct probe_site *s = &g_sites[i];

        /* x2/x3 are printed whether or not a payload was found: they are what says
         * how many arguments the slot really takes. A decoder takes two (this,
         * stream); a third register holding a live pointer would mean the slot is
         * not the decoder at all. */
        MDBG("decode: %s self=%p stream=%p x2=%p x3=%p sret=%p hits=%u\n",
             s->which == 1 ? "jpeg slot" : "png slot", s->self, s->stream,
             s->arg[0], s->arg[1], (void *)s->sret, s->hits);
        /* The stream's own words, vptr first. Slide is 0, so the vptr names the class
         * once this is read back on the Mac. */
        MDBG("decode:   stream words: %#lx %#lx %#lx %#lx %#lx %#lx\n",
             (unsigned long)s->words[0], (unsigned long)s->words[1],
             (unsigned long)s->words[2], (unsigned long)s->words[3],
             (unsigned long)s->words[4], (unsigned long)s->words[5]);
        if (s->fmt)
            MDBG("decode:   -> %s %ux%u found %u hops down (word %u)%s\n",
                 s->fmt == 1 ? "jpeg" : "png", s->w, s->h,
                 CHASE_DEPTH - s->lvl, s->at,
                 s->len ? "" : ", length not scanned");
        else
            MDBG("decode:   -> no image within %d hops of the stream\n", CHASE_DEPTH);

        dump_image(i, s);

        /* What the buffer actually held, snapshotted at decode time: word 1 is the
         * buffer, word 2 its length. The first bytes are what say whether this decoder
         * is being handed a JPEG at all -- and a JPEG's own frame header says at what
         * size, so a thumbnail and a cover are told apart without decoding anything. */
        {
            char hex[sizeof(s->head) * 3 + 1];
            unsigned j;

            for (j = 0; j < sizeof(s->head); j++)
                snprintf(hex + j * 3, 4, "%02x ", s->head[j]);
            hex[sizeof(s->head) * 3 - 1] = '\0';
            MDBG("decode:   buffer(%lu bytes) starts: %s\n",
                 (unsigned long)s->words[2], hex);
        }
    }
    printed = g_sites_n;
}

/* ---- install -------------------------------------------------------------- */

/* Nothing is hooked here -- see decode_arm for why, and for the check that runs
 * before it happens. This only establishes that there is something to arm. */
static int decode_install(void)
{
    if (ep122_sym(EP122_JPEG_IMAGE_FORMAT) == 0 && ep122_sym(EP122_PNG_IMAGE_FORMAT) == 0) {
        MWARN("decode: no image decoder resolved -> probe off\n");
        return -1;
    }
    /* The runtime address of a resolved symbol, minus the address the spec recorded,
     * is the load slide. Every address this file logs can be turned back into a file
     * offset with it, which is what makes the probe's output readable offline. */
    MDBG("decode: slide=%#lx; hook held back %d s (a hooked startup kills EP122)\n",
         (unsigned long)(ep122_sym(EP122_JPEG_IMAGE_FORMAT) - (uintptr_t)0x24cc6d0),
         ARM_DELAY_S);
    g_t0 = time(NULL);
    return 0;
}

KIT_MOD(k_mod_decode,
        .name = "decode", .prio = 71, .install = decode_install,
        .what = "image-decoder probe, armed after the deck settles (diagnostic)");
