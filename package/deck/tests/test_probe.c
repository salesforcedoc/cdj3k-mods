// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_probe.c - the header reader, against headers built here byte by byte.
 *
 * The inputs are constructed rather than captured so each field's position is
 * visible in the test: a probe that reads the wrong offset passes against a
 * real file whose values happen to match at both, and fails against this.
 */
#include "mods/usbsave/probe.h"

#include "test.h"

static size_t le32_put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return 4;
}

/* A FLAC head: "fLaC", STREAMINFO, then a VORBIS_COMMENT carrying the tags. */
static size_t build_flac(uint8_t *buf, int rate, int channels, int bits)
{
    size_t at = 0;
    size_t i;
    uint64_t packed = ((uint64_t)rate << 44) |
                      ((uint64_t)(channels - 1) << 41) |
                      ((uint64_t)(bits - 1) << 36) | 1000u;
    static const char *const comments[] = { "TITLE=Hello", "ARTIST=World" };
    size_t body_len;
    size_t p;

    memcpy(buf + at, "fLaC", 4);
    at += 4;

    /* STREAMINFO: not last. */
    buf[at] = 0x00;
    buf[at + 1] = 0; buf[at + 2] = 0; buf[at + 3] = 34;
    at += 4;
    for (i = 0; i < 34; i++)
        buf[at + i] = 0;
    buf[at + 0] = 0x10; buf[at + 1] = 0x00;      /* min block */
    buf[at + 2] = 0x10; buf[at + 3] = 0x00;      /* max block */
    for (i = 0; i < 8; i++)
        buf[at + 10 + i] = (uint8_t)(packed >> (8 * (7 - i)));
    at += 34;

    /* VORBIS_COMMENT: vendor "" then two comments. */
    body_len = 4 + 0 + 4;
    for (i = 0; i < 2; i++)
        body_len += 4 + strlen(comments[i]);
    buf[at] = 0x84;                              /* type 4, last */
    buf[at + 1] = (uint8_t)(body_len >> 16);
    buf[at + 2] = (uint8_t)(body_len >> 8);
    buf[at + 3] = (uint8_t)body_len;
    at += 4;

    p = at;
    p += le32_put(buf + p, 0);                   /* vendor length */
    p += le32_put(buf + p, 2);                   /* comment count */
    for (i = 0; i < 2; i++) {
        size_t n = strlen(comments[i]);
        p += le32_put(buf + p, (uint32_t)n);
        memcpy(buf + p, comments[i], n);
        p += n;
    }
    return p;
}

static size_t build_wav(uint8_t *buf, int rate, int channels, int bits)
{
    size_t at = 0;

    memcpy(buf + at, "RIFF", 4); at += 4;
    memcpy(buf + at, "size", 4); at += 4;
    memcpy(buf + at, "WAVE", 4); at += 4;
    memcpy(buf + at, "fmt ", 4); at += 4;
    at += le32_put(buf + at, 16);
    buf[at++] = 1; buf[at++] = 0;                 /* PCM */
    buf[at++] = (uint8_t)channels; buf[at++] = 0;
    at += le32_put(buf + at, (uint32_t)rate);
    at += le32_put(buf + at, (uint32_t)(rate * channels * (bits / 8)));
    buf[at++] = (uint8_t)(channels * (bits / 8)); buf[at++] = 0;
    buf[at++] = (uint8_t)bits; buf[at++] = 0;
    memcpy(buf + at, "data", 4); at += 4;
    at += le32_put(buf + at, 0);
    return at;
}

/* An ID3v2 tag with a title, followed by one MPEG-1 Layer III frame header. */
static size_t build_mp3(uint8_t *buf)
{
    static const char body[] = "TIT2";
    size_t at = 0;
    size_t fsize = 1 + 10;                       /* encoding byte + "Some Title" */
    size_t tag_size;

    memcpy(buf + at, "ID3", 3); at += 3;
    buf[at++] = 4; buf[at++] = 0;                /* version 2.4 */
    buf[at++] = 0;                               /* flags */
    tag_size = 10 + fsize;
    /* Synchsafe length. */
    buf[at++] = (uint8_t)((tag_size >> 21) & 0x7f);
    buf[at++] = (uint8_t)((tag_size >> 14) & 0x7f);
    buf[at++] = (uint8_t)((tag_size >> 7) & 0x7f);
    buf[at++] = (uint8_t)(tag_size & 0x7f);

    memcpy(buf + at, body, 4); at += 4;
    buf[at++] = (uint8_t)(fsize >> 24); buf[at++] = (uint8_t)(fsize >> 16);
    buf[at++] = (uint8_t)(fsize >> 8);  buf[at++] = (uint8_t)fsize;
    buf[at++] = 0; buf[at++] = 0;                /* frame flags */
    buf[at++] = 0;                               /* Latin-1 */
    memcpy(buf + at, "Some Title", 10); at += 10;

    /* 0xFF 0xFB: MPEG-1 Layer III. 0x90: bitrate 9, rate index 0 -> 44100. */
    buf[at++] = 0xff; buf[at++] = 0xfb;
    buf[at++] = 0x90;
    buf[at++] = 0x00;                            /* stereo */
    return at;
}

/* An ID3v2 text frame whose body is `text` encoded the way a rekordbox stick
 * actually encodes it: encoding byte 1, a little-endian BOM, then code units.
 * Returns the new offset. */
