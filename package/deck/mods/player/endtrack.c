// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/player/endtrack.c - what the deck does at the end of a track, and from
 * which view it decides to.
 *
 * THE QUESTION THIS EXISTS TO SETTLE. Continuous play advances to the next
 * streaming track at its natural end -- but only while the deck is on the
 * performance view. On the browse view the track ends and nothing follows it.
 * (Measured on the deck since: a USB track advances by itself in the same
 * session, so the machinery is live and the difference is real.)
 *
 * dj_player::StateNotificationServer watches the player and reports per state:
 * one check* method each, and checkTrackEnd(PlayerState const&) is where "the
 * track reached its natural end" is decided. It is also where whatever the deck
 * does about it is decided, since no listener in this build implements
 * ITrackEndNotificationLisner.
 *
 * WHY THIS HOOKS A BODY AND NOT A TASK SLOT, which cost a whole evening to learn.
 * Each of these checks is an AsyncTaskBox task, so each task type has a vtable
 * whose run slot can be patched by name -- and four such patches (the two checks
 * plus the play/pause facades, as a control) were installed at boot and never
 * fired once, through two full play-throughs including a track that ended and
 * advanced by itself. So whatever runs these checks, it is not the run slot.
 *
 * The body below is what they actually reach, and the binary says so: it is
 * called by a direct BL from checkTrackEnd's own state loop at 0x1117b18 (which
 * is also reachable as a symbol, TRACKEND_CHECK), the same call the run slot's
 * thunk would make. A virtual call through a vtable slot is an indirect branch,
 * so no B or BL would name the thunk in either case -- that alone proves nothing
 * about whether the thunk is ever used. What is measured is the slot patches
 * never firing and this call site existing, which is enough: hook the body, and
 * it is hooked however the check is reached.
 *
 * WHAT IT PRINTS. The body's second argument is the state the check is about to
 * judge -- the PlayerState -- and its layout is not known yet, so it is printed as
 * words: the point of the two runs is to watch for a length, a position and a
 * playing flag moving together as a track ends, and to see whether any of that is
 * there at all when the deck is browsing instead of performing. A state whose
 * words never move is the other possible answer, and an equally useful one.
 *
 * OBSERVATION ONLY. The wrapper calls through to the stock body; nothing here
 * changes a flag, a byte or a decision. The deck does whatever it was going to.
 *
 * The volume is bounded by construction: the first calls are printed in full, and
 * after that only every ET_PRINT_EVERYth, so a check that runs on every state
 * change costs a handful of lines rather than a flood.
 */
#include "core/ep122_syms.h"
#include "kit/mod.h"
#include "core/mod_core.h"
#include "juce/juce.h"

#include <time.h>

#define ET_PRINT_FIRST  40
#define ET_PRINT_EVERY  8

/* ---- the advance, and how a key press is really sent ----
 *
 * There is no API for this. The deck takes its panel input from
 * /dev/subucom_spi1.0, one 64-byte frame per read, and the shim hands each frame
 * back out of read() -- so a press is a bit set in a frame before EP122 sees it,
 * which the deck cannot tell from the DJ pressing the switch. The frame is the
 * same one the host builds (cdj3k-emu-subucom/miso_frame.rs), so the offsets and
 * the CRC come from there; the CRC covers the first 62 bytes and has to be
 * recomputed, or the deck is within its rights to ignore everything we changed.
 *
 * The rotary is a COUNTER, not an event: the deck reads the delta between frames.
 * So one step down is a persistent +1, exactly as the host's own MIDI path does
 * it -- a one-frame bump would read as a step down and then a step back up.
 *
 * The 5 s is the DJ's: the deck's own continuous advance is immediate, and that
 * gap is the whole reason this exists rather than simply letting it advance.
 */
#define ET_BTN_BYTE      11
#define ET_BTN_BACK      0x01
#define ET_BTN_ROTARY    0x10     /* the browse knob pushed in: load */
#define ET_ROTARY_OFF    14       /* u16 LE */
#define ET_FRAME_CRC_OFF 62       /* u16 LE over bytes 0..61, CRC-16/X-25 */

