// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * tests/test_json.c - usbsave/json.c against the document Beatport actually
 * writes.
 *
 * The fixture is the track the deck cached on its eMMC, trimmed but not reshaped:
 * the same keys, the same nesting, the same types, with the field order shuffled
 * so nothing here can pass by reading the object left to right in the order it
 * happens to want. `artists` comes before the top-level `name` on purpose -- a
 * reader that does not skip nested values would hand back the artist's name for
 * the track's.
 */
#include "test.h"

#include "usbsave/json.h"

/* One member per line so a failure points at a key rather than at a blob. Keys
 * deliberately out of the order the readers might want them in, and shaped like
 * the real record: the top-level `image` is the 1500x250 banner while the COVER
 * is release.image, which is why the two are read by different paths. */
static const char BEATPORT[] =
    "{"
      "\"stream_quality\":\".256k.aac.mp4\","
      "\"artists\":[{\"id\":1072601,\"name\":\"Victony\"}],"
      "\"bpm\":110,"
      "\"genre\":{\"id\":102,\"name\":\"African\",\"slug\":\"african\"},"
      "\"id\":29581342,"
      "\"image\":{\"uri\":\"https://geo-media.beatport.com/image_size/1500x250/3babb9cf-banner.png\"},"
      "\"isrc\":\"QZWA32202168\","
      "\"key\":{\"camelot_number\":10,\"camelot_letter\":\"A\",\"name\":\"B Minor\"},"
      "\"length\":\"1:46\","
      "\"mix_name\":\"Original Mix\","
      "\"name\":\"SLICK\","
      "\"new_release_date\":\"2026-09-25\","
      "\"publish_date\":\"2020-10-29\","
      "\"release\":{\"id\":7432035,\"name\":\"Cold Nights\","
        "\"image\":{\"uri\":\"https://geo-media.beatport.com/image_size/1400x1400/8cac22ea-cover.jpg\"}}"
    "}";

static void test_top_level(void)
{
    char buf[128];
    long n = -1;

    T_CASE("top level");
    /* The track's own name, not the artist's nested one. */
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "name", buf, sizeof(buf)), 1);
    CHECK_STR(buf, "SLICK");
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "mix_name", buf, sizeof(buf)), 1);
    CHECK_STR(buf, "Original Mix");
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "stream_quality", buf, sizeof(buf)), 1);
    CHECK_STR(buf, ".256k.aac.mp4");
    CHECK_INT(mod_json_int(BEATPORT, sizeof(BEATPORT) - 1, "bpm", &n), 1);
    CHECK_INT(n, 110);
    CHECK_INT(mod_json_int(BEATPORT, sizeof(BEATPORT) - 1, "id", &n), 1);
    CHECK_INT(n, 29581342);
}

static void test_nested(void)
{
    char buf[128];
    long n = -1;

    T_CASE("nested");
    CHECK_INT(mod_json_obj_str(BEATPORT, sizeof(BEATPORT) - 1, "genre", "name", buf, sizeof(buf)), 1);
    CHECK_STR(buf, "African");
    CHECK_INT(mod_json_obj_str(BEATPORT, sizeof(BEATPORT) - 1, "key", "name", buf, sizeof(buf)), 1);
    CHECK_STR(buf, "B Minor");
    CHECK_INT(mod_json_obj_int(BEATPORT, sizeof(BEATPORT) - 1, "key", "camelot_number", &n), 1);
    CHECK_INT(n, 10);
    CHECK_INT(mod_json_obj_str(BEATPORT, sizeof(BEATPORT) - 1, "key", "camelot_letter", buf, sizeof(buf)), 1);
    CHECK_STR(buf, "A");
    CHECK_INT(mod_json_obj_str(BEATPORT, sizeof(BEATPORT) - 1, "image", "uri", buf, sizeof(buf)), 1);
    CHECK_STR(buf, "https://geo-media.beatport.com/image_size/1500x250/3babb9cf-banner.png");
}

static void test_array(void)
{
    char buf[128];

    T_CASE("array");
    CHECK_INT(mod_json_arr_str(BEATPORT, sizeof(BEATPORT) - 1, "artists", "name", buf, sizeof(buf)), 1);
    CHECK_STR(buf, "Victony");
    /* A key that exists only nested is NOT a top-level field. */
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "camelot_number", buf, sizeof(buf)), 0);
    /* An array that is not there. */
    CHECK_INT(mod_json_arr_str(BEATPORT, sizeof(BEATPORT) - 1, "labels", "name", buf, sizeof(buf)), 0);
    /* An array whose first element is not an object. */
    CHECK_INT(mod_json_arr_str("{\"a\":[1,2]}", 13, "a", "x", buf, sizeof(buf)), 0);
}

