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

/* ---- which screen is SHOWN ----
 *
 * Paint turned out to be the wrong question: the widgets do not repaint on a
 * timer, so their paint ages say "when was this screen last changed to", not
 * "which screen is up" -- a screen with a widget that happens not to be redrawn
 * looks identical to a screen that is not there. What a screen change actually is,
 * is a SHOW: one screen's widget is made visible and the other's is not. juce's
 * setVisible is at +0x60, the same slot other mods already patch elsewhere.
 *
 * Every screen the spec names is watched here, and the last one shown is what the
 * gate believes. One calibration run per screen tells us the whole mapping, which
 * is why they are all in one table rather than one at a time.
 */
#define ET_SHOW_N 6
static int64_t et_g_shown[ET_SHOW_N];      /* when each was last shown */
static int     et_g_shown_last = -1;       /* and which one that was */
static uintptr_t et_g_stock_show[ET_SHOW_N];

static const char *const et_g_show_name[ET_SHOW_N] = {
    "performance", "browse", "playlist", "utility", "source", "tracklist",
};

static int et_g_perf_visible;              /* is the play screen itself up */
static int et_g_show_log = -2, et_g_show_log_vis;

static void et_shown(int which, int visible)
{
    /* Slot 0 is the PLAY SCREEN ITSELF, not a widget on it: a screen is shown and
     * hidden whole, so this is where "the play screen came up" is visible at all.
     * Its child never hears about the parent's visibility changing, which is why
     * watching the child only ever printed "performance" once, at boot. */
    if (which == 0)
        et_g_perf_visible = visible;

    if (visible) {
        et_g_shown[which] = et_now_ms();
        et_g_shown_last = which;
    }

    /* One line per CHANGE of state, not per re-show: the browse list's widget
     * re-shows on every list action, which is a dozen lines a second. */
    if (which != et_g_show_log || visible != et_g_show_log_vis) {
        MDBG("endtrack: screen %s: %s\n", et_g_show_name[which],
             visible ? "shown" : "hidden");
        et_g_show_log = which;
        et_g_show_log_vis = visible;
    }
}

#define ET_SHOW_WRAP(WHICH)                                                \
    static void et_show_##WHICH(void *self, int visible)                   \
    {                                                                      \
        et_shown(WHICH, visible);                                          \
        ((void (*)(void *, int))et_g_stock_show[WHICH])(self, visible);    \
    }

ET_SHOW_WRAP(0) ET_SHOW_WRAP(1) ET_SHOW_WRAP(2)
ET_SHOW_WRAP(3) ET_SHOW_WRAP(4) ET_SHOW_WRAP(5)

static void *const et_g_show_wrap[ET_SHOW_N] = {
    (void *)et_show_0, (void *)et_show_1, (void *)et_show_2,
    (void *)et_show_3, (void *)et_show_4, (void *)et_show_5,
};

/* ---- WHICH SCREEN, from the deck's own view switching ----
 *
 * gui::ViewTransitionManager is what performs a screen change, so a screen switch
 * calls exactly one of its virtuals and the argument is the view going up. Which
 * virtual, nobody knows yet, so the low-numbered ones after the two destructors are
 * logged by index and one calibration run names it.
 *
 * THE LOGGING IS RATE LIMITED, and that is not a nicety: one of these slots may be
 * driven per frame, and an fprintf on a per-frame deck path is what hung the guest
 * and had the app's watchdog SIGKILL it. Five lines a second is enough to see which
 * slot fires on a switch and still safe to leave running.
 */
#define ET_TV_SLOTS   8            /* slots 0..7 of the vtable; 0 and 1 are dtors */
#define ET_TV_FIRST   2
static uintptr_t et_g_stock_tv[ET_TV_SLOTS];
static int64_t   et_g_tv_log;

static void et_tv(int slot)
{
    int64_t now = et_now_ms();

    if (now - et_g_tv_log >= 200) {
        et_g_tv_log = now;
        MDBG("endtrack: view transition: slot %d\n", slot);
    }
}

#define ET_TV_WRAP(K)                                                      \
    static uint64_t et_tv_##K(uintptr_t a, uintptr_t b, uintptr_t c)       \
    {                                                                      \
        et_tv(K);                                                          \
        return ((uint64_t (*)(uintptr_t, uintptr_t, uintptr_t))            \
                et_g_stock_tv[K])(a, b, c);                                 \
    }

ET_TV_WRAP(2) ET_TV_WRAP(3) ET_TV_WRAP(4) ET_TV_WRAP(5)
ET_TV_WRAP(6) ET_TV_WRAP(7)

static void *const et_g_tv_wrap[ET_TV_SLOTS] = {
    NULL, NULL, (void *)et_tv_2, (void *)et_tv_3,
    (void *)et_tv_4, (void *)et_tv_5, (void *)et_tv_6, (void *)et_tv_7,
};

/* ---- WHICH SCREEN, by who painted last ----
 *
 * MEASURED, after three wrong instruments, and the measurements are worth keeping:
 *
 *  - The deck's screens are NOT toggled by component visibility. Switching away and
 *    back never hid or re-showed the play screen's widget, and setVisible never
 *    fired, parent or child. A visibility-based gate therefore answers "no" forever.
 *  - gui::ViewTransitionManager's virtuals are not called on a screen change either.
 *  - What DOES fire is PAINT, when a screen is drawn. A screen switch repaints both
 *    the screen being left and the one being entered, in that order -- observed as
 *    perf 10302ms / browse 11417ms, i.e. the switch you had just made. So the most
 *    recently painted screen is the screen you last changed to, which is the screen
 *    you are on.
 *
 * Paint does not fire on a timer: at a track's end one widget sat 43 s stale and the
 * other 181 s, and five seconds later both had aged by exactly five seconds. So the
 * ages are compared, never windowed.
 *
 * Screen 0 is the play screen. Everything else here is a screen the spec can name;
 * a screen that is not in this table cannot be seen, and the cap is the only guard
 * against a widget from hours ago winning the argument. With no evidence at all the
 * answer is yes, as it was before any of this existed: a missed advance is a bug the
 * DJ sees, a wrongly-suppressed one is not.
 */
