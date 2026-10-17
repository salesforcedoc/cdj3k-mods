// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/mp4.c - the splice described in mp4.h.
 *
 * The whole job is: copy `moov`, open a hole in it where the tags belong, write
 * the tags into the hole, and tell every box that encloses the hole that it grew.
 * The one thing that needs care is that `moov` may sit BEFORE `mdat`, in which
 * case everything the file's indexes point at has moved -- so `stco`/`co64` have
 * to move with it. That fix-up is the only place this module touches bytes it was
 * not asked to add, and it is why the walk below is spelled out through the
 * container levels by name rather than searching recursively: a recursive search
 * would descend into `stsd`'s sample entries and find four-byte sequences that
 * are not boxes at all.
 *
 * Everything is bounds-checked against the length the caller gives, and anything
 * that does not add up is a refusal (-1) rather than a guess. The caller then
 * leaves the verbatim copy alone, which is the behaviour that ships today.
 */
#include "usbsave/mp4.h"

#include <string.h>

/* ---- big-endian ------------------------------------------------------------
 *
 * The format is big-endian throughout, and this file touches offsets and sizes
 * that will corrupt a file silently if read the wrong way round. */
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
}

/* Unrolled rather than a call to memcmp: the purity gate allows memcmp, but a
 * four-byte comparison the compiler can see through is what keeps this readable
 * as "same four-character code" rather than "some bytes matched". */
static int same4(const uint8_t *a, const char t[4])
{
    return a[0] == (uint8_t)t[0] && a[1] == (uint8_t)t[1] &&
           a[2] == (uint8_t)t[2] && a[3] == (uint8_t)t[3];
}

/* ---- boxes ----------------------------------------------------------------- */

/* One box header at `off`. Fills the whole box size, the header length (8, or 16
 * for the 64-bit form) and the type bytes. Fails on a size that does not add up,
 * which is the only way a chain can be walked off the end of the buffer. */
static int box_at(const uint8_t *b, uint64_t off, uint64_t limit,
                  uint64_t *size, uint64_t *hdr, const uint8_t **type)
{
    uint64_t sz;

    if (off + 8 > limit)
        return -1;
    sz = rd32(b + off);
    *hdr = 8;
    if (sz == 1) {
        if (off + 16 > limit)
            return -1;
        sz = rd64(b + off + 8);
        *hdr = 16;
    } else if (sz == 0) {
        /* "to the end of the enclosing box", legal only for the last child. The
         * only caller that can see one is the top-level walk, where the enclosing
         * box is the file. */
        sz = limit - off;
    }
    if (sz < *hdr || off + sz > limit)
        return -1;
    *size = sz;
    *type = b + off + 4;
    return 0;
}

/* The first DIRECT child of [start,end) with this type. */
static int find_child(const uint8_t *b, uint64_t start, uint64_t end,
                      const char type[4], uint64_t *off_out, uint64_t *size_out)
{
    uint64_t off = start;

    while (off + 8 <= end) {
        uint64_t size, hdr;
        const uint8_t *t;

        if (box_at(b, off, end, &size, &hdr, &t) != 0)
            return -1;
        if (same4(t, type)) {
            *off_out = off;
            *size_out = size;
            return 0;
        }
        off += size;
    }
    return -1;
}

int mod_mp4_find(const uint8_t *buf, size_t len, const char type[4],
                 struct mod_mp4_box *out)
{
    uint64_t off = 0;

    if (!buf || !out || len < 8)
        return -1;
    while (off + 8 <= len) {
        uint64_t size, hdr;
        const uint8_t *t;

        if (box_at(buf, off, len, &size, &hdr, &t) != 0)
            return -1;
        if (same4(t, type)) {
            out->off  = off;
            out->size = size;
            out->body = off + hdr;
            return 0;
        }
        off += size;
    }
    return -1;
}

/* ---- the atoms that go inside ilst ----------------------------------------- */

/* ilst child: [size][name][ [size]['data'][type][locale][payload] ] -- the shape
 * the file above carries, byte for byte. A freeform child carries two more
 * children, `mean` and `name`, ahead of its `data`: each is
 * [size][type][version/flags][string], with the four version/flags bytes zero. */
static uint64_t tag_size(const struct mod_mp4_tag *t)
{
    uint64_t n = 8 + 16 + (uint64_t)t->value_len;

    if (t->free_mean)
        n += 12 + (uint64_t)t->free_mean_len;
    if (t->free_name)
        n += 12 + (uint64_t)t->free_name_len;
    return n;
}

