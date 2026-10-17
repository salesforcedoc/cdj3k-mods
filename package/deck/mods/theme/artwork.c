// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * theme/artwork.c - keep a copy of the album art the deck draws.
 *
 * USB Save wants the cover for the file it writes, and this is the only place it
 * can be had. The compressed image the deck fetched is inside its address space
 * with no reader, no symbol and no file: the log shows the only path the deck
 * opens under /mnt/StreamingTemp is the track itself, no .jpg or .png anywhere,
 * and yet it draws the art -- so nothing on disk holds it.
 *
 * What IS reachable is the bitmap juce hands to drawImage, at the size the deck
 * displays it: 320x240 on EP122-3.19. That is a re-render rather than the cover
 * file, and this module says so rather than pretending otherwise; 320x240 is
 * still a real image, and it is the difference between a saved track with art and
 * one without.
 *
 * A COPY, not a reference: juce frees and reuses ImagePixelData, so a held
 * pointer is a pointer to whatever sprite lands there next. And the copy is made
 * once per distinct image -- keyed on the fingerprint rather than on the buffer's
 * address, because the address is exactly the thing that gets reused, and a
 * same-address same-size different-cover would otherwise write the previous
 * track's art onto the next track's file.
 *
 * Captured on the render thread, read by USB Save's worker: the lock covers both.
 */
#include "theme/artwork.h"
#include "theme/image_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The largest cover worth taking. 320x240 RGB is 230 KB; this leaves room for a
 * deck that renders bigger without letting a misclassified image eat the heap. */
#define ART_MAX_RGB (1024 * 1024)

/* The two sizes worth keeping, and the order they are wanted in.
 *
 * The deck draws a cover at whatever size the spot showing it uses, and the sizes
 * measured on the deck are 320x240, 240x240, 198x198, 180x180, 150x150 and 140x140 --
 * different parts of the UI, and the smaller ones belong to rows listing OTHER tracks.
 * The user's rule, 2026-10-16: prefer the 240x240 when there is one and fall back to
 * the larger 320x240 when there is not. 240x240 is SQUARE, the artwork's own aspect;
 * the player view's 320x240 is stretched to 4:3.
 *
 * Everything else is dropped, and the BADGE that once made a saved cover come out as a
 * numeral is why this is a set of exact sizes rather than a size floor: measured, the
 * deck draws it at 20x30 through the same call as the artwork.
 *
 * Which of the two is right for a given track is NOT knowable from here -- the browse
 * view draws artwork for whatever row is highlighted, not for the track being saved --
 * so the chosen image is dumped where it can be LOOKED at (usbsave/track_save.c)
 * instead of being trusted. */
#define ART_NEAR_W 240
#define ART_NEAR_H 240
#define ART_FAR_W  320
#define ART_FAR_H  240
#define ART_KINDS  2

/* One kept image. Two of these, because the two sizes are wanted in a fixed order and
 * a single slot would let a 240x240 from an earlier track block every 320x240 after
 * it -- one stale picture winning forever, which is the failure this file exists to
 * avoid. */
struct art_slot {
    uint8_t *rgb;               /* packed RGB, w*h*3, or NULL */
    size_t   cap;               /* what rgb can hold */
    int32_t  w, h;
    uint64_t fp;                /* the image this copy is of */
    int      warned_big;
};

static struct art_slot g_slot[ART_KINDS];      /* 0 = 240x240, 1 = 320x240 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Which slot a drawn image belongs in, or -1 when it is neither of the two. */
static int slot_for(int32_t w, int32_t h)
{
    if (w == ART_NEAR_W && h == ART_NEAR_H)
        return 0;
    if (w == ART_FAR_W && h == ART_FAR_H)
        return 1;
    return -1;
}

/* One image's worth of pixels, converted. Caller holds the lock. */
static int take_locked(int kind, const struct theme_bitmapdata *bd, int32_t w, int32_t h)
{
    struct art_slot *s = &g_slot[kind];
    size_t need = (size_t)w * (size_t)h * 3;
    int32_t x, y;

    if (need > ART_MAX_RGB) {
        if (!s->warned_big) {
            s->warned_big = 1;
            MDBG("theme: artwork %dx%d is %zu bytes, too big to keep\n",
                 (int)w, (int)h, need);
        }
        return -1;
    }
    if (need > s->cap) {
        uint8_t *p = realloc(s->rgb, need);

        if (!p)
            return -1;
        s->rgb = p;
        s->cap = need;
    }

    /* Memory order is B,G,R,A -- the same read the wave census makes
     * (p[2]<<16 | p[1]<<8 | p[0]), so red is p[2]. Alpha is dropped: a PNG of an
     * opaque cover does not need a channel the deck's own art does not use. */
    for (y = 0; y < h; y++) {
        const uint8_t *src = bd->data + (size_t)y * (size_t)bd->line_stride;
        uint8_t *dst = s->rgb + (size_t)y * (size_t)w * 3;

        for (x = 0; x < w; x++, src += bd->pixel_stride, dst += 3) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
    }
    s->w = w;
    s->h = h;
    return 0;
}

