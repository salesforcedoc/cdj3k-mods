// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * tests/test_key.c - usbsave/key.c.
 *
 * The expected values are the standard Camelot wheel, written out here rather than
 * read from the module: a table that agreed with itself would prove nothing. The
 * three anchors that are easy to check by hand -- 8B is C, 8A is its relative minor,
 * and Db minor is 12A, which is the code the deck itself displays for that key.
 */
#include "test.h"

#include "usbsave/key.h"

static const char *camelot(const char *in)
{
    static char buf[16];

    strcpy(buf, "?");
    if (!mod_key_camelot(in, buf, sizeof(buf)))
        return "(refused)";
    return buf;
}

static void test_already_camelot(void)
{
    T_CASE("already Camelot");
    CHECK_STR(camelot("12A"), "12A");
    CHECK_STR(camelot("5B"), "5B");
    CHECK_STR(camelot("1a"), "1A");
    CHECK_STR(camelot("10b"), "10B");
    CHECK_STR(camelot(" 7B "), "7B");
    /* Out of range, and trailing junk, are not Camelot. */
    CHECK_STR(camelot("13A"), "(refused)");
    CHECK_STR(camelot("0B"), "(refused)");
    CHECK_STR(camelot("12AB"), "(refused)");
    CHECK_STR(camelot("12A B"), "(refused)");
}

static void test_musical_keys(void)
{
    T_CASE("musical -> Camelot");
    /* The anchors. */
    CHECK_STR(camelot("C major"), "8B");
    CHECK_STR(camelot("C Maj"), "8B");
    CHECK_STR(camelot("A minor"), "8A");
    CHECK_STR(camelot("Am"), "8A");
    CHECK_STR(camelot("Db Minor"), "12A");
    CHECK_STR(camelot("C# minor"), "12A");        /* enharmonic spelling */
    /* The rest of the wheel, in the forms the deck and the cache use. */
    CHECK_STR(camelot("Cm"), "5A");
    CHECK_STR(camelot("Dm"), "7A");
    CHECK_STR(camelot("F Minor"), "4A");
    CHECK_STR(camelot("Bm"), "10A");
    CHECK_STR(camelot("Ebm"), "2A");
    CHECK_STR(camelot("D#m"), "2A");
    CHECK_STR(camelot("Eb Minor"), "2A");
    CHECK_STR(camelot("F# minor"), "11A");
    CHECK_STR(camelot("D major"), "10B");
    CHECK_STR(camelot("Eb Major"), "5B");
    CHECK_STR(camelot("G Major"), "9B");
    CHECK_STR(camelot("Gb major"), "2B");
    CHECK_STR(camelot("F# major"), "2B");
    CHECK_STR(camelot("Ab major"), "4B");
    CHECK_STR(camelot("G# major"), "4B");
    CHECK_STR(camelot("B major"), "1B");
    CHECK_STR(camelot("A major"), "11B");
    CHECK_STR(camelot("  eb minor"), "2A");
}

static void test_refuses(void)
{
    T_CASE("refusals");
    /* A note with no quality is major or minor and cannot be told apart. */
    CHECK_STR(camelot("D"), "(refused)");
    CHECK_STR(camelot("C"), "(refused)");
    CHECK_STR(camelot("Eb"), "(refused)");
    CHECK_STR(camelot("B"), "(refused)");
    /* Not a key at all. */
    CHECK_STR(camelot(""), "(refused)");
    CHECK_STR(camelot("   "), "(refused)");
    CHECK_STR(camelot("H"), "(refused)");
    CHECK_STR(camelot("xyz"), "(refused)");
    CHECK_STR(camelot("Not Loaded"), "(refused)");
}

static void test_capacity(void)
{
    /* Three bytes exactly: enough for a sentinel, not enough for "12A" and its NUL,
     * so this is a refusal and the bytes must come back untouched. memcpy rather
     * than strcpy, since the point is that there is no room for a terminator. */
    char small[3], exact[4];

    T_CASE("capacity");
    memcpy(small, "snt", 3);
    CHECK_INT(mod_key_camelot("12A", small, sizeof(small)), 0);
    CHECK(small[0] == 's' && small[1] == 'n' && small[2] == 't');
    CHECK_INT(mod_key_camelot("12A", exact, sizeof(exact)), 1);
    CHECK_STR(exact, "12A");
    CHECK_INT(mod_key_camelot("12A", exact, 0), 0);
}

int main(void)
{
    test_already_camelot();
    test_musical_keys();
    test_refuses();
    test_capacity();
    return t_done("key");
}
