// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * tests/test_mp4.c - the tag writer in usbsave/mp4.c.
 *
 * The walker and the builders below deliberately do NOT share anything with the
 * module. A test that called the writer's own idea of where `ilst` lives would
 * agree with it about a file nobody else can read -- the same reason test_flac.c
 * carries its own CRC implementation rather than the encoder's. Here that means:
 * every expected byte is placed by the builders below, and every actual byte is
 * found by the reader below, which only knows that a box is a four-byte size and
 * a four-character type.
 *
 * The containers are built rather than captured, as in test_probe.c, so each
 * field's position is visible in the test instead of hidden in a fixture.
 */
#include "test.h"

#include <string.h>

#include "usbsave/mp4.h"

/* The test's own belief about the handler a real muxer writes inside meta: 33
 * bytes, not the spec's 32. Deliberately not taken from the module -- if the
 * writer changed its mind about this, the test should notice by parsing what came
 * out, not by agreeing with it. */
#define T_HDLR_SIZE 33

/* Tag names as raw bytes. Spelled this way rather than as "\xA9nam" literals
 * because 0xA9 is outside the range of a plain char and every compiler says so --
 * and because these are four-character codes, not C strings, in the format's own
 * terms: nothing about them is NUL-terminated. */
static const char TAG_NAM[4] = { (char)0xA9, 'n', 'a', 'm' };
static const char TAG_ART[4] = { (char)0xA9, 'A', 'R', 'T' };

/* ---- an independent reader -------------------------------------------------- */

static uint32_t g32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t g64(const uint8_t *p)
{
    return ((uint64_t)g32(p) << 32) | g32(p + 4);
}

static int is4(const uint8_t *p, const char *t)
{
    return p[0] == (uint8_t)t[0] && p[1] == (uint8_t)t[1] &&
           p[2] == (uint8_t)t[2] && p[3] == (uint8_t)t[3];
}

/* The first child of [off+8, off+size) with this type. */
static int find(const uint8_t *b, uint64_t off, uint64_t size, const char *type,
                uint64_t *o, uint64_t *s)
{
    uint64_t at = off + 8, end = off + size;

    while (at + 8 <= end) {
        uint64_t sz = g32(b + at), hdr = 8;

        if (sz == 1) {
            sz = g64(b + at + 8);
            hdr = 16;
        }
        if (sz < hdr || at + sz > end)
            return -1;
        if (is4(b + at + 4, type)) {
            *o = at;
            *s = sz;
            return 0;
        }
        at += sz;
    }
    return -1;
}

/* Every child of a container must fit, and they must between them fill it
 * exactly. This is the check that catches a size field that was grown by the
 * wrong amount -- which is the whole risk of an in-place splice. */
static int children_fit(const uint8_t *b, uint64_t off, uint64_t size, int depth)
{
    uint64_t at = off + 8, end = off + size;

    if (size < 8 || off + size > (uint64_t)1 << 20)
        return -1;
    while (at < end) {
        uint64_t sz = g32(b + at), hdr = 8;

        if (sz == 1) {
            sz = g64(b + at + 8);
            hdr = 16;
        }
        if (sz < hdr || at + sz > end)
            return -1;
        if (is4(b + at + 4, "udta") || is4(b + at + 4, "meta") ||
            is4(b + at + 4, "ilst") || is4(b + at + 4, "moov") ||
            is4(b + at + 4, "trak") || is4(b + at + 4, "mdia") ||
            is4(b + at + 4, "minf") || is4(b + at + 4, "stbl")) {
            if (is4(b + at + 4, "meta")) {
                /* FullBox: children start four bytes later. */
                if (sz < 12 || children_fit(b, at + 4, sz - 4, depth + 1) != 0)
                    return -1;
            } else if (children_fit(b, at, sz, depth + 1) != 0) {
                return -1;
            }
        }
        at += sz;
    }
    return at == end ? 0 : -1;
}

