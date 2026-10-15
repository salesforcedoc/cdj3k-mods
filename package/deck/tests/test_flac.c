// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_flac.c - the verbatim FLAC writer, read back by a parser written here.
 *
 * The parser below deliberately does NOT share the encoder's checksums or its
 * quantiser. A test that called the encoder's own crc16() would pass just as
 * happily with a wrong polynomial; this one carries its own, so the two have
 * to agree on the bytes rather than on a helper.
 *
 * What this cannot check is that the deck's reader accepts the file -- that
 * needs EP122 and a stick. What it pins is the structure the deck's reader
 * walks: the marker, STREAMINFO, the tag block, and frame headers whose block
 * bounds and CRCs are self-consistent.
 */
#include "mods/usbsave/flac.h"

#include "test.h"

#define CAP (1u << 20)

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

/* ---- an independent reader ------------------------------------------------- */

struct br {
    const uint8_t *p;
    size_t         bit;   /* read cursor, in bits */
};

static uint32_t br_get(struct br *r, unsigned n)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < n; i++) {
        size_t b = r->bit + i;
        v = (v << 1) | ((r->p[b >> 3] >> (7 - (b & 7))) & 1u);
    }
    r->bit += n;
    return v;
}

static size_t br_byte(struct br *r)
{
    return r->bit >> 3;
}

/* The UTF-8-like frame number: a run of leading ones counts the bytes. */
static uint32_t br_frame_number(struct br *r)
{
    uint32_t b0 = br_get(r, 8);
    unsigned lead = 0, i;
    uint32_t v;

    while (lead < 8 && (b0 & (0x80u >> lead)))
        lead++;
    if (lead == 0)
        return b0;
    if (lead == 1 || lead == 8)
        return 0xffffffffu;
    v = b0 & ((1u << (7 - lead)) - 1);
    for (i = 0; i < lead - 1; i++)
        v = (v << 6) | (br_get(r, 8) & 0x3fu);
    return v;
}

static uint8_t t_crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

static uint16_t t_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

/* ---- input ----------------------------------------------------------------- */

#define FRAMES 5000
#define CH     2

static float g_in[FRAMES * CH];

/* A signal with the awkward cases in it: full-scale both ways, exact zero, and
 * a value that must round rather than truncate. */
static void fill_input(void)
{
    int i;

    for (i = 0; i < FRAMES; i++) {
        int j;

        for (j = 0; j < CH; j++) {
            switch (i % 5) {
            case 0: g_in[i * CH + j] = 0.0f; break;
            case 1: g_in[i * CH + j] = 1.0f; break;
            case 2: g_in[i * CH + j] = -1.0f; break;
            case 3: g_in[i * CH + j] = (float)((i + j) & 1) ? 0.5f : -0.5f; break;
            default: g_in[i * CH + j] = (float)(i % 97) / 200.0f - 0.25f; break;
            }
        }
    }
}

/* Independent of the encoder's: round half away from zero, then clamp. */
static int32_t expect(float v, int bits)
{
    double s = (double)v * (double)(1u << (bits - 1));
    long long q = (long long)(s >= 0 ? s + 0.5 : s - 0.5);
    long long lo = -((long long)1 << (bits - 1));
    long long hi = ((long long)1 << (bits - 1)) - 1;

    if (q < lo) q = lo;
    if (q > hi) q = hi;
    return (int32_t)q;
}

