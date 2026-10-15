// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * tests/test_sqlite.c - usbsave/sqlite.c.
 *
 * The fixture is a miniature database rather than a captured one, so every byte is
 * placed where the test says it is: a 100-byte header, then pages of a size small
 * enough (512) to lay several out on the stack, each beginning with the four header
 * bytes the real thing has. A value is then written ACROSS the pages, and the test
 * asserts that after the compaction it reads as one unbroken run -- which is the
 * whole point of the module.
 */
#include "test.h"

#include "usbsave/sqlite.h"

#define PS 512
#define PAGES 3

/* A page's first four bytes, as an overflow page's next-page pointer. Recognisable
 * so a test can say "these must not appear in the payload any more". */
static const uint8_t PAGE_HDR[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

static void build_db(uint8_t *buf, size_t pages)
{
    size_t p;

    memset(buf, 0, pages * PS);
    memcpy(buf, "SQLite format 3", 16);
    buf[16] = (uint8_t)(PS >> 8);
    buf[17] = (uint8_t)PS;
    for (p = 0; p < pages; p++) {
        size_t start = p * PS + (p ? 4 : 100);   /* past the header */
        size_t i;

        for (i = start; i < (p + 1) * PS; i++)
            buf[i] = (uint8_t)('A' + (i % 26));
        if (p)
            memcpy(buf + p * PS, PAGE_HDR, 4);
    }
}

static void test_page_size(void)
{
    static uint8_t buf[PAGES * PS];
    static uint8_t other[128];

    T_CASE("page size");
    build_db(buf, PAGES);
    CHECK_U32(mod_sqlite_page_size(buf, sizeof(buf)), PS);

    /* 4096, the usual. */
    memset(other, 0, sizeof(other));
    memcpy(other, "SQLite format 3", 16);
    other[16] = 0x10; other[17] = 0x00;
    CHECK_U32(mod_sqlite_page_size(other, sizeof(other)), 4096);

    /* 1 is the format's spelling of 65536, which will not fit in the field. */
    other[16] = 0x00; other[17] = 0x01;
    CHECK_U32(mod_sqlite_page_size(other, sizeof(other)), 65536);

    T_CASE("page size: refusals");
    other[16] = 0x0B; other[17] = 0xB8;          /* 3000: not a power of two */
    CHECK_U32(mod_sqlite_page_size(other, sizeof(other)), 0);
    other[16] = 0x00; other[17] = 0x00;          /* zero */
    CHECK_U32(mod_sqlite_page_size(other, sizeof(other)), 0);
    /* Not a database at all. */
    memset(other, 'x', sizeof(other));
    CHECK_U32(mod_sqlite_page_size(other, sizeof(other)), 0);
    /* Too short to hold a header. */
    CHECK_U32(mod_sqlite_page_size((const uint8_t *)"SQLite format 3", 15), 0);
    CHECK_U32(mod_sqlite_page_size(NULL, 100), 0);
}

static void test_deoverflow(void)
{
    static uint8_t buf[PAGES * PS];
    size_t n;

    T_CASE("de-overflow");
    build_db(buf, PAGES);

    /* Where the payload of page 1 must land, and what byte it is. */
    n = mod_sqlite_deoverflow(buf, sizeof(buf), PS);

    /* Page 0 is left whole; every later page loses its four header bytes. */
    CHECK_INT((int)n, (int)(PS + (PS - 4) * (PAGES - 1)));
    /* The payload now runs unbroken: page 1's first payload byte follows page 0's
     * last, and it is the byte that was at offset PS+4 before the compaction. */
    CHECK_U32(buf[PS], (uint8_t)('A' + ((PS + 4) % 26)));
    /* And no page header survives anywhere in the compacted payload. */
    {
        size_t i, found = 0;

        for (i = 0; i + 4 <= n; i++) {
            if (buf[i] == PAGE_HDR[0] && buf[i + 1] == PAGE_HDR[1] &&
                buf[i + 2] == PAGE_HDR[2] && buf[i + 3] == PAGE_HDR[3])
                found++;
        }
        CHECK_INT((int)found, 0);
    }
}

static void test_deoverflow_edges(void)
{
    static uint8_t buf[PAGES * PS];
    size_t n;

    T_CASE("de-overflow: edges");
    build_db(buf, PAGES);
    /* A page size that is nonsense changes nothing. */
    n = mod_sqlite_deoverflow(buf, sizeof(buf), 4);
    CHECK_INT((int)n, (int)sizeof(buf));
    /* A tail shorter than a whole page is still kept, and its header dropped. */
    build_db(buf, PAGES);
    n = mod_sqlite_deoverflow(buf, PS + 8, PS);
    CHECK_INT((int)n, (int)(PS + 4));
    /* One page: nothing to do. */
    build_db(buf, PAGES);
    n = mod_sqlite_deoverflow(buf, PS, PS);
    CHECK_INT((int)n, (int)PS);
    CHECK_U32(buf[100], (uint8_t)('A' + (100 % 26)));
}

int main(void)
{
    test_page_size();
    test_deoverflow();
    test_deoverflow_edges();
    return t_done("sqlite");
}