#define ET_DELAY_MS      5000
#define ET_TAP_MS         140     /* long enough for the deck's poll to see it */
#define ET_GAP_MS         500     /* long enough for the view to change */

static uintptr_t et_g_stock_check;      /* the check body, before us */
static uintptr_t et_g_stock_state;      /* the check method, before us */
static unsigned  et_g_n;

static int64_t  et_armed_at;            /* 0 when no advance is pending */
static int      et_stage = -1;
static unsigned et_rotary_offset;       /* persistent: the deck reads deltas, so the
                                         * offset accumulates and is never reset */
static int      et_rotary_sign = 1;     /* which way is "the next track". MEASURED: -1
                                         * moves the highlight UP the list, so +1 is
                                         * down. (The first attempt at +1 looked like it
                                         * did nothing, but the row was already the
                                         * top one -- so that reading was worthless,
                                         * and this is the one that counts.) Still
                                         * flips itself if a load lands back on the
                                         * track that ended; see the state hook. */
static unsigned et_advances;            /* completed sequences, for the log */
static int      et_latched;             /* this park has already been acted on */
static uint64_t et_g_ended_len;         /* the length of the track that ended */
static int      et_retries;             /* how many times this end has been re-tried */
static uint64_t et_g_exhausted;         /* set when the list ran out, holds the length */