/* The payload of one ilst child, by name. -1 when it is not there. */
static int ilst_value(const uint8_t *b, uint64_t moov_off, uint64_t moov_sz,
                      const char *name, char *out, size_t cap)
{
    uint64_t udta, udta_sz, meta, meta_sz, ilst, ilst_sz, at, end;
    uint64_t dsz;

    if (find(b, moov_off, moov_sz, "udta", &udta, &udta_sz) != 0) return -1;
    if (find(b, udta, udta_sz, "meta", &meta, &meta_sz) != 0) return -1;
    if (find(b, meta + 4, meta_sz - 4, "ilst", &ilst, &ilst_sz) != 0) return -1;
    at = ilst + 8;
    end = ilst + ilst_sz;
    while (at + 8 <= end) {
        uint64_t sz = g32(b + at);

        if (sz < 8 || at + sz > end) return -1;
        if (is4(b + at + 4, name)) {
            /* [size][name][ [size]['data'][type][locale][payload] ] */
            if (sz < 24) return -1;
            dsz = g32(b + at + 8);
            if (dsz < 16 || at + 8 + dsz > at + sz) return -1;
            if (!is4(b + at + 12, "data")) return -1;
            if (dsz - 16 >= cap) return -1;
            memcpy(out, b + at + 24, dsz - 16);
            out[dsz - 16] = '\0';       /* so the caller can compare it as a string */
            return (int)(dsz - 16);
        }
        at += sz;
    }
    return -1;
}

static uint32_t ilst_type(const uint8_t *b, uint64_t moov_off, uint64_t moov_sz,
                          const char *name)
{
    uint64_t udta, udta_sz, meta, meta_sz, ilst, ilst_sz, at, end;

    if (find(b, moov_off, moov_sz, "udta", &udta, &udta_sz) != 0) return 0;
    if (find(b, udta, udta_sz, "meta", &meta, &meta_sz) != 0) return 0;
    if (find(b, meta + 4, meta_sz - 4, "ilst", &ilst, &ilst_sz) != 0) return 0;
    at = ilst + 8; end = ilst + ilst_sz;
    while (at + 8 <= end) {
        uint64_t sz = g32(b + at);

        if (sz < 8 || at + sz > end) return 0;
        if (is4(b + at + 4, name))
            return g32(b + at + 16);
        at += sz;
    }
    return 0;
}

/* The payload of the freeform ("----") child whose `name` child is `free_name`.
 * -1 when it is not there, and -1 as well on any size inside the atom that does
 * not add up -- only this reader walks far enough to see those, since
 * children_fit() descends into udta/meta/ilst and stops. */
static int ilst_freeform_value(const uint8_t *b, uint64_t moov_off, uint64_t moov_sz,
                               const char *free_name, char *out, size_t cap)
{
    uint64_t udta, udta_sz, meta, meta_sz, ilst, ilst_sz, at, end;

    if (find(b, moov_off, moov_sz, "udta", &udta, &udta_sz) != 0) return -1;
    if (find(b, udta, udta_sz, "meta", &meta, &meta_sz) != 0) return -1;
    if (find(b, meta + 4, meta_sz - 4, "ilst", &ilst, &ilst_sz) != 0) return -1;
    at = ilst + 8; end = ilst + ilst_sz;
    while (at + 8 <= end) {
        uint64_t sz = g32(b + at);

        if (sz < 8 || at + sz > end) return -1;
        if (is4(b + at + 4, "----")) {
            uint64_t io = at + 8, iend = at + sz;
            int named = 0;

            while (io + 8 <= iend) {
                uint64_t isz = g32(b + io);

                if (isz < 12 || io + isz > iend) return -1;
                if (is4(b + io + 4, "name") && isz - 12 == strlen(free_name) &&
                    memcmp(b + io + 12, free_name, isz - 12) == 0)
                    named = 1;
                if (is4(b + io + 4, "data")) {
                    if (named) {
                        if (isz < 16 || isz - 16 >= cap) return -1;
                        memcpy(out, b + io + 16, isz - 16);
                        out[isz - 16] = '\0';
                        return (int)(isz - 16);
                    }
                    break;                  /* a different freeform field: keep looking */
                }
                io += isz;
            }
        }
        at += sz;
    }
    return -1;
}

/* ---- builders --------------------------------------------------------------- */

