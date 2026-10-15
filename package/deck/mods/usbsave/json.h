// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/json.h - read a few fields out of a JSON object, for USB Save.
 *
 * WHY THIS EXISTS. A streamed track's file carries no tags, so USB Save names and
 * tags the copy from what the deck knows. The deck's title bar gives the title, the
 * BPM and the key, but there is no artist label on it and no genre anywhere -- and
 * the artist is what a DJ's folder is organised by. The deck DOES have them: it
 * fetched the track's metadata over HTTP and cached the JSON on its own eMMC, with
 * `artists[].name`, `genre.name`, `key.name`, `bpm`, `name` and `mix_name` all in
 * it. This reads those out of the cached file. No request leaves the machine.
 *
 * IT IS NOT A JSON PARSER. It answers "what is this field" about the one schema
 * Beatport writes, and a document it does not understand yields "not found" rather
 * than a guess: every entry point returns 0 and nothing is written. That is the
 * same bargain mp4.c makes with a container it cannot vouch for.
 *
 * Pure, and in the mods' libc subset: lengths are passed in rather than measured
 * with strlen, and nothing here names a symbol outside memcpy and friends.
 */
#ifndef EP122_MOD_USBSAVE_JSON_H
#define EP122_MOD_USBSAVE_JSON_H

#include <stddef.h>

/* Every call below reads the object that begins the document and returns 1 with
 * `out` filled when the field is there and is of the right type, or 0 when it is
 * absent, is some other type, or does not fit. `out` is NUL-terminated on success
 * and is left untouched on failure. Strings are unescaped, with \uXXXX decoded to
 * UTF-8; `cap` counts the NUL. */

/* A top-level string field: mod_json_str(js, len, "name", out, cap). */
int mod_json_str(const char *js, size_t len, const char *key, char *out,
                 size_t cap);

/* A string one level in: the `key` of the object-valued field `outer` -- the
 * artist's name, the genre's, the key's, the cover's uri. */
int mod_json_obj_str(const char *js, size_t len, const char *outer,
                     const char *key, char *out, size_t cap);

/* The `key` of the FIRST object in the array-valued field `arr` -- the first
 * entry of `artists`, which is the one the deck credits. */
int mod_json_arr_str(const char *js, size_t len, const char *arr,
                     const char *key, char *out, size_t cap);

/* Every object's `key` in the array-valued field `arr`, joined with `sep`. A track
 * is very often credited to several artists, and the deck's own cache keeps them
 * comma-joined in one column for exactly that reason -- so a tag that named only
 * the first would drop the rest. Returns 1 when at least one was found. */
int mod_json_arr_join(const char *js, size_t len, const char *arr,
                      const char *key, const char *sep, char *out, size_t cap);

/* A top-level integer field: `bpm`. */
int mod_json_int(const char *js, size_t len, const char *key, long *out);

/* An integer one level in: `key`'s camelot_number, which pairs with the letter
 * to make the key a DJ reads ("10A"). */
int mod_json_obj_int(const char *js, size_t len, const char *outer,
                     const char *key, long *out);

/* A string reached by walking `path` as a chain of objects and taking the last
 * key's string: {"release","image","uri"} is release.image.uri, which is the
 * cover -- the top-level `image` is a 1500x250 banner, not the artwork. */
int mod_json_path_str(const char *js, size_t len, const char *const *path,
                      size_t depth, char *out, size_t cap);

/* The offset just past the JSON value beginning at `start`, or 0 when it does not
 * balance. `start` must be '{' or '['. This is what lifts ONE object out of a
 * larger blob: the deck keeps a track's metadata inside its client's sqlite cache
 * rather than as a .json file, so the caller scans for the id and takes the braces
 * around it. The buffer may hold arbitrary bytes; only the object is examined. */
size_t mod_json_span(const char *js, size_t len, size_t start);

#endif /* EP122_MOD_USBSAVE_JSON_H */