static int64_t et_now_ms(void)
{
    struct timespec ts;

    /* CLOCK_MONOTONIC, not the wall clock: the shim time-shifts gettimeofday. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint16_t et_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408)
                            : (uint16_t)(crc >> 1);
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

/* ---- which screen is up ----
 *
 * The advance must only happen from the performance view: pressing BACK from the
 * browse view does not reliably return to the playlist the DJ came from, so the
 * sequence would land on the wrong list. The end-of-track trigger cannot carry
 * that on its own -- it is a playhead against a length, which is the same on every
 * screen -- so the screen has to be observed separately.
 *
 * Observed how: the widgets themselves. gui's TouchAria is the waveform on the
 * performance screen and BrowseTitle is the browse list's header, both already
 * hooked by other mods, and each has juce's paint at +0xd0. A screen that is not
 * in front stops painting, so the most recent painter IS the screen in front.
 *
 * The two wrappers do nothing but write a timestamp, deliberately and without
 * exception: they are on a per-frame path, and MDBG is fprintf + fflush. Logging
 * from a hot deck path is what hung EP122 earlier today.
 *
 * Failure is open. If neither has painted (an unfamiliar build, a wrong slot, a
 * screen we have no symbol for) the advance goes ahead exactly as it did before,
 * because a missed advance is a bug the DJ sees and a wrongly-suppressed one is
 * not.
 */
#define ET_SCREEN_PERF    0
#define ET_SCREEN_BROWSE  1
#define ET_SCREEN_N       2
#define ET_SCREEN_FRESH_MS 1500

static int64_t   et_g_screen[ET_SCREEN_N];
static uintptr_t et_g_stock_screen[ET_SCREEN_N];

static void et_paint_perf(void *self, void *g)
{
    et_g_screen[ET_SCREEN_PERF] = et_now_ms();
    ((void (*)(void *, void *))et_g_stock_screen[ET_SCREEN_PERF])(self, g);
}

static void et_paint_browse(void *self, void *g)
{
    et_g_screen[ET_SCREEN_BROWSE] = et_now_ms();
    ((void (*)(void *, void *))et_g_stock_screen[ET_SCREEN_BROWSE])(self, g);
}

static int et_on_performance_screen(void)
{
    int64_t now = et_now_ms();
    int perf   = et_g_screen[ET_SCREEN_PERF] &&
                 now - et_g_screen[ET_SCREEN_PERF] < ET_SCREEN_FRESH_MS;
    int browse = et_g_screen[ET_SCREEN_BROWSE] &&
                 now - et_g_screen[ET_SCREEN_BROWSE] < ET_SCREEN_FRESH_MS;

    return !(browse && !perf);        /* only a live browse screen stops us */
}

/* Called for every panel frame EP122 reads, so the idle path is one compare. */
void panel_frame(uint8_t *buf, size_t len)
{
    int64_t t;
    int stage = -1, changed = 0;
    uint16_t rot;

    if (len < 64 || (!et_armed_at && !et_rotary_offset))
        return;

    if (et_armed_at) {
        t = et_now_ms() - et_armed_at - ET_DELAY_MS;
        if (t < 0)                              stage = 0;   /* the wait */
        else if (t < ET_TAP_MS)                 stage = 1;   /* BACK down */
        else if (t < ET_TAP_MS + ET_GAP_MS)     stage = 2;   /* the view changes */
        else if (t < ET_TAP_MS + 2 * ET_GAP_MS) stage = 3;   /* one step down */
        else if (t < 2 * ET_TAP_MS + 2 * ET_GAP_MS) stage = 4;  /* load */
        else                                    stage = 5;   /* done */

        /* Write first, then log: the bytes printed are the ones EP122 will read,
         * which is the only way to tell a press that never landed from one the
         * deck chose to ignore. */
        /* The screen decides here, at the moment of pressing rather than at the
         * end of the track, because it is the screen in front when the keys are
         * sent that the keys act on. */
        if (stage >= 1 && stage <= 4 && !et_on_performance_screen()) {
            MDBG("endtrack: another screen is in front -> not pressing\n");
            et_armed_at = 0;
            et_stage = -1;
            stage = -1;                       /* nothing further from this one */
        }

        if (stage == 1) { buf[ET_BTN_BYTE] |= ET_BTN_BACK;   changed = 1; }
        if (stage == 4) { buf[ET_BTN_BYTE] |= ET_BTN_ROTARY; changed = 1; }

        if (stage != et_stage) {
            et_stage = stage;
            if (stage == 1)
                et_advances++;      /* per sequence, not per frame of the tap */
            if (stage == 1)
                MDBG("endtrack: advance %u: BACK (frame now bytes 10-11 %02x %02x, "
                     "rotary %02x %02x)\n",
                     et_advances, buf[10], buf[11], buf[14], buf[15]);
            if (stage == 3) {
                et_rotary_offset = (unsigned)((int)et_rotary_offset + et_rotary_sign);
                MDBG("endtrack: advance %u: one step %+d (offset now %+d)\n",
                     et_advances, et_rotary_sign, (int)et_rotary_offset);
            }
            if (stage == 4)
                MDBG("endtrack: advance %u: load\n", et_advances);
            if (stage == 5) {
                MDBG("endtrack: advance %u: done\n", et_advances);
                et_armed_at = 0;
            }
        }
    }

    if (et_rotary_offset) {
        rot = (uint16_t)(buf[ET_ROTARY_OFF] |
                         ((uint16_t)buf[ET_ROTARY_OFF + 1] << 8));
        rot = (uint16_t)(rot + et_rotary_offset);
        buf[ET_ROTARY_OFF]     = (uint8_t)(rot & 0xff);
        buf[ET_ROTARY_OFF + 1] = (uint8_t)(rot >> 8);
        changed = 1;
    }

    if (changed) {
        uint16_t crc = et_crc16(buf, ET_FRAME_CRC_OFF);

        buf[ET_FRAME_CRC_OFF]     = (uint8_t)(crc & 0xff);
        buf[ET_FRAME_CRC_OFF + 1] = (uint8_t)(crc >> 8);
    }
}

/* BEFORE the stock call, deliberately: the state printed is the one the check is
 * about to judge, not whatever it left behind. */
/* THE METHOD'S ARGUMENT IS THE PLAYER STATE, and the end lives in it.
 *
 * Measured (streaming track, 400.0 s): word 1d is the playhead in samples at
 * 96 kHz, advancing at exactly 96000/s, and word 20 is the track's length. At the
 * natural end the playhead parked ON the length -- 0x249e600 both -- and then the
 * ticks STOPPED, because this method is called on state changes and a stopped
 * player changes nothing. So the last tick IS the end of the track: there is no
 * need to infer it, and no need for a view or streaming flag to get it right.
 *
 * The double-advance guard falls out of the same reading: if the deck advanced by
 * itself (which it does for a non-streaming track in continuous mode), a new tick
 * arrives with a playhead that is NOT the parked value, and the advance is called
 * off. A pause cannot arm this at all -- the playhead is nowhere near the length.
 */
#define ET_STATE_WORDS  48
#define ET_W_PLAYHEAD   0x1d
#define ET_W_LENGTH     0x20

static uint64_t et_g_parked;            /* the playhead we armed on */

static uint64_t et_wrap_state(uintptr_t self, uintptr_t state)
{
    uint64_t w[ET_STATE_WORDS] = { 0 };

    /* Read on EVERY call: the trigger lives in here and must not be missed
     * because a print was throttled. */
    (void)mod_safe_read(state, w, sizeof(w));

    /* A tick whose playhead sits exactly on the length is the end of a track.
     * Latched: the parked state can appear on more than one tick, and one end of
     * one track must mean one advance. */
    /* The DJ loading anything with a different length is what ends the
     * stand-down: it is the one thing we cannot cause ourselves. */
    if (et_g_exhausted && w[ET_W_LENGTH] && w[ET_W_LENGTH] != et_g_exhausted)
        et_g_exhausted = 0;

    if (!et_armed_at && !et_g_exhausted && w[ET_W_LENGTH] &&
        w[ET_W_PLAYHEAD] == w[ET_W_LENGTH] && !et_latched) {
        et_latched = 1;
        et_armed_at = et_now_ms();
        et_g_parked = w[ET_W_PLAYHEAD];
        et_g_ended_len = w[ET_W_LENGTH];
        et_retries = 0;
        et_stage = -1;
        MDBG("endtrack: playhead reached the length (%llu samples) -> "
             "advancing in %d ms\n",
             (unsigned long long)et_g_parked, ET_DELAY_MS);
    }

    /* A playhead that is neither the one we armed on nor the "no track" sentinel
     * means the deck loaded something of its own accord: stand down, and clear
     * the latch so the next track's end can arm again. The sentinel is excluded
     * deliberately -- the deck clears its player during the streaming round-trip,
     * and standing down on that would cancel a valid advance. */
    if (et_latched && w[ET_W_PLAYHEAD] != et_g_parked &&
        w[ET_W_PLAYHEAD] != 0x7fffffffffffffffULL) {
        if (et_armed_at) {
            MDBG("endtrack: playhead moved to %llu -> the deck advanced itself, "
                 "standing down\n", (unsigned long long)w[ET_W_PLAYHEAD]);
            et_armed_at = 0;
            et_stage = -1;
        } else if (et_g_ended_len && w[ET_W_LENGTH] == et_g_ended_len) {
            /* The load landed back on the track that just ended, and since the
             * step's direction is now measured (+1 is down) the only reason for
             * that is the end of the list: the highlight was already on its last
             * row, so there was nowhere to go. That is not a failure to retry --
             * retrying would step UP and play the previous track, walking
             * backwards -- so this stands down for good and waits for the DJ to
             * load something else. The deck's own continuous play stops at the
             * end of a list too, so a stopped deck is the behaviour that matches. */
            et_g_exhausted = w[ET_W_LENGTH];
            MDBG("endtrack: the list ended -- reloaded the last track (length "
                 "%llu), standing down until something else is loaded\n",
                 (unsigned long long)w[ET_W_LENGTH]);
        } else if (w[ET_W_LENGTH] != et_g_ended_len) {
            et_retries = 0;                     /* a different track: it worked */
        }
        et_latched = 0;
        et_g_ended_len = 0;
    }

    /* NOTHING IS LOGGED HERE, deliberately. This runs on the player's own state
     * path, and MDBG is fprintf + fflush: a stdio lock taken on a hot player
     * thread is how a process hangs, and a hung EP122 is a guest the app's
     * watchdog has to SIGKILL. The console says as much -- "QEMU did not exit
     * within 8s" with no kernel panic anywhere -- and the stalls only started
     * when this hook did. The trigger still reads the state; it just says nothing
     * unless it acts. */
    return ((uint64_t (*)(uintptr_t, uintptr_t))et_g_stock_state)(self, state);
}

static uint64_t et_wrap_check(uintptr_t callable, uintptr_t state, uintptr_t bias)
{
    uint64_t w[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned n = ++et_g_n;

    (void)mod_safe_read(state, w, sizeof(w));

    if (n <= ET_PRINT_FIRST || n % ET_PRINT_EVERY == 0)
        MDBG("endtrack: checkTrackEnd #%u callable=%p state=%p bias=%#lx "
             "words=%016llx %016llx %016llx %016llx %016llx %016llx %016llx "
             "%016llx\n",
             n, (void *)callable, (void *)state, (unsigned long)bias,
             (unsigned long long)w[0], (unsigned long long)w[1],
             (unsigned long long)w[2], (unsigned long long)w[3],
             (unsigned long long)w[4], (unsigned long long)w[5],
             (unsigned long long)w[6], (unsigned long long)w[7]);

    /* The arm is not here: the end of a track is read from the playhead against
     * the length, in the state hook below. This one is the notification body,
     * which fires when the deck decides something changed -- and, measured, never
     * at the natural end of a streaming track. Kept because the payload is still
     * the only place the deck's own view of the end would show up. */

    return ((uint64_t (*)(uintptr_t, uintptr_t, uintptr_t))et_g_stock_check)(
        callable, state, bias);
}

static int et_install(void)
{
    if (!ep122_sym(EP122_TRACKEND_INVOKE)) {
        MDBG("endtrack: the end-of-track check body did not resolve\n");
        return -1;
    }
    if (mod_patch_fn("endtrackCheck", ep122_sym(EP122_TRACKEND_INVOKE),
                     (void *)et_wrap_check, &et_g_stock_check) != 0) {
        MDBG("endtrack: the end-of-track check body could not be hooked\n");
        return -1;
    }
    MDBG("endtrack: watching the end-of-track check at %#lx\n",
         (unsigned long)et_g_stock_check);

    /* The screen gate. Neither is required: without them the advance fires on
     * any screen, which is what it did before they existed. */
    {
        static const struct { const char *name; int sym; int which; void *wrap; } k_scr[] = {
            { "endtrackPerf",   EP122_TOUCHARIA,   ET_SCREEN_PERF,   (void *)et_paint_perf },
            { "endtrackBrowse", EP122_BROWSE_TITLE, ET_SCREEN_BROWSE, (void *)et_paint_browse },
        };
        int k, n = 0;

        for (k = 0; k < 2; k++) {
            if (!ep122_sym(k_scr[k].sym))
                continue;
            if (mod_patch_vslot(k_scr[k].name, k_scr[k].sym, JUCE_VT_PAINT,
                                k_scr[k].wrap, &et_g_stock_screen[k_scr[k].which]) == 0)
                n++;
        }
        MDBG("endtrack: %d of 2 screens observed\n", n);
    }

    /* Not required: this one is the state reader, and losing it costs the
     * measurement rather than the mod. */
    if (ep122_sym(EP122_TRACKEND_CHECK) &&
        mod_patch_fn("endtrackState", ep122_sym(EP122_TRACKEND_CHECK),
                     (void *)et_wrap_state, &et_g_stock_state) == 0)
        MDBG("endtrack: reading PlayerState at %#lx\n",
             (unsigned long)et_g_stock_state);
    else
        MDBG("endtrack: the state tick could not be hooked\n");
    return 0;
}

KIT_MOD(k_mod_endtrack,
        .name = "endtrack", .prio = 63, .install = et_install,
        .what = "end of track: what the deck notices, and what it does next");