static void p32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Open a box at `off`; returns the body offset. Close it with close_box(). */
static uint64_t open_box(uint8_t *b, uint64_t off, const char *type)
{
    memcpy(b + off + 4, type, 4);
    return off + 8;
}

static void close_box(uint8_t *b, uint64_t off, uint64_t end)
{
    p32(b + off, (uint32_t)(end - off));
}

/* stbl holding one stco with `n` offsets, so a shift has something to move. */
static uint64_t build_stbl(uint8_t *b, uint64_t off, const uint32_t *offs, int n)
{
    uint64_t stbl = off, stco, at, moov_like;
    int i;

    off = open_box(b, off, "stbl");
    stco = off;
    off = open_box(b, off, "stco");
    p32(b + off, 0);                    /* version/flags */
    p32(b + off + 4, (uint32_t)n);
    at = off + 8;
    for (i = 0; i < n; i++, at += 4)
        p32(b + at, offs[i]);
    close_box(b, stco, at);
    off = at;
    close_box(b, stbl, off);
    moov_like = off;
    return moov_like;
}

/* A `moov` with an mvhd and one trak>mdia>minf>stbl>stco. `off` is where it goes;
 * returns the offset just past it. */
static uint64_t build_moov(uint8_t *b, uint64_t off, const uint32_t *offs, int n,
                           const char *have_name, const char *have_value)
{
    uint64_t moov = off, at, mvhd, trak, mdia, minf, ilst = 0;

    off = open_box(b, off, "moov");
    if (have_name) {
        uint64_t udta = off, meta, i2;

        off = open_box(b, off, "udta");
        meta = off;
        off = open_box(b, off, "meta");
        p32(b + off, 0);
        off += 4;
        i2 = off;
        off = open_box(b, off, "hdlr");
        memset(b + off, 0, 8);
        memcpy(b + off + 8, "mdir", 4);
        memset(b + off + 12, 0, T_HDLR_SIZE - 12);
        off += T_HDLR_SIZE;
        close_box(b, i2, off);
        ilst = off;
        off = open_box(b, off, "ilst");
        /* One pre-existing tag, so gap-filling has something to leave alone. */
        {
            size_t vlen = strlen(have_value);

            p32(b + off, (uint32_t)(8 + 16 + vlen));
            memcpy(b + off + 4, have_name, 4);
            p32(b + off + 8, (uint32_t)(16 + vlen));
            memcpy(b + off + 12, "data", 4);
            p32(b + off + 16, MOD_MP4_TEXT);
            p32(b + off + 20, 0);
            memcpy(b + off + 24, have_value, vlen);
            off += 8 + 16 + vlen;
        }
        close_box(b, ilst, off);
        close_box(b, meta, off);
        close_box(b, udta, off);
    }
    mvhd = off;
    off = open_box(b, off, "mvhd");
    memset(b + off, 0, 100);
    off += 100;
    close_box(b, mvhd, off);

    trak = off;
    off = open_box(b, off, "trak");
    mdia = off;
    off = open_box(b, off, "mdia");
    minf = off;
    off = open_box(b, off, "minf");
    off = build_stbl(b, off, offs, n);
    close_box(b, minf, off);
    close_box(b, mdia, off);
    close_box(b, trak, off);

    at = off;
    close_box(b, moov, at);
    (void)at;
    return off;
}

static struct mod_mp4_tag text_tag(const char *name, const char *value)
{
    struct mod_mp4_tag t;

    memset(&t, 0, sizeof(t));           /* the freeform members are "not one" */
    memcpy(t.name, name, 4);
    t.type = MOD_MP4_TEXT;
    t.value = value;
    t.value_len = strlen(value);
    return t;
}

static struct mod_mp4_tag freeform_tag(const char *mean, const char *name,
                                       const char *value)
{
    struct mod_mp4_tag t;

    memset(&t, 0, sizeof(t));
    memcpy(t.name, "----", 4);
    t.type = MOD_MP4_TEXT;
    t.value = value;
    t.value_len = strlen(value);
    t.free_mean = mean;
    t.free_mean_len = strlen(mean);
    t.free_name = name;
    t.free_name_len = strlen(name);
    return t;
}