static size_t id3_utf16_frame(uint8_t *buf, size_t at, const char *id,
                              const char *text)
{
    uint8_t body[128];
    size_t blen = 0, i;

    body[blen++] = 1;                        /* UTF-16 with BOM */
    body[blen++] = 0xff;
    body[blen++] = 0xfe;                     /* little-endian */
    for (i = 0; text[i]; i++) {
        body[blen++] = (uint8_t)text[i];
        body[blen++] = 0;
    }
    body[blen++] = 0;
    body[blen++] = 0;

    memcpy(buf + at, id, 4); at += 4;
    buf[at++] = (uint8_t)(blen >> 24); buf[at++] = (uint8_t)(blen >> 16);
    buf[at++] = (uint8_t)(blen >> 8);  buf[at++] = (uint8_t)blen;
    buf[at++] = 0; buf[at++] = 0;            /* frame flags */
    memcpy(buf + at, body, blen);
    return at + blen;
}

static size_t build_mp3_utf16(uint8_t *buf)
{
    size_t at, body_size;

    memcpy(buf, "ID3", 3);
    buf[3] = 4; buf[4] = 0; buf[5] = 0;       /* v2.4, no flags */
    at = 10;

    at = id3_utf16_frame(buf, at, "TPE1", "Palace");
    at = id3_utf16_frame(buf, at, "TIT2", "Vision (Original Mix)");

    body_size = at - 10;
    buf[6] = (uint8_t)((body_size >> 21) & 0x7f);
    buf[7] = (uint8_t)((body_size >> 14) & 0x7f);
    buf[8] = (uint8_t)((body_size >> 7) & 0x7f);
    buf[9] = (uint8_t)(body_size & 0x7f);

    buf[at++] = 0xff; buf[at++] = 0xfb;       /* MPEG-1 Layer III */
    buf[at++] = 0x90;                        /* 44100 */
    buf[at++] = 0x00;                        /* stereo */
    return at;
}

int main(void)
{
    uint8_t buf[512];
    struct mod_probe p;

    T_CASE("flac: rate, shape and tags");
    {
        size_t n = build_flac(buf, 48000, 2, 24);
        CHECK_INT(mod_probe_header(buf, n, &p), 0);
        CHECK_INT(p.rate, 48000);
        CHECK_INT(p.channels, 2);
        CHECK_INT(p.bits, 24);
        CHECK_STR(p.title, "Hello");
        CHECK_STR(p.artist, "World");
        CHECK_STR(p.album, "");
    }

    T_CASE("flac: 44.1k stereo 16-bit reads back too");
    {
        size_t n = build_flac(buf, 44100, 1, 16);
        CHECK_INT(mod_probe_header(buf, n, &p), 0);
        CHECK_INT(p.rate, 44100);
        CHECK_INT(p.channels, 1);
        CHECK_INT(p.bits, 16);
    }

    T_CASE("wav");
    {
        size_t n = build_wav(buf, 44100, 2, 16);
        CHECK_INT(mod_probe_header(buf, n, &p), 0);
        CHECK_INT(p.rate, 44100);
        CHECK_INT(p.channels, 2);
        CHECK_INT(p.bits, 16);
    }

    T_CASE("mp3: id3 tags and the first frame header");
    {
        size_t n = build_mp3(buf);
        CHECK_INT(mod_probe_header(buf, n, &p), 0);
        CHECK_STR(p.title, "Some Title");
        CHECK_INT(p.rate, 44100);
        CHECK_INT(p.channels, 2);
        /* An MP3 decodes to 16-bit whatever the source was. */
        CHECK_INT(p.bits, 16);
    }

    T_CASE("mp3: tags written UTF-16 with a BOM, as a rekordbox stick does");
    {
        size_t n = build_mp3_utf16(buf);
        CHECK_INT(mod_probe_header(buf, n, &p), 0);
        CHECK_STR(p.artist, "Palace");
        CHECK_STR(p.title, "Vision (Original Mix)");
        CHECK_INT(p.rate, 44100);
    }

    T_CASE("a truncated head is refused, not trusted");
    {
        /* 20 bytes: the block header claims 34 more, which are not there. */
        size_t n = build_flac(buf, 48000, 2, 24);
        CHECK(n > 20);
        CHECK_INT(mod_probe_header(buf, 20, &p), -1);
        CHECK_INT(p.rate, 0);
    }

    T_CASE("a head cut after STREAMINFO still reports the audio parameters");
    {
        /* 44 bytes reaches past STREAMINFO (which ends at 42) but stops short of
         * the comment block. The container IS understood -- only the tags were
         * lost -- so this succeeds with the rate intact. */
        size_t n = build_flac(buf, 48000, 2, 24);
        CHECK(n > 44);
        CHECK_INT(mod_probe_header(buf, 44, &p), 0);
        CHECK_INT(p.rate, 48000);
        CHECK_INT(p.bits, 24);
        CHECK_STR(p.title, "");
    }

    T_CASE("an unknown container leaves defaults alone");
    {
        memcpy(buf, "OggS\0\0\0\0\0\0\0\0", 12);
        CHECK_INT(mod_probe_header(buf, 12, &p), -1);
        CHECK_INT(p.rate, 0);
        CHECK_INT(p.channels, 0);
        CHECK_INT(p.bits, 0);
        CHECK_STR(p.title, "");
    }

    return t_done("probe");
}
