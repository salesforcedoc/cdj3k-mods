// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * tests/test_sqlite.c - usbsave/sqlite.c.
 *
 * Against a REAL database (tests/sqlite_fixture.h, made by mk_sqlite_fixture.py)
 * rather than a hand-built page: the thing being tested is where a value's bytes
 * actually live, and a fixture built by hand would encode the same misunderstanding
 * the reader might have. The second row's value is longer than its 512-byte page
 * can hold, so it can only be returned correctly by following the overflow chain.
 */
#include "test.h"

#include "sqlite_fixture.h"
#include "usbsave/sqlite.h"

/* One whole record while it is decoded; the fixture's largest is ~1.5 KB. */
static uint8_t scratch[64 * 1024];

/* The names a lookup needs, with their lengths: the reader takes lengths rather
 * than calling strlen, which is not in the mods' libc subset. At a call site these
 * are compile-time constants. */
#define Q(table, idcol, col, want) \
    { table, sizeof(table) - 1, idcol, sizeof(idcol) - 1, col, sizeof(col) - 1, want }

static int find(long id, const char *column, char *out, size_t cap, size_t *len)
{
    struct mod_sqlite_query q;

    q.table = "tracks";
    q.table_len = sizeof("tracks") - 1;
    q.id_column = "id";
    q.id_column_len = sizeof("id") - 1;
    q.column = column;
    q.column_len = strlen(column);
    q.id = id;
    return mod_sqlite_find_text(SQLITE_FIXTURE, SQLITE_FIXTURE_LEN, &q,
                                scratch, sizeof(scratch), out, cap, len);
}

static void test_page_size(void)
{
    static const uint8_t notdb[1024] = { 'x' };

    T_CASE("page size");
    CHECK_U32(mod_sqlite_page_size(SQLITE_FIXTURE, SQLITE_FIXTURE_LEN), 512);
    CHECK_U32(mod_sqlite_page_size(notdb, sizeof(notdb)), 0);
    CHECK_U32(mod_sqlite_page_size(SQLITE_FIXTURE, 99), 0);
    CHECK_U32(mod_sqlite_page_size(NULL, 1024), 0);
}

static void test_rows(void)
{
    char out[8192];
    size_t len = 0;

    T_CASE("every row, by id");
    /* Row 1 fits on its page. */
    CHECK_INT(find(1, "json", out, sizeof(out), &len), 1);
    CHECK_STR(out, SQLITE_FIXTURE_JSON_1);
    CHECK_INT((int)len, (int)strlen(SQLITE_FIXTURE_JSON_1));

    /* Row 3 also fits. */
    CHECK_INT(find(3, "json", out, sizeof(out), &len), 1);
    CHECK_STR(out, SQLITE_FIXTURE_JSON_3);

    T_CASE("a value that overflows its page");
    /* Row 2 is the point of the fixture: 1.5 KB of text on a 512-byte page, so the
     * tail of it lives on overflow pages and can only be reassembled by following
     * the chain. Read with a buffer large enough, it must come back whole -- the
     * comparison is the whole value, not a prefix. */
    CHECK((size_t)strlen(SQLITE_FIXTURE_JSON_2) > 512);
    CHECK_INT(find(2, "json", out, sizeof(out), &len), 1);
    CHECK_INT((int)len, (int)strlen(SQLITE_FIXTURE_JSON_2));
    CHECK_STR(out, SQLITE_FIXTURE_JSON_2);
    /* And its tail is really there -- a reader that stopped at the page boundary
     * would still have passed a prefix check. */
    CHECK_STR(out + strlen(out) - 40, SQLITE_FIXTURE_JSON_2 + strlen(SQLITE_FIXTURE_JSON_2) - 40);
    CHECK(strstr(out, "\"Casey Club\"") != NULL);

    T_CASE("a column that is not the one asked for");
    /* Row 1's artists, from the same record. */
    CHECK_INT(find(1, "artists", out, sizeof(out), &len), 1);
    CHECK_STR(out, "One Artist");
}