/* ---- the tests -------------------------------------------------------------- */

static void test_adds_tags_to_a_bare_moov(void)
{
    static uint8_t src[4096], dst[8192];
    static const uint32_t offs[3] = { 44, 512, 52674 };
    struct mod_mp4_tag tags[2];
    char got[64];
    int64_t n, grew = 0;
    uint64_t moov, moov_len;

    moov = 0;
    moov_len = build_moov(src, moov, offs, 3, NULL, NULL) - moov;

    tags[0] = text_tag(TAG_NAM, "Cream");
    tags[1] = text_tag(TAG_ART, "Prince");

    n = mod_mp4_add_tags(src, (size_t)moov_len, tags, 2, 0, dst, sizeof(dst), &grew);

    T_CASE("bare moov: tags added, sizes consistent");
    CHECK(n > 0);
    CHECK(grew > 0);
    CHECK_INT((int)n, (int)(moov_len + (uint64_t)grew));
    /* The whole point: every size field still adds up after the splice. */
    CHECK_INT(children_fit(dst, 0, (uint64_t)n, 0), 0);
    /* A bare moov has no udta, so the writer had to create udta/meta/hdlr/ilst. */
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_NAM, got, sizeof(got)),
              (int)strlen("Cream"));
    CHECK_STR(got, "Cream");
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_ART, got, sizeof(got)),
              (int)strlen("Prince"));
    CHECK_STR(got, "Prince");
    CHECK_U32(ilst_type(dst, 0, (uint64_t)n, TAG_NAM), MOD_MP4_TEXT);

    /* The handler inside meta, which a reader needs to find the tags at all. */
    {
        uint64_t udta, udta_sz, meta, meta_sz, hdlr, hdlr_sz;

        CHECK_INT(find(dst, 0, (uint64_t)n, "udta", &udta, &udta_sz), 0);
        CHECK_INT(find(dst, udta, udta_sz, "meta", &meta, &meta_sz), 0);
        CHECK_INT(find(dst, meta + 4, meta_sz - 4, "hdlr", &hdlr, &hdlr_sz), 0);
        CHECK_INT((int)hdlr_sz, T_HDLR_SIZE);
        CHECK(is4(dst + hdlr + 16, "mdir"));
    }
}

static void test_leaves_an_existing_field_alone(void)
{
    static uint8_t src[4096], dst[8192];
    static const uint32_t offs[2] = { 100, 200 };
    struct mod_mp4_tag tags[2];
    char got[64];
    int64_t n, grew = 0;
    uint64_t moov_len;

    moov_len = build_moov(src, 0, offs, 2, TAG_NAM, "OLD");

    tags[0] = text_tag(TAG_NAM, "SHOULD NOT LAND");
    tags[1] = text_tag(TAG_ART, "Prince");

    n = mod_mp4_add_tags(src, (size_t)moov_len, tags, 2, 0, dst, sizeof(dst), &grew);

    T_CASE("gap-fill: an existing field is not overwritten");
    CHECK(n > 0);
    CHECK_INT(children_fit(dst, 0, (uint64_t)n, 0), 0);
    /* The file's own title survived UNCHANGED -- the value the builder wrote, not
     * the one the caller asked for -- and the missing artist was added. */
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_NAM, got, sizeof(got)),
              (int)strlen("OLD"));
    CHECK_STR(got, "OLD");
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_ART, got, sizeof(got)),
              (int)strlen("Prince"));
    CHECK_STR(got, "Prince");
    /* Only the artist was written, so the file grew by exactly one tag atom:
     * the ilst child's own header (8) plus its `data` child (16) plus the text. */
    CHECK_INT((int)grew, (int)(8 + 16 + strlen("Prince")));
}