/* One of a freeform atom's mean/name children: [size][type][0][string]. */
static void put_free_str(uint8_t *p, const char type[4], const char *s, size_t len)
{
    wr32(p, (uint32_t)(12 + len));
    memcpy(p + 4, type, 4);
    wr32(p + 8, 0);                         /* version/flags: none */
    memcpy(p + 12, s, len);
}

static void put_tag(uint8_t *p, const struct mod_mp4_tag *t)
{
    uint8_t *q = p + 8;

    wr32(p, (uint32_t)tag_size(t));
    memcpy(p + 4, t->name, 4);
    if (t->free_mean) {
        put_free_str(q, "mean", t->free_mean, t->free_mean_len);
        q += 12 + t->free_mean_len;
    }
    if (t->free_name) {
        put_free_str(q, "name", t->free_name, t->free_name_len);
        q += 12 + t->free_name_len;
    }
    wr32(q, (uint32_t)(16 + (uint64_t)t->value_len));
    memcpy(q + 4, "data", 4);
    wr32(q + 8, (uint32_t)t->type);
    wr32(q + 12, 0);                        /* locale: none */
    memcpy(q + 16, t->value, t->value_len);
}

/* The handler every tagged file carries inside meta. Size 33 rather than the
 * spec's 32: that is what real muxers write (a trailing NUL after the reserved
 * words), and matching the file we are rewriting is worth more than matching the
 * page. */
#define META_HDLR_SIZE 33

static void put_meta_hdlr(uint8_t *p)
{
    wr32(p, META_HDLR_SIZE);
    memcpy(p + 4, "hdlr", 4);
    memset(p + 8, 0, 8);                    /* version/flags, pre_defined */
    memcpy(p + 16, "mdir", 4);              /* it is metadata            */
    memset(p + 20, 0, META_HDLR_SIZE - 20);
}

/* ---- growing the indexed samples ------------------------------------------- */

/* Add `delta` to every chunk offset inside one `stbl`'s stco/co64. Returns 0 when
 * there is nothing to fix (or it is already fixed), -1 when a 32-bit table would
 * have to overflow, which is a refusal and not a wrap. */
static int shift_stbl(uint8_t *b, uint64_t stbl_off, uint64_t stbl_size,
                      uint64_t delta)
{
    static const char *const names[] = { "stco", "co64" };
    int which;

    for (which = 0; which < 2; which++) {
        uint64_t off, size;
        int64_t n, i;
        int wide = (which == 1);

        if (find_child(b, stbl_off + 8, stbl_off + stbl_size, names[which],
                       &off, &size) != 0)
            continue;
        if (size < 16)
            return -1;
        n = (int64_t)rd32(b + off + 12);
        if ((uint64_t)(16 + (uint64_t)n * (wide ? 8u : 4u)) > size)
            return -1;
        for (i = 0; i < n; i++) {
            uint64_t at = off + 16 + (uint64_t)i * (wide ? 8u : 4u);

            if (wide) {
                wr64(b + at, rd64(b + at) + delta);
            } else {
                uint32_t v = rd32(b + at);

                if (delta > 0xffffffffu - (uint64_t)v)
                    return -1;              /* would not fit; refuse, do not wrap */
                wr32(b + at, (uint32_t)((uint64_t)v + delta));
            }
        }
    }
    return 0;
}

/* Every chunk offset in `moov` moved, because `moov` was in front of the media.
 * The path is spelled out level by level; a recursive search would walk into
 * sample entries and read box headers off the middle of a codec record. */
static int shift_chunks(uint8_t *b, uint64_t moov_size, uint64_t delta)
{
    uint64_t off = 8;

    while (off + 8 <= moov_size) {
        uint64_t size, hdr;
        const uint8_t *t;

        if (box_at(b, off, moov_size, &size, &hdr, &t) != 0)
            return -1;
        if (same4(t, "trak")) {
            uint64_t mdia, mdia_sz, minf, minf_sz, stbl, stbl_sz;

            if (find_child(b, off + hdr, off + size, "mdia", &mdia, &mdia_sz) == 0 &&
                find_child(b, mdia + 8, mdia + mdia_sz, "minf", &minf, &minf_sz) == 0 &&
                find_child(b, minf + 8, minf + minf_sz, "stbl", &stbl, &stbl_sz) == 0) {
                if (shift_stbl(b, stbl, stbl_sz, delta) != 0)
                    return -1;
            }
        }
        off += size;
    }
    return 0;
}

/* ---- the splice ------------------------------------------------------------- */

