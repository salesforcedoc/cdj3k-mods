// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_wav.c - the WAV writer, read back field by field.
 *
 * The header is checked against the numbers a player keys off -- the data
 * length, block align and byte rate -- because those are what a deck trusts
 * when it seeks and what a wrong one breaks silently.
 */
#include "mods/usbsave/wav.h"

#include "test.h"

#define CAP    (1u << 18)
#define FRAMES 5000
#define CH     2

static uint8_t g_out[CAP];
static size_t  g_len;

static int sink(void *user, const void *buf, size_t len)
{
    (void)user;
    if (g_len + len > CAP)
        return -1;
    memcpy(g_out + g_len, buf, len);
    g_len += len;
    return 0;
}

static float g_in[FRAMES * CH];

static uint32_t le32(size_t at)
{
    return (uint32_t)g_out[at] | ((uint32_t)g_out[at + 1] << 8) |
           ((uint32_t)g_out[at + 2] << 16) | ((uint32_t)g_out[at + 3] << 24);
}

static uint32_t le16(size_t at)
{
    return (uint32_t)g_out[at] | ((uint32_t)g_out[at + 1] << 8);
}

static void fill_input(void)
{
    int i, j;

    for (i = 0; i < FRAMES; i++)
        for (j = 0; j < CH; j++)
            g_in[i * CH + j] = (float)(((i * 31 + j * 17) % 401) - 200) / 200.0f;
}

int main(void)
{
    struct mod_wav w;

    fill_input();

    T_CASE("begin");
    g_len = 0;
    CHECK_INT(mod_wav_begin(&w, sink, NULL, CH, 16, 44100, FRAMES, NULL), 0);
    CHECK_U32((unsigned long)g_len, 44);

    T_CASE("riiff header");
    CHECK(memcmp(g_out, "RIFF", 4) == 0);
    CHECK(memcmp(g_out + 8, "WAVE", 4) == 0);
    CHECK(memcmp(g_out + 12, "fmt ", 4) == 0);
    CHECK(memcmp(g_out + 36, "data", 4) == 0);
    CHECK_U32(le32(4), 36 + FRAMES * CH * 2);
    CHECK_U32(le32(16), 16);
    CHECK_U32(le16(20), 1);                      /* PCM */
    CHECK_U32(le16(22), CH);
    CHECK_U32(le32(24), 44100);
    CHECK_U32(le32(28), 44100 * CH * 2);        /* byte rate */
    CHECK_U32(le16(32), CH * 2);                /* block align */
    CHECK_U32(le16(34), 16);
    CHECK_U32(le32(40), FRAMES * CH * 2);

    T_CASE("samples");
    {
        int64_t done = 0;
        const int64_t chunks[3] = { 7, 3000, 1 };

        while (done < FRAMES) {
            int64_t n = chunks[(done / 7) % 3];
            if (done + n > FRAMES)
                n = FRAMES - done;
            CHECK_INT(mod_wav_write(&w, g_in + done * CH, n), 0);
            done += n;
        }
    }
    CHECK_INT(mod_wav_finish(&w), 0);
    CHECK_U32((unsigned long)g_len, 44 + (unsigned long)(FRAMES * CH * 2));

    T_CASE("every sample round-trips");
    {
        int i, j;
        for (i = 0; i < FRAMES; i++) {
            for (j = 0; j < CH; j++) {
                size_t at = 44 + (size_t)(i * CH + j) * 2;
                int16_t got = (int16_t)(uint16_t)le16(at);
                double scaled = (double)g_in[i * CH + j] * 32768.0;
                long long want = (long long)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
                if (want > 32767) want = 32767;
                if (want < -32768) want = -32768;
                CHECK_INT(got, want);
            }
        }
    }

    T_CASE("24-bit header");
    {
        struct mod_wav a;
        g_len = 0;
        CHECK_INT(mod_wav_begin(&a, sink, NULL, 1, 24, 48000, 10, NULL), 0);
        CHECK_U32(le16(34), 24);
        CHECK_U32(le16(32), 3);
        CHECK_U32(le32(28), 48000 * 3);
        CHECK_U32(le32(40), 30);
    }

    T_CASE("refuses what it cannot write");
    {
        struct mod_wav a;
        CHECK_INT(mod_wav_begin(&a, sink, NULL, 3, 16, 44100, 0, NULL), -1);
        CHECK_INT(mod_wav_begin(&a, sink, NULL, 2, 20, 44100, 0, NULL), -1);
        CHECK_INT(mod_wav_begin(&a, sink, NULL, 2, 16, 0, 0, NULL), -1);
    }

    T_CASE("a short finish is refused");
    {
        struct mod_wav a;
        g_len = 0;
        CHECK_INT(mod_wav_begin(&a, sink, NULL, CH, 16, 44100, 10, NULL), 0);
        CHECK_INT(mod_wav_write(&a, g_in, 4), 0);
        CHECK_INT(mod_wav_finish(&a), -1);
    }

    T_CASE("LIST/INFO tags");
    {
        struct mod_wav a;
        struct mod_wav_info info;
        size_t p;

        info.title = "Test Title";   info.title_len = 10;
        info.artist = "Test Artist"; info.artist_len = 11;
        info.album = "";             info.album_len = 0;   /* empty: omitted */

        g_len = 0;
        CHECK_INT(mod_wav_begin(&a, sink, NULL, CH, 16, 44100, FRAMES, &info), 0);

        p = 12 + 8 + 16;                     /* past "fmt " and its 16 bytes */
        CHECK(memcmp(g_out + p, "LIST", 4) == 0);
        CHECK(memcmp(g_out + p + 8, "INFO", 4) == 0);
        /* INAM is 8 + (10+1) + 1 pad = 20; IART is 8 + (11+1) = 20; plus the
         * 4-byte "INFO" form tag. */
        CHECK_U32(le32(p + 4), 44);
        CHECK(memcmp(g_out + p + 12, "INAM", 4) == 0);
        CHECK_U32(le32(p + 16), 11);        /* NUL counts */
        CHECK(memcmp(g_out + p + 20, "Test Title", 10) == 0);
        CHECK_INT(g_out[p + 30], 0);        /* the NUL */
        CHECK_INT(g_out[p + 31], 0);        /* pad, because 11 is odd */
        CHECK(memcmp(g_out + p + 32, "IART", 4) == 0);
        CHECK_U32(le32(p + 36), 12);
        CHECK(memcmp(g_out + p + 40, "Test Artist", 11) == 0);
        p += 8 + 44;

        CHECK(memcmp(g_out + p, "data", 4) == 0);
        CHECK_U32(le32(p + 4), FRAMES * CH * 2);
        /* The RIFF size covers the header AND the audio that is still to come:
         * the header is written complete, up front, so nothing has to be
         * patched later on a stick that can be pulled at any moment. */
        CHECK_U32(le32(4), (unsigned long)(g_len - 8 + FRAMES * CH * 2));
    }

    return t_done("wav");
}