static void test_nothing_to_add_writes_nothing(void)
{
    static uint8_t src[4096], dst[8192];
    static const uint32_t offs[1] = { 44 };
    struct mod_mp4_tag tags[1];
    int64_t n, grew = -1;
    uint64_t moov_len;

    moov_len = build_moov(src, 0, offs, 1, TAG_NAM, "OLD");
    tags[0] = text_tag(TAG_NAM, "anything");

    n = mod_mp4_add_tags(src, (size_t)moov_len, tags, 1, 0, dst, sizeof(dst), &grew);

    T_CASE("gap-fill: nothing missing means no rewrite at all");
    CHECK_INT((int)n, 0);
    CHECK_INT((int)grew, 0);
}

static void test_shifts_chunk_offsets_when_moov_moves_the_media(void)
{
    static uint8_t src[4096], dst[8192];
    static const uint32_t offs[3] = { 44, 512, 52674 };
    struct mod_mp4_tag tags[1];
    char got[64];
    int64_t n, grew = 0;
    uint64_t moov_len, trak, trak_sz, mdia, mdia_sz, minf, minf_sz, stbl, stbl_sz;
    uint64_t stco, stco_sz;
    int i;

    moov_len = build_moov(src, 0, offs, 3, NULL, NULL);
    tags[0] = text_tag(TAG_NAM, "Cream");

    /* moov_precedes_mdat = 1: growing moov moves every sample, so every offset
     * the index holds has to move by the same amount. */
    n = mod_mp4_add_tags(src, (size_t)moov_len, tags, 1, 1, dst, sizeof(dst), &grew);

    T_CASE("moov before mdat: chunk offsets move with the media");
    CHECK(n > 0);
    CHECK(grew > 0);
    CHECK_INT(children_fit(dst, 0, (uint64_t)n, 0), 0);

    CHECK_INT(find(dst, 0, (uint64_t)n, "trak", &trak, &trak_sz), 0);
    CHECK_INT(find(dst, trak, trak_sz, "mdia", &mdia, &mdia_sz), 0);
    CHECK_INT(find(dst, mdia, mdia_sz, "minf", &minf, &minf_sz), 0);
    CHECK_INT(find(dst, minf, minf_sz, "stbl", &stbl, &stbl_sz), 0);
    CHECK_INT(find(dst, stbl, stbl_sz, "stco", &stco, &stco_sz), 0);
    CHECK_U32(g32(dst + stco + 12), 3);
    for (i = 0; i < 3; i++)
        CHECK_U32(g32(dst + stco + 16 + (uint64_t)i * 4),
                  (uint32_t)(offs[i] + (uint32_t)grew));

    /* And the tags still landed. */
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_NAM, got, sizeof(got)),
              (int)strlen("Cream"));
    CHECK_STR(got, "Cream");
}