/* Is there a direct child of [start,end) of `type` whose body -- past the four
 * version/flags bytes -- is exactly `s`? This is how a freeform atom is
 * identified: its ilst id is the four dashes for EVERY freeform field, so the
 * `name` child is what actually tells them apart. */
static int child_named(const uint8_t *b, uint64_t start, uint64_t end,
                       const char type[4], const char *s, size_t len)
{
    uint64_t off = start;

    while (off + 8 <= end) {
        uint64_t size, hdr;
        const uint8_t *t;

        if (box_at(b, off, end, &size, &hdr, &t) != 0)
            return 0;
        if (same4(t, type) && size >= hdr + 4 + len &&
            memcmp(b + off + hdr + 4, s, len) == 0)
            return 1;
        off += size;
    }
    return 0;
}

int64_t mod_mp4_add_tags_why(const uint8_t *moov, size_t moov_len,
                             const struct mod_mp4_tag *tags, int ntags,
                             int moov_precedes_mdat,
                             uint8_t *out, size_t out_cap, int64_t *grew,
                             int *why, uint64_t *where)
{
    const struct mod_mp4_tag *keep[MOD_MP4_MAX_TAGS];
    uint64_t moov_size, hdr, udta = 0, udta_sz = 0, meta = 0, meta_sz = 0;
    uint64_t ilst = 0, ilst_sz = 0, insert_at, payload = 0;
    uint64_t patch[4];
    const uint8_t *t;
    int nkeep = 0, npatch = 0, have_udta, have_meta, have_ilst, i, j;
    uint8_t *hole;

    /* Every refusal below is the same -1 to the caller, and the caller's fallback is
     * the same for all of them. Naming the gate here is the only place the
     * difference survives. */
#define REFUSE(code, off) do { if (why) *why = (code); if (where) *where = (off); \
                               return -1; } while (0)

    if (why)
        *why = MOD_MP4_WHY_NONE;
    if (where)
        *where = 0;
    if (!moov || !tags || ntags <= 0 || !out)
        REFUSE(MOD_MP4_WHY_ARGS, 0);
    if (grew)
        *grew = 0;
    /* The caller hands over exactly one box. Anything else means it read the
     * wrong range, which is not something to paper over. */
    if (ntags > (int)(sizeof(keep) / sizeof(keep[0])))
        REFUSE(MOD_MP4_WHY_ARGS, (uint64_t)ntags);
    if (box_at(moov, 0, moov_len, &moov_size, &hdr, &t) != 0)
        REFUSE(MOD_MP4_WHY_MOOV_BOX, 0);
    if (!same4(t, "moov") || moov_size != moov_len)
        REFUSE(MOD_MP4_WHY_NOT_MOOV, moov_size);

    have_udta = find_child(moov, hdr, moov_size, "udta", &udta, &udta_sz) == 0;
    if (have_udta) {
        /* udta is a plain box: children start right after its header. */
        have_meta = find_child(moov, udta + 8, udta + udta_sz, "meta",
                               &meta, &meta_sz) == 0;
    } else {
        have_meta = 0;
    }
    if (have_meta) {
        /* meta is a FullBox: four bytes of version/flags before its children. */
        have_ilst = find_child(moov, meta + 12, meta + meta_sz, "ilst",
                               &ilst, &ilst_sz) == 0;
    } else {
        have_ilst = 0;
    }

    /* Gap-filling, per FIELD: a tag the file already carries is left exactly as
     * it is -- that is the "only if it is missing" rule, and it is checked here
     * because only here is the existing ilst in hand. */
    for (i = 0; i < ntags; i++) {
        if (tags[i].value_len == 0 || !tags[i].value)
            continue;
        if (have_ilst) {
            uint64_t off = ilst + 8;
            int present = 0;

            while (off + 8 <= ilst + ilst_sz) {
                uint64_t size, chdr;
                const uint8_t *ct;

                if (box_at(moov, off, ilst + ilst_sz, &size, &chdr, &ct) != 0)
                    REFUSE(MOD_MP4_WHY_ILST_CHILD, off);
                if (same4(ct, tags[i].name)) {
                    /* A freeform id is shared by every freeform field, so the id
                     * alone is not a match: the `name` child says which one. */
                    if (!tags[i].free_name ||
                        child_named(moov, off + chdr, off + size, "name",
                                    tags[i].free_name, tags[i].free_name_len))
                        present = 1;
                    if (present)
                        break;
                }
                off += size;
            }
            if (present)
                continue;
        }
        keep[nkeep++] = &tags[i];
    }
    if (nkeep == 0)
        return 0;                       /* nothing to add: not an error */

    /* Where the hole goes, and which boxes enclose it. Inserting at the END of a
     * box means every enclosing header is before the hole, so none of them move
     * and each one only needs its size field grown afterwards. */
    if (have_ilst) {
        insert_at = ilst + ilst_sz;
        patch[npatch++] = ilst;
    } else if (have_meta) {
        insert_at = meta + meta_sz;
        payload += 8;                   /* the ilst box we are creating */
    } else if (have_udta) {
        insert_at = udta + udta_sz;
        payload += 8 + 4 + META_HDLR_SIZE + 8;      /* meta + hdlr + ilst */
    } else {
        insert_at = moov_size;          /* append to moov itself          */
        payload += 8;                   /* udta                          */
        payload += 8 + 4 + META_HDLR_SIZE + 8;      /* meta + hdlr + ilst */
    }
    for (i = 0; i < nkeep; i++)
        payload += tag_size(keep[i]);

    if (have_meta)
        patch[npatch++] = meta;
    if (have_udta)
        patch[npatch++] = udta;
    patch[npatch++] = 0;                /* moov is always grown          */

    if (moov_len + payload > out_cap)
        REFUSE(MOD_MP4_WHY_OUT_CAP, moov_len + payload);

    /* Copy with the hole. memmove because the caller may have passed a buffer
     * that overlaps `moov` -- cheap insurance for a mistake made elsewhere. */
    memmove(out, moov, (size_t)insert_at);
    memmove(out + insert_at + payload, moov + insert_at,
            (size_t)(moov_size - insert_at));

    hole = out + insert_at;

    if (!have_udta) {
        wr32(hole, (uint32_t)(payload));
        memcpy(hole + 4, "udta", 4);
        hole += 8;
    }
    if (!have_meta) {
        wr32(hole, (uint32_t)(payload - (uint64_t)(hole - (out + insert_at))));
        memcpy(hole + 4, "meta", 4);
        memset(hole + 8, 0, 4);         /* version/flags */
        put_meta_hdlr(hole + 12);
        hole += 12 + META_HDLR_SIZE;
    }
    if (!have_ilst) {
        wr32(hole, (uint32_t)(payload - (uint64_t)(hole - (out + insert_at))));
        memcpy(hole + 4, "ilst", 4);
        hole += 8;
    }
    for (i = 0; i < nkeep; i++) {
        put_tag(hole, keep[i]);
        hole += tag_size(keep[i]);
    }

    /* Grow every box that encloses the hole. */
    for (i = 0; i < npatch; i++)
        wr32(out + patch[i], rd32(out + patch[i]) + (uint32_t)payload);

    /* The media moved only if moov was in front of it. */
    if (moov_precedes_mdat && payload > 0) {
        if (shift_chunks(out, moov_size + payload, payload) != 0)
            REFUSE(MOD_MP4_WHY_SHIFT, 0);
    }

    j = (int)(hole - out);              /* the writer above must land exactly */
    if ((uint64_t)j != insert_at + payload)
        REFUSE(MOD_MP4_WHY_LANDING, (uint64_t)j);
    if (grew)
        *grew = (int64_t)payload;
    return (int64_t)(moov_size + payload);