/* Sizes the deck draws that are neither of the two worth keeping, said once each.
 *
 * BOUNDED, and that matters more than the list: this runs on the RENDER path, and
 * MDBG is fprintf + fflush. The first version logged whenever the lookup missed,
 * which after the table filled was every single draw -- 115 lines in sixteen seconds,
 * on the one path this project has measured hanging EP122. Once the table is full it
 * now says nothing at all, so the worst case is eight lines for the whole boot. */
static void note_unwanted(int32_t w, int32_t h)
{
    static int32_t seen_w[8], seen_h[8];
    static int n;
    int i;

    for (i = 0; i < n; i++) {
        if (seen_w[i] == w && seen_h[i] == h)
            return;
    }
    if (n >= (int)(sizeof(seen_w) / sizeof(seen_w[0])))
        return;                         /* enough seen: quiet from here on */
    seen_w[n] = w;
    seen_h[n] = h;
    n++;
    MDBG("theme: image %dx%d drawn, not one of the two cover sizes\n",
         (int)w, (int)h);
}

void theme_artwork_note(void *pd)
{
    struct theme_bitmapdata bd;
    int32_t w, h, fmt;
    uint64_t fp;
    int kind;

    if (!pd || *(uintptr_t *)pd != THEME_VT_SOFTPIXELDATA)
        return;
    w = *(int32_t *)((uintptr_t)pd + IPD_WIDTH_OFF);
    h = *(int32_t *)((uintptr_t)pd + IPD_HEIGHT_OFF);
    fmt = *(int32_t *)((uintptr_t)pd + IPD_PIXELFORMAT_OFF);
    if (w <= 0 || h <= 0 || (fmt != PIXFMT_RGB && fmt != PIXFMT_ARGB))
        return;

    /* The two sizes worth keeping, or nothing. This replaces a size floor and a
     * shape rule that together admitted everything from a 20x30 badge up to a
     * 320x240 cover and left whichever was drawn last in the file. */
    kind = slot_for(w, h);
    if (kind < 0) {
        note_unwanted(w, h);
        return;
    }
    if (!theme_bitmap(pd, w, h, &bd))
        return;

    fp = theme_fingerprint(&bd, w, h);
    if (fp == 0)
        return;

    pthread_mutex_lock(&g_lock);
    if (fp != g_slot[kind].fp) {
        if (take_locked(kind, &bd, w, h) == 0) {
            g_slot[kind].fp = fp;
            MDBG("theme: artwork kept %dx%d%s (%zu bytes of RGB)\n", (int)w, (int)h,
                 kind == 0 ? " (preferred)" : " (fallback)",
                 (size_t)w * (size_t)h * 3);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

int theme_artwork_rgb(uint8_t *dst, size_t cap, int32_t *w, int32_t *h)
{
    size_t need;
    int k, got = 0;

    if (!dst || !w || !h)
        return -1;
    pthread_mutex_lock(&g_lock);
    /* In order: the 240x240 when there is one, else the 320x240. */
    for (k = 0; k < ART_KINDS && got == 0; k++) {
        const struct art_slot *s = &g_slot[k];

        need = (size_t)s->w * (size_t)s->h * 3;
        if (s->rgb && need > 0 && need <= cap) {
            memcpy(dst, s->rgb, need);
            *w = s->w;
            *h = s->h;
            got = (int)need;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

int theme_artwork_size(int32_t *w, int32_t *h)
{
    int k, have = 0;

    if (!w || !h)
        return 0;
    pthread_mutex_lock(&g_lock);
    for (k = 0; k < ART_KINDS && !have; k++) {
        if (g_slot[k].rgb && g_slot[k].w > 0 && g_slot[k].h > 0) {
            *w = g_slot[k].w;
            *h = g_slot[k].h;
            have = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return have;
}