static void test_refuses_what_it_cannot_trust(void)
{
    static uint8_t src[4096], dst[64];
    static const uint32_t offs[1] = { 44 };
    struct mod_mp4_tag tags[1];
    int64_t grew = 0;
    uint64_t moov_len;

    moov_len = build_moov(src, 0, offs, 1, NULL, NULL);
    tags[0] = text_tag(TAG_NAM, "Cream");

    T_CASE("refusals write nothing");
    /* A length that disagrees with the box's own size field. */
    CHECK_INT((int)mod_mp4_add_tags(src, (size_t)moov_len - 1, tags, 1, 0,
                                    dst, sizeof(dst), &grew), -1);
    /* Not a moov at all. */
    CHECK_INT((int)mod_mp4_add_tags(src + 8, (size_t)moov_len - 8, tags, 1, 0,
                                    dst, sizeof(dst), &grew), -1);
    /* A capacity that cannot hold the result -- refused, not truncated. */
    CHECK_INT((int)mod_mp4_add_tags(src, (size_t)moov_len, tags, 1, 0,
                                    dst, sizeof(dst), &grew), -1);
    CHECK_INT((int)grew, 0);

    T_CASE("refusals: find() on a broken chain");
    {
        struct mod_mp4_box box;
        static uint8_t junk[16] = { 0, 0, 0, 8, 'm', 'o', 'o', 'v',
                                    0, 0, 0, 200, 'm', 'd', 'a', 't' };

        /* A second box whose size runs past the buffer. */
        CHECK_INT(mod_mp4_find(junk, sizeof(junk), "mdat", &box), -1);
        CHECK_INT(mod_mp4_find(junk, sizeof(junk), "moov", &box), 0);
        CHECK_U32((uint32_t)box.size, 8);
    }

    /* And WHICH gate, because all three are the same -1 to a caller that only falls
     * back -- and the fallbacks are not interchangeable. A source this cannot parse is
     * copied verbatim with no tags and no cover, and measured 2026-10-16 that is
     * exactly what happened with nothing in the log to say which refusal it was. */
    T_CASE("refusals say which gate");
    {
        int why = -1;

        CHECK_INT((int)mod_mp4_add_tags_why(src, (size_t)moov_len - 1, tags, 1, 0,
                                            dst, sizeof(dst), &grew, &why, NULL), -1);
        CHECK_INT(why, MOD_MP4_WHY_MOOV_BOX);
        why = -1;
        CHECK_INT((int)mod_mp4_add_tags_why(src + 8, (size_t)moov_len - 8, tags, 1, 0,
                                            dst, sizeof(dst), &grew, &why, NULL), -1);
        CHECK_INT(why, MOD_MP4_WHY_NOT_MOOV);
        why = -1;
        CHECK_INT((int)mod_mp4_add_tags_why(src, (size_t)moov_len, tags, 1, 0,
                                            dst, sizeof(dst), &grew, &why, NULL), -1);
        CHECK_INT(why, MOD_MP4_WHY_OUT_CAP);
        /* The name table is part of the contract: every code has words, and a code
         * nothing knows is not silently blank. */
        CHECK_STR(mod_mp4_why_name(MOD_MP4_WHY_ILST_CHILD),
                  "a child of the existing ilst will not walk");
        CHECK(mod_mp4_why_name(9999) != NULL);
        CHECK_INT((int)grew, 0);
    }
}

static void test_a_broken_ilst_is_named_as_such(void)
{
    static uint8_t src[8192], dst[16384];
    static const uint32_t offs[1] = { 44 };
    struct mod_mp4_tag tags[1];
    uint64_t moov_len, udta, udta_sz, meta, meta_sz, ilst, ilst_sz;
    int64_t n, grew = 0;
    int why = -1;
    uint64_t at = 0;

    /* The shape that refused on the deck: a source that ALREADY carries an ilst, so
     * the gap-fill walk has to parse what is in the file before it can know which
     * fields are missing. Built by the test's own builder, and the ilst is found by
     * the test's own reader -- the module's idea of where it lives is not consulted,
     * or this would agree with it about a file nobody else can read. */
    moov_len = build_moov(src, 0, offs, 1, TAG_NAM, "Cream");
    tags[0] = text_tag(TAG_ART, "Somebody");

    T_CASE("an ilst that walks: added, and the gate reports none");
    n = mod_mp4_add_tags_why(src, (size_t)moov_len, tags, 1, 0,
                             dst, sizeof(dst), &grew, &why, &at);
    CHECK(n > 0);
    CHECK_INT(why, MOD_MP4_WHY_NONE);
    CHECK(grew > 0);

    T_CASE("an existing ilst child that will not walk is named, with its offset");
    CHECK_INT(find(src, 0, moov_len, "udta", &udta, &udta_sz), 0);
    CHECK_INT(find(src, udta, udta_sz, "meta", &meta, &meta_sz), 0);
    CHECK_INT(find(src, meta + 4, meta_sz - 4, "ilst", &ilst, &ilst_sz), 0);

    /* The first child claims more bytes than the ilst has left: the one thing that
     * walks a chain off the end of its container. */
    p32(src + ilst + 8, (uint32_t)(ilst_sz + 64));
    why = -1;
    at = 0;
    CHECK_INT((int)mod_mp4_add_tags_why(src, (size_t)moov_len, tags, 1, 0,
                                        dst, sizeof(dst), &grew, &why, &at), -1);
    CHECK_INT(why, MOD_MP4_WHY_ILST_CHILD);
    CHECK_INT((int)at, (int)(ilst + 8));
}

