// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/sqlite.c - the page handling described in sqlite.h.
 *
 * Two small functions and no parsing. The compaction is a single forward pass: read
 * a page, write it four bytes in, and carry on -- which is safe in place because the
 * write pointer never overtakes the read pointer.
 */
#include "usbsave/sqlite.h"

#include <string.h>

/* The 16 bytes every SQLite database file begins with. */
static const char SQLITE_MAGIC[16] = "SQLite format 3";

uint32_t mod_sqlite_page_size(const uint8_t *buf, size_t len)
{
    uint32_t ps;

    if (!buf || len < 100)
        return 0;
    if (memcmp(buf, SQLITE_MAGIC, sizeof(SQLITE_MAGIC)) != 0)
        return 0;
    /* The header's page size, big-endian, at offset 16. The value 1 is the format's
     * way of spelling 65536, which does not fit in the field. */
    ps = ((uint32_t)buf[16] << 8) | (uint32_t)buf[17];
    if (ps == 1)
        return 65536;
    /* Anything else must be a power of two of at least 512. A file that says
     * otherwise is not one this should be reading as a database. */
    if (ps < 512 || (ps & (ps - 1)) != 0)
        return 0;
    return ps;
}

size_t mod_sqlite_deoverflow(uint8_t *buf, size_t len, uint32_t page_size)
{
    size_t page, w = 0;

    if (!buf || page_size < 8)
        return len;

    for (page = 0; page * (size_t)page_size < len; page++) {
        size_t start = page * (size_t)page_size;
        size_t end = start + page_size;

        if (end > len)
            end = len;
        if (page > 0) {
            /* This page's header, which is never payload. */
            if (start + 4 >= end)
                continue;                   /* a tail too short to hold a header */
            start += 4;
        }
        if (start < end) {
            memmove(buf + w, buf + start, end - start);
            w += end - start;
        }
    }
    return w;
}