static void test_absent_and_wrong_type(void)
{
    char buf[128];
    long n = -1;

    T_CASE("absent / type");
    /* Absent leaves the buffer alone, and says so. */
    strcpy(buf, "sentinel");
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "album", buf, sizeof(buf)), 0);
    CHECK_STR(buf, "sentinel");
    /* bpm is a number, not a string. */
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "bpm", buf, sizeof(buf)), 0);
    /* name is a string, not a number. */
    CHECK_INT(mod_json_int(BEATPORT, sizeof(BEATPORT) - 1, "name", &n), 0);
    /* A nested key asked for at the top level is not found. */
    CHECK_INT(mod_json_obj_str(BEATPORT, sizeof(BEATPORT) - 1, "genre", "uri", buf, sizeof(buf)), 0);
    /* A scalar asked for as an object. */
    CHECK_INT(mod_json_obj_str(BEATPORT, sizeof(BEATPORT) - 1, "name", "x", buf, sizeof(buf)), 0);
}

static void test_bounds_and_escapes(void)
{
    T_CASE("bounds / escapes");
    /* "SLICK" is five bytes and needs six with the NUL: four is too small, and
     * the reader refuses rather than truncate, leaving the buffer alone. */
    {
        char tiny[4], exact[6];

        strcpy(tiny, "snt");
        CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "name", tiny, sizeof(tiny)), 0);
        CHECK_STR(tiny, "snt");
        CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "name", exact, sizeof(exact)), 1);
        CHECK_STR(exact, "SLICK");
    }

    /* Escapes, including a \u that needs two bytes out and a surrogate pair. */
    {
        static const char ESC[] =
            "{\"t\":\"A\\\"B\\\\C\\nD\\u00e9\\u0041\\ud83d\\ude00\"}";
        char big[64];

        CHECK_INT(mod_json_str(ESC, sizeof(ESC) - 1, "t", big, sizeof(big)), 1);
        CHECK_STR(big, "A\"B\\C\nD\xc3\xa9" "A\xf0\x9f\x98\x80");
    }

    /* Malformed: an unterminated string, and a missing colon. */
    {
        char buf[8];

        CHECK_INT(mod_json_str("{\"a\":\"x}", 8, "a", buf, sizeof(buf)), 0);
        CHECK_INT(mod_json_str("{\"a\" \"x\"}", 9, "a", buf, sizeof(buf)), 0);
        /* Whitespace between everything must not matter. */
        CHECK_INT(mod_json_str("  {  \"a\"  :  \"x\"  }  ", 20, "a", buf, sizeof(buf)), 1);
        CHECK_STR(buf, "x");
    }
}

static void test_empty_and_null(void)
{
    char buf[8];

    T_CASE("empty / null");
    CHECK_INT(mod_json_str("{}", 2, "a", buf, sizeof(buf)), 0);
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "name", NULL, sizeof(buf)), 0);
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "name", buf, 0), 0);
}

static void test_path(void)
{
    static const char *const cover[] = { "release", "image", "uri" };
    static const char *const release_name[] = { "release", "name" };
    static const char *const missing[] = { "release", "label", "name" };
    static const char *const not_an_object[] = { "name", "x" };
    char buf[256];

    T_CASE("path");
    /* The cover is release.image.uri, NOT the top-level image -- that one is the
     * 1500x250 banner and its hash is not what the deck caches the art under. */
    CHECK_INT(mod_json_path_str(BEATPORT, sizeof(BEATPORT) - 1, cover, 3,
                                buf, sizeof(buf)), 1);
    CHECK_STR(buf, "https://geo-media.beatport.com/image_size/1400x1400/8cac22ea-cover.jpg");
    CHECK_INT(mod_json_path_str(BEATPORT, sizeof(BEATPORT) - 1, release_name, 2,
                                buf, sizeof(buf)), 1);
    CHECK_STR(buf, "Cold Nights");
    /* A key that is not on the way, and one whose ancestor is not an object. */
    CHECK_INT(mod_json_path_str(BEATPORT, sizeof(BEATPORT) - 1, missing, 3,
                                buf, sizeof(buf)), 0);
    CHECK_INT(mod_json_path_str(BEATPORT, sizeof(BEATPORT) - 1, not_an_object, 2,
                                buf, sizeof(buf)), 0);
    CHECK_INT(mod_json_path_str(BEATPORT, sizeof(BEATPORT) - 1, cover, 0,
                                buf, sizeof(buf)), 0);

    T_CASE("dates");
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "new_release_date",
                           buf, sizeof(buf)), 1);
    CHECK_STR(buf, "2026-09-25");
    CHECK_INT(mod_json_str(BEATPORT, sizeof(BEATPORT) - 1, "publish_date",
                           buf, sizeof(buf)), 1);
    CHECK_STR(buf, "2020-10-29");
}