/* The set that was refused. Measured 2026-10-16 on the deck: a streamed track with a
 * full record AND a cover is exactly the one that reaches nine tags, and mp4.c held
 * eight -- so it was copied verbatim with no tags and no cover, and the log said only
 * "no tag rewrite". A capacity that only the best-populated tracks can exceed is a
 * bug that hides until everything works. */
static void test_the_full_tag_set_the_caller_builds(void)
{
    static uint8_t src[8192], dst[32768];
    static const uint32_t offs[1] = { 44 };
    static const uint8_t cover[6] = { 0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10 };
    static const char TAG_ALB[4] = { (char)0xA9, 'a', 'l', 'b' };
    static const char TAG_GEN[4] = { (char)0xA9, 'g', 'e', 'n' };
    static const char TAG_DAY[4] = { (char)0xA9, 'd', 'a', 'y' };
    static const char TAG_CMT[4] = { (char)0xA9, 'c', 'm', 't' };
    static const char TAG_WRT[4] = { (char)0xA9, 'w', 'r', 't' };
    static const char URL[] = "https://geo-media.beatport.com/image_size/1400x1400/52acad8e.jpg";
    struct mod_mp4_tag tags[MOD_MP4_MAX_TAGS];
    uint64_t moov_len;
    char got[256];
    int64_t n, grew = 0;
    int why = -1;

    moov_len = build_moov(src, 0, offs, 1, NULL, NULL);
    tags[0] = text_tag(TAG_NAM, "Too Deep (Original Mix)");
    tags[1] = text_tag(TAG_ART, "Formula");
    tags[2] = text_tag(TAG_ALB, "Too Deep");
    tags[3] = text_tag(TAG_GEN, "140 / Deep Dubstep / Grime");
    tags[4] = text_tag(TAG_DAY, "2026");
    tags[5] = text_tag("tmpo", "140");
    tags[6] = freeform_tag("com.apple.iTunes", "initialkey", "10B");
    tags[7] = text_tag(TAG_CMT, URL);
    tags[8] = text_tag(TAG_WRT, URL);
    memset(&tags[9], 0, sizeof(tags[9]));
    memcpy(tags[9].name, "covr", 4);
    tags[9].type = MOD_MP4_JPEG;
    tags[9].value = (const char *)cover;
    tags[9].value_len = sizeof(cover);

    T_CASE("the caller's full ten-tag set: accepted, and every field lands");
    n = mod_mp4_add_tags_why(src, (size_t)moov_len, tags, 10, 0,
                             dst, sizeof(dst), &grew, &why, NULL);
    CHECK(n > 0);
    CHECK_INT(why, MOD_MP4_WHY_NONE);
    CHECK(grew > 0);
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_NAM, got, sizeof(got)),
              (int)strlen("Too Deep (Original Mix)"));
    CHECK_STR(got, "Too Deep (Original Mix)");
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_GEN, got, sizeof(got)),
              (int)strlen("140 / Deep Dubstep / Grime"));
    CHECK_STR(got, "140 / Deep Dubstep / Grime");
    /* The URL, in BOTH fields it is written to: the two are what different readers
     * show, and a file carrying only one is a file half the readers cannot point at
     * the larger artwork from. */
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_CMT, got, sizeof(got)),
              (int)strlen(URL));
    CHECK_STR(got, URL);
    CHECK_INT(ilst_value(dst, 0, (uint64_t)n, TAG_WRT, got, sizeof(got)),
              (int)strlen(URL));
    CHECK_STR(got, URL);
    CHECK_INT(ilst_type(dst, 0, (uint64_t)n, "covr"), MOD_MP4_JPEG);
    CHECK_INT(ilst_freeform_value(dst, 0, (uint64_t)n, "initialkey",
                                  got, sizeof(got)),
              (int)strlen("10B"));
    CHECK_STR(got, "10B");

    T_CASE("past the capacity it is refused, by name");
    /* One more than the limit: the check is before any tag is read, so naming a
     * count this array cannot hold is safe and is exactly what the guard is for. */
    why = -1;
    CHECK_INT((int)mod_mp4_add_tags_why(src, (size_t)moov_len, tags,
                                        MOD_MP4_MAX_TAGS + 1, 0,
                                        dst, sizeof(dst), &grew, &why, NULL), -1);
    CHECK_INT(why, MOD_MP4_WHY_ARGS);
}

