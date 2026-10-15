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

/* And the smallest. The shape rule alone admits BADGES: a 20x30 icon is just as
 * "squarish" as a cover, and the deck draws several beside the real thing. Without
 * this the capture alternates between the cover and a badge on every frame --
 * copying 230 KB each time and leaving whichever was drawn last in the file. */
#define ART_MIN_DIM 64

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_rgb;          /* packed RGB, w*h*3        */
static size_t   g_cap;          /* what g_rgb can hold      */
static int32_t  g_w, g_h;
static uint64_t g_fp;           /* the image this copy is of */
static int      g_warned_big;

/* One image's worth of pixels, converted. Caller holds the lock. */
static int take_locked(const struct theme_bitmapdata *bd, int32_t w, int32_t h)
{
    size_t need = (size_t)w * (size_t)h * 3;
    int32_t x, y;

    if (need > ART_MAX_RGB) {
        if (!g_warned_big) {
            g_warned_big = 1;
            MDBG("theme: artwork %dx%d is %zu bytes, too big to keep\n",
                 (int)w, (int)h, need);
        }
        return -1;
    }
    if (need > g_cap) {
        uint8_t *p = realloc(g_rgb, need);

        if (!p)
            return -1;
        g_rgb = p;
        g_cap = need;
    }

    /* Memory order is B,G,R,A -- the same read the wave census makes
     * (p[2]<<16 | p[1]<<8 | p[0]), so red is p[2]. Alpha is dropped: a PNG of an
     * opaque cover does not need a channel the deck's own art does not use. */
    for (y = 0; y < h; y++) {
        const uint8_t *src = bd->data + (size_t)y * (size_t)bd->line_stride;
        uint8_t *dst = g_rgb + (size_t)y * (size_t)w * 3;

        for (x = 0; x < w; x++, src += bd->pixel_stride, dst += 3) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
    }
    g_w = w;
    g_h = h;
    return 0;
}

void theme_artwork_note(void *pd)
{
    struct theme_bitmapdata bd;
    int32_t w, h, fmt;
    uint64_t fp;

    if (!pd || *(uintptr_t *)pd != THEME_VT_SOFTPIXELDATA)
        return;
    w = *(int32_t *)((uintptr_t)pd + IPD_WIDTH_OFF);
    h = *(int32_t *)((uintptr_t)pd + IPD_HEIGHT_OFF);
    fmt = *(int32_t *)((uintptr_t)pd + IPD_PIXELFORMAT_OFF);
    if (w <= 0 || h <= 0 || (fmt != PIXFMT_RGB && fmt != PIXFMT_ARGB))
        return;

    /* The same shape rule wrap_drawimg classifies by: taller than any sprite, or
     * not strip-like. Written once, here, because the draw path only computes it
     * under debug -- and the cover is wanted on a deck with logging off. */
    if (!(h > THEME_WAVE_MAX_H || w < THEME_WAVE_ASPECT * h))
        return;
    if (!theme_bitmap(pd, w, h, &bd))
        return;

    fp = theme_fingerprint(&bd, w, h);
    if (fp == 0)
        return;

    pthread_mutex_lock(&g_lock);
    if (fp != g_fp || w != g_w || h != g_h) {
        if (take_locked(&bd, w, h) == 0) {
            g_fp = fp;
            MDBG("theme: artwork kept %dx%d (%zu bytes of RGB)\n", (int)w, (int)h,
                 (size_t)w * (size_t)h * 3);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

int theme_artwork_rgb(uint8_t *dst, size_t cap, int32_t *w, int32_t *h)
{
    size_t need;
    int got = 0;

    if (!dst || !w || !h)
        return -1;
    pthread_mutex_lock(&g_lock);
    need = (size_t)g_w * (size_t)g_h * 3;
    if (g_rgb && need > 0 && need <= cap) {
        memcpy(dst, g_rgb, need);
        *w = g_w;
        *h = g_h;
        got = (int)need;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

int theme_artwork_size(int32_t *w, int32_t *h)
{
    int have;

    if (!w || !h)
        return 0;
    pthread_mutex_lock(&g_lock);
    *w = g_w;
    *h = g_h;
    have = (g_rgb != NULL && g_w > 0 && g_h > 0);
    pthread_mutex_unlock(&g_lock);
    return have;
}