static void test_refusals(void)
{
    char out[8192];
    size_t len = 0;

    T_CASE("refusals");
    /* No such row. */
    CHECK_INT(find(99, "json", out, sizeof(out), &len), 0);
    CHECK_INT(find(0, "json", out, sizeof(out), &len), 0);
    CHECK_INT(find(-1, "json", out, sizeof(out), &len), 0);
    /* No such column, and no such table. */
    CHECK_INT(find(1, "nothing_like_this", out, sizeof(out), &len), 0);
    {
        struct mod_sqlite_query q = Q("nosuchtable", "id", "json", 1);

        CHECK_INT(mod_sqlite_find_text(SQLITE_FIXTURE, SQLITE_FIXTURE_LEN, &q,
                                       scratch, sizeof(scratch), out,
                                       sizeof(out), &len), 0);
    }
    /* A column that is an integer, not text: bpm is 120, and a number is not a
     * value worth handing back as a string. */
    CHECK_INT(find(1, "bpm", out, sizeof(out), &len), 0);
    /* A buffer that cannot hold the value: refused, not truncated. */
    {
        char small[16];

        strcpy(small, "sentinel");
        CHECK_INT(find(2, "json", small, sizeof(small), &len), 0);
        CHECK_STR(small, "sentinel");
    }
    /* Not a database at all, and a database cut short. */
    {
        struct mod_sqlite_query q = Q("tracks", "id", "json", 1);

        CHECK_INT(mod_sqlite_find_text((const uint8_t *)"not a database",
                                       sizeof("not a database"), &q,
                                       scratch, sizeof(scratch), out,
                                       sizeof(out), &len), 0);
        CHECK_INT(mod_sqlite_find_text(SQLITE_FIXTURE, 128, &q, scratch,
                                       sizeof(scratch), out, sizeof(out),
                                       &len), 0);
        /* A scratch buffer too small for any record. */
        CHECK_INT(mod_sqlite_find_text(SQLITE_FIXTURE, SQLITE_FIXTURE_LEN, &q,
                                       scratch, 8, out, sizeof(out), &len), 0);
    }
    /* Null arguments. */
    {
        struct mod_sqlite_query q = Q("tracks", "id", "json", 1);

        CHECK_INT(mod_sqlite_find_text(NULL, 0, &q, scratch, sizeof(scratch),
                                       out, sizeof(out), &len), 0);
        CHECK_INT(mod_sqlite_find_text(SQLITE_FIXTURE, SQLITE_FIXTURE_LEN, NULL,
                                       scratch, sizeof(scratch), out,
                                       sizeof(out), &len), 0);
    }
}

static void test_damaged(void)
{
    static uint8_t copy[SQLITE_FIXTURE_LEN];
    char out[8192];
    size_t len = 0;

    T_CASE("a damaged copy");
    /* The deck rewrites this database underneath us, so a torn read is a real
     * possibility. Every kind of damage must be a refusal: a wrong answer here is
     * a wrong tag on a file nobody will check until it is played. */
    memcpy(copy, SQLITE_FIXTURE, sizeof(copy));
    copy[16] = 0x00; copy[17] = 0x00;            /* page size zero */
    {
        struct mod_sqlite_query q = Q("tracks", "id", "json", 1);

        CHECK_INT(mod_sqlite_find_text(copy, sizeof(copy), &q, scratch,
                                       sizeof(scratch), out, sizeof(out),
                                       &len), 0);
    }
    memcpy(copy, SQLITE_FIXTURE, sizeof(copy));
    copy[20] = 0xff;                             /* reserved space: no usable page left */
    {
        struct mod_sqlite_query q = Q("tracks", "id", "json", 1);

        CHECK_INT(mod_sqlite_find_text(copy, sizeof(copy), &q, scratch,
                                       sizeof(scratch), out, sizeof(out),
                                       &len), 0);
    }
    memcpy(copy, SQLITE_FIXTURE, sizeof(copy));
    copy[100] = 0x00;                            /* page 1 is not a table page */
    {
        struct mod_sqlite_query q = Q("tracks", "id", "json", 1);

        CHECK_INT(mod_sqlite_find_text(copy, sizeof(copy), &q, scratch,
                                       sizeof(scratch), out, sizeof(out),
                                       &len), 0);
    }
}

int main(void)
{
    test_page_size();
    test_rows();
    test_refusals();
    test_damaged();
    return t_done("sqlite");
}