static void test_freeform_atom(void)
{
    static uint8_t src[8192], dst[16384];
    static const uint32_t offs[1] = { 44 };
    struct mod_mp4_tag tags[2];
    char got[64];
    int64_t n, grew = 0;
    uint64_t moov_len;

    moov_len = build_moov(src, 0, offs, 1, NULL, NULL);
    tags[0] = text_tag(TAG_NAM, "Cream");
    tags[1] = freeform_tag("com.apple.iTunes", "initialkey", "10A");

    n = mod_mp4_add_tags(src, (size_t)moov_len, tags, 2, 0, dst, sizeof(dst), &grew);

    T_CASE("freeform: a key rides in ----/mean/name");
    CHECK(n > 0);
    CHECK_INT(children_fit(dst, 0, (uint64_t)n, 0), 0);
    CHECK_INT(ilst_freeform_value(dst, 0, (uint64_t)n, "initialkey", got, sizeof(got)),
              (int)strlen("10A"));
    CHECK_STR(got, "10A");
    /* Growth: the container the writer had to create (udta 8, meta 8+4, hdlr 33,
     * ilst 8), then the ordinary tag (8+16+5), then the freeform child's own
     * header (8) and its three children -- mean (12+16), name (12+10), data
     * (16+3). */
    CHECK_INT((int)grew, (int)((8 + 8 + 4 + T_HDLR_SIZE + 8) +
                               (8 + 16 + strlen("Cream")) +
                               (8 + (12 + strlen("com.apple.iTunes")) +
                                (12 + strlen("initialkey")) +
                                (16 + strlen("10A")))));
}

static void test_freeform_gap_fill(void)
{
    static uint8_t a[16384], b[16384];
    static const uint32_t offs[1] = { 44 };
    struct mod_mp4_tag tag;
    char got[64];
    int64_t n1, n2, grew = 0;
    uint64_t moov_len;

    moov_len = build_moov(a, 0, offs, 1, NULL, NULL);
    tag = freeform_tag("com.apple.iTunes", "initialkey", "10A");
    n1 = mod_mp4_add_tags(a, (size_t)moov_len, &tag, 1, 0, b, sizeof(b), &grew);
    CHECK(n1 > 0);

    T_CASE("freeform: the same field is left alone");
    /* Feed the result back in: that field is already there. */
    n2 = mod_mp4_add_tags(b, (size_t)n1, &tag, 1, 0, a, sizeof(a), &grew);
    CHECK_INT((int)n2, 0);
    CHECK_INT((int)grew, 0);

    T_CASE("freeform: a different field in the same file is added");
    /* The ilst id is shared by every freeform field, so the `name` is what tells
     * them apart: a second one IS written. */
    tag = freeform_tag("com.apple.iTunes", "ISRC", "QZWA32202168");
    n2 = mod_mp4_add_tags(b, (size_t)n1, &tag, 1, 0, a, sizeof(a), &grew);
    CHECK(n2 > n1);
    CHECK_INT(children_fit(a, 0, (uint64_t)n2, 0), 0);
    CHECK_INT(ilst_freeform_value(a, 0, (uint64_t)n2, "ISRC", got, sizeof(got)),
              (int)strlen("QZWA32202168"));
    CHECK_STR(got, "QZWA32202168");
    CHECK_INT(ilst_freeform_value(a, 0, (uint64_t)n2, "initialkey", got, sizeof(got)),
              (int)strlen("10A"));
    CHECK_STR(got, "10A");
}

int main(void)
{
    test_adds_tags_to_a_bare_moov();
    test_leaves_an_existing_field_alone();
    test_nothing_to_add_writes_nothing();
    test_shifts_chunk_offsets_when_moov_moves_the_media();
    test_refuses_what_it_cannot_trust();
    test_a_broken_ilst_is_named_as_such();
    test_the_full_tag_set_the_caller_builds();
    test_freeform_atom();
    test_freeform_gap_fill();
    return t_done("mp4");
}