int main(void)
{
    struct mod_flac f;
    struct mod_flac_tags tags;
    struct br r;
    int64_t frames_seen = 0;
    unsigned frame_count = 0;
    size_t at;

    /* ---- encode, in deliberately ragged chunks ---- */
    {
        static const struct mod_flac_tag items[] = {
            { "TITLE",     5,  "Test Title",  10 },
            { "ARTIST",    6,  "Test Artist", 11 },
            { "ALBUM",     5,  "",            0  },   /* empty: omitted */
            { "COMMENT",   7,  NULL,          0  },
            { "BPM",       3,  "127.00",      6  },
            { "INITIALKEY", 10, "8A",         2  },
        };

        tags.vendor     = NULL;              /* the built-in vendor string */
        tags.vendor_len = 0;
        tags.items      = items;
        tags.count      = sizeof(items) / sizeof(items[0]);
    }

    g_len = 0;
    T_CASE("begin");
    {
        int rc = mod_flac_begin(&f, sink, NULL, CH, 16, 44100, FRAMES, NULL, &tags);
        CHECK_INT(rc, 0);
    }

    fill_input();
    T_CASE("write in ragged chunks");
    {
        int64_t done = 0;
        const int64_t chunks[3] = { 1, 4095, 3 };

        while (done < FRAMES) {
            int64_t n = chunks[frame_count % 3];
            if (done + n > FRAMES)
                n = FRAMES - done;
            CHECK_INT(mod_flac_write(&f, g_in + done * CH, n), 0);
            done += n;
            frame_count++;
        }
    }

    T_CASE("finish");
    CHECK_INT(mod_flac_finish(&f), 0);
    CHECK(g_len > 44);

    frame_count = 0;

    /* ---- marker and STREAMINFO ---- */
    T_CASE("marker");
    CHECK(memcmp(g_out, "fLaC", 4) == 0);

    T_CASE("streaminfo header");
    CHECK(g_len >= 8 + 34);
    CHECK_INT(g_out[4] & 0x7f, 0);          /* type 0 */
    CHECK_INT(g_out[4] & 0x80, 0);          /* not last: the tags follow */
    CHECK_INT((g_out[5] << 16) | (g_out[6] << 8) | g_out[7], 34);

    T_CASE("streaminfo fields");
    CHECK_INT((g_out[8] << 8) | g_out[9], MOD_FLAC_BLOCK);
    CHECK_INT((g_out[10] << 8) | g_out[11], MOD_FLAC_BLOCK);
    {
        /* rate(20) channels-1(3) bits-1(5) total(36), packed into 64 bits. */
        uint64_t packed = ((uint64_t)g_out[18] << 56) | ((uint64_t)g_out[19] << 48) |
                          ((uint64_t)g_out[20] << 40) | ((uint64_t)g_out[21] << 32) |
                          ((uint64_t)g_out[22] << 24) | ((uint64_t)g_out[23] << 16) |
                          ((uint64_t)g_out[24] << 8)  | (uint64_t)g_out[25];
        uint32_t rate  = (uint32_t)(packed >> 44);
        unsigned chan  = (unsigned)((packed >> 41) & 0x7);
        unsigned bits  = (unsigned)((packed >> 36) & 0x1f);
        uint64_t total = packed & 0xfffffffffull;

        CHECK_U32(rate, 44100);
        CHECK_INT(chan + 1, CH);
        CHECK_INT(bits + 1, 16);
        CHECK_U32((unsigned long)total, FRAMES);
    }
    {
        int i, zero = 1;
        for (i = 0; i < 16; i++)
            if (g_out[26 + i]) zero = 0;
        T_CASE("md5 unknown when none supplied");
        CHECK_INT(zero, 1);
    }

    /* ---- tags ---- */
    /* "fLaC" (4) + block header (4) + STREAMINFO (34) puts the second block
     * header at 42 and its body at 46. */
    T_CASE("vorbis comment");
    CHECK_INT(g_out[42] & 0x7f, 4);          /* type 4 */
    CHECK_INT(g_out[42] & 0x80, 0x80);       /* last metadata block */
    {
        size_t len = ((size_t)g_out[43] << 16) | ((size_t)g_out[44] << 8) | g_out[45];
        size_t p = 46;
        size_t vendor_len, count, i;
        int found_title = 0, found_artist = 0, found_bpm = 0, found_empty_album = 0;

        vendor_len = (size_t)g_out[p] | ((size_t)g_out[p + 1] << 8) |
                     ((size_t)g_out[p + 2] << 16) | ((size_t)g_out[p + 3] << 24);
        p += 4 + vendor_len;
        count = (size_t)g_out[p] | ((size_t)g_out[p + 1] << 8);
        p += 4;

        for (i = 0; i < count; i++) {
            size_t clen = (size_t)g_out[p] | ((size_t)g_out[p + 1] << 8);
            p += 4;
            if (clen == 16 && memcmp(g_out + p, "TITLE=Test Title", 16) == 0)
                found_title = 1;
            if (clen == 18 && memcmp(g_out + p, "ARTIST=Test Artist", 18) == 0)
                found_artist = 1;
            if (clen == 10 && memcmp(g_out + p, "BPM=127.00", 10) == 0)
                found_bpm = 1;
            if (clen >= 6 && memcmp(g_out + p, "ALBUM=", 6) == 0)
                found_empty_album = 1;
            p += clen;
        }
        CHECK_INT(p, 46 + len);
        CHECK_INT(found_title, 1);
        CHECK_INT(found_artist, 1);
        CHECK_INT(found_bpm, 1);
        /* An empty field is omitted, not written blank. */
        CHECK_INT(found_empty_album, 0);
        at = 46 + len;                       /* first frame starts here */
    }

    /* ---- frames ---- */
    T_CASE("frames");
    r.p = g_out;
    r.bit = at * 8;
    for (;;) {
        size_t start_byte = br_byte(&r);
        size_t header_end;
        uint32_t block, ch;
        int64_t i;

        if (start_byte + 8 > g_len)
            break;
        if (br_get(&r, 14) != 0x3ffe)
            break;                           /* ran off the end: done */

        CHECK_INT(br_get(&r, 1), 0);         /* reserved */
        CHECK_INT(br_get(&r, 1), 0);         /* fixed block size */
        CHECK_INT(br_get(&r, 4), 0x7);        /* 16-bit block size literal */
        CHECK_INT(br_get(&r, 4), 0x0);        /* rate from STREAMINFO */
        CHECK_INT(br_get(&r, 4), CH - 1);    /* independent channels */
        CHECK_INT(br_get(&r, 3), 0x0);        /* depth from STREAMINFO */
        CHECK_INT(br_get(&r, 1), 0);          /* reserved */
        CHECK_U32(br_frame_number(&r), frame_count);
        block = br_get(&r, 16) + 1;
        r.bit = ((r.bit + 7) / 8) * 8;        /* pad to the CRC byte */
        header_end = br_byte(&r);

        CHECK_U32(t_crc8(g_out + start_byte, header_end - start_byte),
                  g_out[header_end]);
        r.bit += 8;

        CHECK(block > 0 && block <= MOD_FLAC_BLOCK);
        CHECK_U32(frames_seen + block <= FRAMES ? block : (uint32_t)(FRAMES - frames_seen),
                  block);

        for (ch = 0; ch < CH; ch++) {
            int32_t got;

            CHECK_INT(br_get(&r, 1), 0);
            CHECK_U32(br_get(&r, 6), 0x1);    /* verbatim */
            CHECK_INT(br_get(&r, 1), 0);
            for (i = 0; i < (int64_t)block; i++) {
                got = (int32_t)(int16_t)br_get(&r, 16);
                CHECK_INT(got, expect(g_in[(frames_seen + i) * CH + ch], 16));
            }
        }
        r.bit = ((r.bit + 7) / 8) * 8;
        {
            size_t end = br_byte(&r);
            uint16_t want = t_crc16(g_out + start_byte, end - start_byte);
            uint16_t have = (uint16_t)((g_out[end] << 8) | g_out[end + 1]);

            CHECK_U32(want, have);
            r.bit += 16;
        }

        frames_seen += block;
        frame_count++;
        if (frames_seen >= FRAMES)
            break;
    }
    CHECK_U32((unsigned long)frames_seen, FRAMES);
    /* 5000 frames at a 4096 block is one full frame and one short one. */
    CHECK_U32(frame_count, 2);
    CHECK_U32(br_byte(&r), g_len);

    T_CASE("short finish is refused");
    {
        struct mod_flac g;
        g_len = 0;
        CHECK_INT(mod_flac_begin(&g, sink, NULL, CH, 16, 44100, 10, NULL, NULL), 0);
        CHECK_INT(mod_flac_write(&g, g_in, 4), 0);
        CHECK_INT(mod_flac_finish(&g), -1);  /* promised 10, got 4 */
    }

    T_CASE("refuses what it cannot encode");
    {
        struct mod_flac g;
        CHECK_INT(mod_flac_begin(&g, sink, NULL, 3, 16, 44100, 0, NULL, NULL), -1);
        CHECK_INT(mod_flac_begin(&g, sink, NULL, 2, 20, 44100, 0, NULL, NULL), -1);
        CHECK_INT(mod_flac_begin(&g, sink, NULL, 2, 16, 0, 0, NULL, NULL), -1);
    }

    return t_done("flac");
}
