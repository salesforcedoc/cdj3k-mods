// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/sqlite.h - read one column out of a SQLite database, for USB Save.
 *
 * WHY THIS EXISTS. The deck keeps a streamed track's metadata as a JSON blob in a
 * column of its client's sqlite3.db, and the mods want that blob -- the artist,
 * album, genre, year and cover come from it and from nowhere else, because the
 * track itself carries no tags at all.
 *
 * IT USED TO SCAN THE FILE AS FLAT BYTES, and that is not sound. A value too big
 * for its page spills onto OVERFLOW PAGES, and those pages are chained BY PAGE
 * NUMBER, not by position: the page holding the next part of a value can be
 * anywhere in the file. Scanning contiguously therefore reads whatever the pages
 * after it happen to hold. That works while a database is young and its pages are
 * laid out in order, and stops working as soon as the file has been rewritten
 * enough for the free list to be reused -- which is exactly the shape of a deck
 * that rewrites this database on every track it streams. The symptom was
 * maddening and intermittent: the record's text was plainly in the file, and no
 * balanced object could be found around it.
 *
 * So this walks the database the way SQLite does: down the B-tree to the row,
 * decode the record, and follow the overflow chain by page number. It is not a
 * general SQLite client -- no SQL, no indexes, no writing -- just enough to answer
 * "what is this row's value", which is the only question here.
 *
 * Pure: the mods' libc subset only, and no allocation. The caller supplies the
 * scratch the largest record needs.
 */
#ifndef EP122_MOD_USBSAVE_SQLITE_H
#define EP122_MOD_USBSAVE_SQLITE_H

#include <stddef.h>
#include <stdint.h>

/* The page size the database declares, or 0 when `buf` does not begin with a
 * SQLite header or declares one that cannot be right. */
uint32_t mod_sqlite_page_size(const uint8_t *buf, size_t len);

/* What to look for. The three strings' lengths come with them because strlen is
 * not in the mods' libc subset -- and because writing the counting loop out by hand
 * does not help: the compiler folds it straight back into a strlen call. At every
 * call site these are compile-time constants, spelled sizeof("...") - 1. */
struct mod_sqlite_query {
    const char *table;          /* e.g. "tracks"                              */
    size_t      table_len;
    const char *id_column;      /* matched against `id`, e.g. "id"             */
    size_t      id_column_len;
    const char *column;         /* the value wanted, e.g. "json"               */
    size_t      column_len;
    long        id;
};

/* Find the row of `table` whose `id_column` equals `q->id`, and copy that row's
 * `column` into `out` if it is text. Returns 1 on success with the length in
 * `out_len` and `out` NUL-terminated, 0 when there is no such row, the value is
 * not text, or anything about the file does not add up -- nothing is written in
 * that case. `scratch` holds one whole record while it is decoded and must be at
 * least as large as the biggest one; a record that does not fit is skipped rather
 * than truncated. */
int mod_sqlite_find_text(const uint8_t *db, size_t len,
                         const struct mod_sqlite_query *q,
                         uint8_t *scratch, size_t scratch_cap,
                         char *out, size_t cap, size_t *out_len);

#endif /* EP122_MOD_USBSAVE_SQLITE_H */