#undef REFUSE
}

/* The plain entry point, and the one the tests hold: the answer without the reason.
 * A caller that only needs to fall back gets the same behaviour it always had. */
int64_t mod_mp4_add_tags(const uint8_t *moov, size_t moov_len,
                         const struct mod_mp4_tag *tags, int ntags,
                         int moov_precedes_mdat,
                         uint8_t *out, size_t out_cap, int64_t *grew)
{
    return mod_mp4_add_tags_why(moov, moov_len, tags, ntags, moov_precedes_mdat,
                                out, out_cap, grew, NULL, NULL);
}

const char *mod_mp4_why_name(int why)
{
    switch (why) {
    case MOD_MP4_WHY_NONE:       return "none";
    case MOD_MP4_WHY_ARGS:       return "bad arguments";
    case MOD_MP4_WHY_MOOV_BOX:   return "moov header will not walk";
    case MOD_MP4_WHY_NOT_MOOV:   return "not a moov, or its size disagrees";
    case MOD_MP4_WHY_ILST_CHILD: return "a child of the existing ilst will not walk";
    case MOD_MP4_WHY_OUT_CAP:    return "caller's buffer too small";
    case MOD_MP4_WHY_SHIFT:      return "stco/co64 offsets will not rewrite";
    case MOD_MP4_WHY_LANDING:    return "writer did not land on the hole";
    }
    return "unknown";
}
