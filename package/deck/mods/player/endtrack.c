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

#define ET_PRINT_FIRST  40
#define ET_PRINT_EVERY  8

static uintptr_t et_g_stock_check;      /* the check body, before us */
static unsigned  et_g_n;

/* BEFORE the stock call, deliberately: the state printed is the one the check is
 * about to judge, not whatever it left behind. */
static uint64_t et_wrap_check(uintptr_t callable, uintptr_t state, uintptr_t bias)
{
    uint64_t w[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned n = ++et_g_n;

    if (n <= ET_PRINT_FIRST || n % ET_PRINT_EVERY == 0) {
        (void)mod_safe_read(state, w, sizeof(w));
        MDBG("endtrack: checkTrackEnd #%u callable=%p state=%p bias=%#lx "
             "words=%016llx %016llx %016llx %016llx %016llx %016llx %016llx "
             "%016llx\n",
             n, (void *)callable, (void *)state, (unsigned long)bias,
             (unsigned long long)w[0], (unsigned long long)w[1],
             (unsigned long long)w[2], (unsigned long long)w[3],
             (unsigned long long)w[4], (unsigned long long)w[5],
             (unsigned long long)w[6], (unsigned long long)w[7]);
    }
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
    return 0;
}

KIT_MOD(k_mod_endtrack,
        .name = "endtrack", .prio = 63, .install = et_install,
        .what = "end of track: what the deck notices, and what it does next");