#define ET_SCREEN_N 6
#define ET_SCREEN_CAP_MS 600000        /* 10 minutes */

static int64_t   et_g_painted[ET_SCREEN_N];
static uintptr_t et_g_stock_paint[ET_SCREEN_N];

static const char *const et_g_screen_name[ET_SCREEN_N] = {
    "performance", "browse", "playlist", "utility", "source", "tracklist",
};

static void et_painted(int which)
{
    et_g_painted[which] = et_now_ms();
}

#define ET_PAINT_WRAP(K)                                                   \
    static void et_paint_##K(void *self, void *g)                          \
    {                                                                      \
        et_painted(K);                                                     \
        ((void (*)(void *, void *))et_g_stock_paint[K])(self, g);           \
    }

ET_PAINT_WRAP(0) ET_PAINT_WRAP(1) ET_PAINT_WRAP(2)
ET_PAINT_WRAP(3) ET_PAINT_WRAP(4) ET_PAINT_WRAP(5)

static void *const et_g_paint_wrap[ET_SCREEN_N] = {
    (void *)et_paint_0, (void *)et_paint_1, (void *)et_paint_2,
    (void *)et_paint_3, (void *)et_paint_4, (void *)et_paint_5,
};

static long et_screen_age_ms(int which)
{
    return et_g_painted[which] ? (long)(et_now_ms() - et_g_painted[which]) : -1;
}

/* The screen most recently painted, or -1 when that was too long ago to mean
 * anything. */
static int et_screen_now(void)
{
    int i, best = -1;
    long best_age = ET_SCREEN_CAP_MS;

    for (i = 0; i < ET_SCREEN_N; i++) {
        long age = et_screen_age_ms(i);

        if (age >= 0 && age < best_age) {
            best_age = age;
            best = i;
        }
    }
    return best;
}

static int et_on_performance_screen(void)
{
    int which = et_screen_now();

    return which < 0 || which == 0;
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
        /* THE SCREEN IS JUDGED ONCE, as the sequence begins, and never again
         * during it. The first press is BACK, whose whole purpose is to leave the
         * performance screen: re-checking on every frame sees the browse list
         * painting 200 ms later and cancels the very sequence that caused it.
         * Measured -- that is exactly what this gate did the first time it ran.
         * The screen that matters is the one the DJ was on when the track ended. */
        if (stage == 1 && et_stage != 1) {
            int which = et_screen_now();

            MDBG("endtrack: screen check: painted last %s %ldms ago -> %s\n",
                 which < 0 ? "nothing" : et_g_screen_name[which],
                 which < 0 ? -1L : et_screen_age_ms(which),
                 et_on_performance_screen() ? "pressing" : "not pressing");
        }
        if (stage == 1 && et_stage != 1 && !et_on_performance_screen()) {
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
             "advancing in %d ms (last painted: %s %ldms ago)\n",
             (unsigned long long)et_g_parked, ET_DELAY_MS,
             et_screen_now() < 0 ? "nothing" : et_g_screen_name[et_screen_now()],
             et_screen_now() < 0 ? -1L : et_screen_age_ms(et_screen_now()));
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

    /* The screen gate: the deck's own views, by which one painted last. None is
     * required -- without any of them the advance fires on any screen, which is
     * what it did before the gate existed. */
    {
        static const struct { const char *name; int sym; } k_scr[ET_SCREEN_N] = {
            { "endtrackPerf",   EP122_TOUCHARIA },
            { "endtrackBrowse", EP122_BROWSE_TITLE },
            { "endtrackPlaylist", EP122_PLAYLIST_VIEW },
            { "endtrackUtility", EP122_UTILITY_VIEW },
            { "endtrackSource", EP122_SOURCE_SELECT_VIEW },
            { "endtrackTracklist", EP122_TRACKLIST },
        };
        int k, n = 0;

        for (k = 0; k < ET_SCREEN_N; k++) {
            if (!ep122_sym(k_scr[k].sym))
                continue;
            if (mod_patch_vslot(k_scr[k].name, k_scr[k].sym, JUCE_VT_PAINT,
                                et_g_paint_wrap[k], &et_g_stock_paint[k]) == 0)
                n++;
        }
        MDBG("endtrack: %d of %d screens watched for paint\n", n, ET_SCREEN_N);
    }

    {
        int k, n = 0;
        char name[32];

        if (ep122_sym(EP122_VIEW_TRANSITION)) {
            for (k = ET_TV_FIRST; k < ET_TV_SLOTS; k++) {
                snprintf(name, sizeof(name), "endtrackVt%d", k);
                if (mod_patch_vslot(name, EP122_VIEW_TRANSITION, k * 8,
                                    et_g_tv_wrap[k], &et_g_stock_tv[k]) == 0)
                    n++;
            }
            MDBG("endtrack: %d of %d view-transition slots watched\n",
                 n, ET_TV_SLOTS - ET_TV_FIRST);
        }
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