static void test_span(void)
{
    /* The value holds a '}' and a '{' INSIDE a string, which is the case that
     * separates a brace walk that skips strings from one that does not. */
    static const char DOC[] = "{\"a\":{\"b\":1},\"c\":\"}not a brace{\"}";

    T_CASE("span");
    /* From the opening brace, the span is the whole object. */
    CHECK_INT((int)mod_json_span(DOC, sizeof(DOC) - 1, 0), (int)(sizeof(DOC) - 1));
    /* A nested value balances on its own. */
    CHECK_INT((int)mod_json_span(DOC, sizeof(DOC) - 1, 5), 12);
    /* Not a value, and past the end. */
    CHECK_INT((int)mod_json_span(DOC, sizeof(DOC) - 1, 1), 0);
    CHECK_INT((int)mod_json_span(DOC, sizeof(DOC) - 1, 999), 0);
    /* Unterminated. */
    CHECK_INT((int)mod_json_span("{\"a\":1", 6, 0), 0);
    /* An object lifted out of surrounding binary, which is how the deck's sqlite
     * cache is read: the span ends at the matching brace and no further. */
    {
        static const char BLOB[] = "\x00\x11junk{\"id\":7,\"name\":\"x\"}\x7f\x00";
        size_t s = 6, e = mod_json_span(BLOB, sizeof(BLOB) - 1, s);

        CHECK_INT((int)e, 25);
    }
}

static void test_array_join(void)
{
    /* A track credited to three artists, the shape the deck's own cache stores as
     * one comma-joined column. */
    static const char THREE[] =
        "{\"name\":\"Mi Chico\",\"artists\":[{\"id\":1,\"name\":\"Jason Derulo\"},"
        "{\"id\":2,\"name\":\"Melody\"},{\"id\":3,\"name\":\"DJ Goja\"}]}";
    /* One that is not there, one that is empty, and one with a non-object element. */
    static const char ODD[] = "{\"artists\":[],\"x\":[1,2]}";
    char buf[128];

    T_CASE("array join");
    CHECK_INT(mod_json_arr_join(THREE, sizeof(THREE) - 1, "artists", "name", ", ",
                                buf, sizeof(buf)), 1);
    CHECK_STR(buf, "Jason Derulo, Melody, DJ Goja");
    /* A single entry has no separator. */
    CHECK_INT(mod_json_arr_join(BEATPORT, sizeof(BEATPORT) - 1, "artists", "name",
                                ", ", buf, sizeof(buf)), 1);
    CHECK_STR(buf, "Victony");
    /* Nothing to join is a refusal, and the buffer is untouched. */
    strcpy(buf, "sent");
    CHECK_INT(mod_json_arr_join(ODD, sizeof(ODD) - 1, "artists", "name", ", ",
                                buf, sizeof(buf)), 0);
    CHECK_STR(buf, "sent");
    /* A field that is not an array at all. */
    CHECK_INT(mod_json_arr_join(BEATPORT, sizeof(BEATPORT) - 1, "name", "name",
                                ", ", buf, sizeof(buf)), 0);
    /* Too small for the whole list: refuses rather than truncating mid-name. */
    {
        char small[10];

        CHECK_INT(mod_json_arr_join(THREE, sizeof(THREE) - 1, "artists", "name",
                                    ", ", small, sizeof(small)), 1);
        CHECK(small[sizeof(small) - 1] == '\0');
    }
}

int main(void)
{
    test_top_level();
    test_nested();
    test_array();
    test_array_join();
    test_absent_and_wrong_type();
    test_bounds_and_escapes();
    test_empty_and_null();
    test_path();
    test_span();
    return t_done("json");
}
