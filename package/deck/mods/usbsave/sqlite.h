// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/sqlite.h - reading a value out of a SQLite database file, for USB Save.
 *
 * WHY THIS EXISTS. The deck keeps a streamed track's metadata as a JSON blob in a
 * column of its Beatport client's sqlite3.db, and the mod finds it by scanning the
 * file for the track's own `"id"`. That scan reads the database as flat bytes, and
 * for most rows that works: a ~3 KB record fits inside one 4 KB page.
 *
 * IT STOPS WORKING FOR BIG RECORDS. When a value does not fit on its page SQLite
 * spills the rest onto OVERFLOW PAGES, and every page in the file begins with its own
 * header -- four bytes on an overflow page, holding the number of the page that
 * continues it. Reading the file linearly therefore steps straight over four bytes of
 * page number in the middle of the text. A brace or a key straddling that point is
 * corrupted, the JSON no longer balances, and the record cannot be read at all.
 *
 * Removing those four bytes from every page leaves every value's bytes contiguous
 * again -- in every kind of page the four are header, never payload:
 *
 *   b-tree page   8-byte header (type, freeblock, cell count, content start, frag),
 *                  or 12 with a right-most pointer; payload starts at 8 or later
 *   overflow page the 4-byte next-page pointer, then payload
 *   freelist page 8-byte trunk header
 *
 * So the caller strips the file once, in place, and then scans it exactly as before.
 * That is all this module is: no cell decoding, no B-tree walk, nothing that would
 * need to understand the schema.
 *
 * Pure: the mods' libc subset only.
 */
#ifndef EP122_MOD_USBSAVE_SQLITE_H
#define EP122_MOD_USBSAVE_SQLITE_H

#include <stddef.h>
#include <stdint.h>

/* The page size the database declares, or 0 when `buf` does not begin with a SQLite
 * header or declares one that cannot be right. */
uint32_t mod_sqlite_page_size(const uint8_t *buf, size_t len);

/* Compact `buf` in place, dropping each page's header. Returns the new length, or
 * `len` unchanged when there is nothing to do. Page 0 keeps its first bytes: its
 * 100-byte database header is never payload, so what is dropped there is arbitrary
 * and doing nothing is simpler. */
size_t mod_sqlite_deoverflow(uint8_t *buf, size_t len, uint32_t page_size);

#endif /* EP122_MOD_USBSAVE_SQLITE_H */
