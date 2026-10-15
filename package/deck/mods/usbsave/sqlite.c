// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/sqlite.c - the reader described in sqlite.h.
 *
 * The file format is walked in the order a value actually lives in: the database
 * header, then page 1's sqlite_master to find the table's root page, then the
 * table's own B-tree down to the row, then the record, then -- for a value too big
 * for its page -- the overflow chain, one page number at a time.
 *
 * Everything is bounds-checked against the file the caller passed, and anything
 * that does not add up is a refusal. That matters more here than elsewhere: this
 * reads a file the deck rewrites underneath it, so a half-written page is a real
 * possibility rather than a hypothetical, and the only safe answer to one is to
 * stop rather than to read whatever the bytes happen to say.
 *
 * Pure: no allocation, and no libc beyond memcpy and memcmp.
 */
#include "usbsave/sqlite.h"

#include <string.h>

#define SQLITE_MAGIC "SQLite format 3"

/* Largest CREATE TABLE statement this will read. The deck's own are a few hundred
 * bytes; anything longer is not something to guess at. */
#define SQL_MAX 1024

/* The header of the database itself is 100 bytes; a page 1 b-tree header starts
 * after it. Every other page starts at zero. */
#define DB_HEADER 100

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ---- variable-length integers --------------------------------------------- *
 *
 * Up to nine bytes: eight holding seven bits each, and a ninth holding eight. Used
 * for every length, rowid and serial type in the format. */
static int rd_varint(const uint8_t *p, size_t len, size_t *used, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;

    for (i = 0; i < 8; i++) {
        if (i >= len)
            return -1;
        v = (v << 7) | (uint64_t)(p[i] & 0x7f);
        if ((p[i] & 0x80) == 0) {
            *used = i + 1;
            *out = v;
            return 0;
        }
    }
    if (i >= len)
        return -1;
    *out = (v << 8) | (uint64_t)p[8];
    *used = 9;
    return 0;
}

/* ---- the database ---------------------------------------------------------- */

uint32_t mod_sqlite_page_size(const uint8_t *buf, size_t len)
{
    uint32_t ps;

    if (!buf || len < DB_HEADER)
        return 0;
    if (memcmp(buf, SQLITE_MAGIC, 16) != 0)
        return 0;
    /* Big-endian at offset 16; the value 1 is how the format spells 65536, which
     * does not fit in the two bytes it has. */
    ps = ((uint32_t)buf[16] << 8) | (uint32_t)buf[17];
    if (ps == 1)
        return 65536;
    if (ps < 512 || (ps & (ps - 1)) != 0)
        return 0;
    return ps;
}

struct db {
    const uint8_t *buf;
    size_t         len;
    uint32_t       page;        /* page size in bytes                     */
    uint32_t       usable;      /* page size less the reserved tail        */
    uint32_t       npage;
};

static int db_open(struct db *d, const uint8_t *buf, size_t len)
{
    uint32_t ps = mod_sqlite_page_size(buf, len);
    uint32_t reserved;

    if (ps == 0)
        return -1;
    reserved = buf[20];
    if (reserved >= ps)
        return -1;
    d->buf = buf;
    d->len = len;
    d->page = ps;
    d->usable = ps - reserved;
    /* The format requires at least 480 usable bytes, and says so because below that
     * the payload arithmetic stops working. A file claiming less is not one to read
     * a value out of. */
    if (d->usable < 480)
        return -1;
    d->npage = (uint32_t)(len / ps);
    return d->npage == 0 ? -1 : 0;
}

/* Page `pno`, which is one-based, or NULL. */
static const uint8_t *db_page(const struct db *d, uint32_t pno)
{
    size_t off;

    if (pno < 1 || pno > d->npage)
        return NULL;
    off = (size_t)(pno - 1) * d->page;
    if (off + d->usable > d->len)
        return NULL;
    return d->buf + off;
}

/* ---- B-tree pages ---------------------------------------------------------- */

struct btree {
    int      leaf;
    uint32_t ncell;
    size_t   hdr;       /* where the page's b-tree header starts within it */
    size_t   hdrlen;    /* 8, or 12 on an interior page                   */
};

static int btree_open(const struct db *d, uint32_t pno, struct btree *bt,
                      const uint8_t **page)
{
    const uint8_t *p = db_page(d, pno);
    size_t hdr = (pno == 1) ? DB_HEADER : 0;

    if (!p || hdr + 12 > d->usable)
        return -1;
    if (p[hdr] == 0x0d)
        bt->leaf = 1;
    else if (p[hdr] == 0x05)
        bt->leaf = 0;
    else
        return -1;                      /* an index page, or a free one */
    bt->ncell = ((uint32_t)p[hdr + 3] << 8) | (uint32_t)p[hdr + 4];
    bt->hdr = hdr;
    bt->hdrlen = bt->leaf ? 8 : 12;
    if (hdr + bt->hdrlen + (size_t)bt->ncell * 2 > d->usable)
        return -1;
    *page = p;
    return 0;
}

static size_t cell_off(const struct btree *bt, const uint8_t *page, uint32_t i)
{
    size_t at = bt->hdr + bt->hdrlen + (size_t)i * 2;

    return ((size_t)page[at] << 8) | (size_t)page[at + 1];
}

/* ---- getting a leaf cell's payload ----------------------------------------- *
 *
 * A value that does not fit on its page keeps its first part there and the rest on
 * overflow pages, chained by the four-byte page number that sits immediately after
 * those first bytes. HOW MUCH stays behind is the format's own arithmetic, and it
 * is not "as much as fits": the split is chosen so no overflow page holds a
 * fragment too small to be useful, which is what U-35 and the M/K terms below are.
 * Getting them wrong reads a value shifted by a few bytes, which parses as
 * confident nonsense rather than failing, so they are spelled out. */
static int cell_payload(const struct db *d, const uint8_t *page, size_t coff,
                        uint8_t *out, size_t cap, size_t *plen)
{
    const uint8_t *p;
    size_t avail, used;
    uint64_t payload, skip, local, rest, U, X, M, K;
    uint32_t next;

    if (coff + 2 > d->usable)
        return -1;
    p = page + coff;
    avail = d->usable - coff;

    if (rd_varint(p, avail, &used, &payload) != 0)
        return -1;
    p += used; avail -= used;
    if (rd_varint(p, avail, &used, &skip) != 0)       /* the rowid */
        return -1;
    p += used; avail -= used;
    (void)skip;

    if (payload > (uint64_t)cap)
        return -1;

    U = d->usable;
    X = U - 35;
    if (payload <= X) {
        local = payload;
    } else {
        M = ((U - 12) * 32 / 255) - 23;
        K = M + ((payload - M) % (U - 4));
        local = (K <= X) ? K : M;
    }
    if (local > (uint64_t)avail)
        return -1;
    memcpy(out, p, (size_t)local);
    rest = payload - local;
    p += local;
    avail -= (size_t)local;

    if (rest > 0) {
        if (avail < 4)
            return -1;
        next = be32(p);
        while (rest > 0) {
            const uint8_t *op = db_page(d, next);
            size_t chunk;

            if (!op)
                return -1;
            chunk = (rest < (uint64_t)(U - 4)) ? (size_t)rest : (size_t)(U - 4);
            memcpy(out + (size_t)(payload - rest), op + 4, chunk);
            rest -= chunk;
            if (rest > 0)
                next = be32(op);
        }
    }
    *plen = (size_t)payload;
    return 0;
}

/* ---- records --------------------------------------------------------------- *
 *
 * A record is a header of serial types followed by the values, in column order.
 * The header's first varint is its own length, which is what says where the values
 * begin. */

/* How many bytes a value of this serial type occupies. The two odd/even branches
 * of the large range are one expression: for an odd type, (t-12)/2 truncates to
 * (t-13)/2. */
static size_t ser_size(uint64_t t)
{
    static const uint8_t intsz[7] = { 0, 1, 2, 3, 4, 6, 8 };

    if (t == 0 || t == 8 || t == 9 || t == 10 || t == 11)
        return 0;
    if (t <= 6)
        return intsz[t];
    if (t == 7)
        return 8;
    return (size_t)((t - 12) / 2);
}

/* Column `want` of a record: its serial type and its bytes. */
static int rec_col(const uint8_t *rec, size_t len, uint32_t want,
                   uint64_t *st, const uint8_t **val, size_t *vlen)
{
    size_t used, h, at, i;
    uint64_t hdr;

    if (rd_varint(rec, len, &used, &hdr) != 0)
        return -1;
    if (hdr < used || hdr > len)
        return -1;
    h = used;
    at = (size_t)hdr;
    for (i = 0; h < (size_t)hdr; i++) {
        uint64_t t;
        size_t u, n;

        if (rd_varint(rec + h, (size_t)hdr - h, &u, &t) != 0)
            return -1;
        h += u;
        n = ser_size(t);
        if (i == want) {
            if (at + n > len)
                return -1;
            *st = t;
            *val = rec + at;
            *vlen = n;
            return 0;
        }
        at += n;
    }
    return -1;
}

/* A big-endian integer of `n` bytes, signed -- the format stores integers with the
 * fewest bytes that hold them, so the top bit of the first byte is the sign. */
static int64_t be_int(const uint8_t *p, size_t n)
{
    int64_t v = 0;
    size_t i;

    for (i = 0; i < n; i++)
        v = (v << 8) | (int64_t)p[i];
    if (n > 0 && n < 8 && (p[0] & 0x80))
        v -= ((int64_t)1) << (n * 8);
    return v;
}

/* A record's column as an integer, whichever of the integer encodings it used.
 * Returns 0 when the column is not an integer at all. */
static int rec_int(const uint8_t *rec, size_t len, uint32_t want, int64_t *out)
{
    uint64_t st;
    const uint8_t *v;
    size_t n;

    if (rec_col(rec, len, want, &st, &v, &n) != 0)
        return 0;
    if (st >= 1 && st <= 6) {
        *out = be_int(v, n);
        return 1;
    }
    if (st == 8) {
        *out = 0;
        return 1;
    }
    if (st == 9) {
        *out = 1;
        return 1;
    }
    return 0;
}

/* ---- the schema ------------------------------------------------------------ *
 *
 * Which column is which comes from the table's own CREATE TABLE statement, read out
 * of sqlite_master. Column POSITION would be simpler and is how it worked when this
 * was a byte scan, but it is a guess about someone else's schema: the deck writes
 * this database, and a firmware update is free to reorder it. */

static int ident_end(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')';
}

/* Is the identifier at `p` exactly `name` (which is lower case)? */
static int col_named(const char *p, size_t n, const char *name, size_t nlen)
{
    size_t i = 0, k = 0;

    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r'))
        i++;
    while (k < nlen) {
        char c;

        if (i + k >= n)
            return 0;
        c = p[i + k];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != name[k])
            return 0;
        k++;
    }
    if (i + k < n && !ident_end(p[i + k]))
        return 0;
    return 1;
}

/* The index of `name` among the column definitions of a CREATE TABLE statement, or
 * -1. Commas inside a type's parentheses are tracked by depth so DECIMAL(10,2)
 * cannot look like a column boundary. */
static int column_index(const char *sql, size_t len, const char *name, size_t nlen)
{
    size_t i, start = 0;
    int depth = 0, index = 0;

    for (i = 0; i < len; i++) {
        char c = sql[i];

        if (depth == 0) {
            if (c == '(') {
                depth = 1;
                index = 0;
                start = i + 1;
            }
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth == 1) {
                if (col_named(sql + start, i - start, name, nlen))
                    return index;
                return -1;
            }
            depth--;
        } else if (c == ',' && depth == 1) {
            if (col_named(sql + start, i - start, name, nlen))
                return index;
            index++;
            start = i + 1;
        }
    }
    return -1;
}

/* ---- walking a tree -------------------------------------------------------- */

/* One leaf cell of `table` decoded far enough to compare its id column, and, when
 * it matches, to hand back the wanted column's text. */
static int row_matches(const struct db *d, const uint8_t *page, size_t coff,
                       const struct mod_sqlite_query *q, int id_col, int want_col,
                       uint8_t *scratch, size_t scratch_cap,
                       char *out, size_t cap, size_t *out_len)
{
    size_t plen = 0, n;
    uint64_t st;
    const uint8_t *v;
    int64_t got;

    if (cell_payload(d, page, coff, scratch, scratch_cap, &plen) != 0)
        return 0;
    if (!rec_int(scratch, plen, (uint32_t)id_col, &got) || got != (int64_t)q->id)
        return 0;
    if (rec_col(scratch, plen, (uint32_t)want_col, &st, &v, &n) != 0)
        return 0;
    /* TEXT is any odd serial type of 13 or more; anything else -- NULL, a number, a
     * blob -- is not a value worth writing out. */
    if (st < 13 || (st % 2) == 0 || n + 1 > cap)
        return 0;
    memcpy(out, v, n);
    out[n] = '\0';
    *out_len = n;
    return 1;
}

static int tree_find(const struct db *d, uint32_t root,
                     const struct mod_sqlite_query *q, int id_col, int want_col,
                     uint8_t *scratch, size_t scratch_cap,
                     char *out, size_t cap, size_t *out_len, int depth)
{
    struct btree bt;
    const uint8_t *page = NULL;
    uint32_t i;

    if (depth > 16 || btree_open(d, root, &bt, &page) != 0)
        return 0;

    if (bt.leaf) {
        for (i = 0; i < bt.ncell; i++) {
            if (row_matches(d, page, cell_off(&bt, page, i), q, id_col, want_col,
                            scratch, scratch_cap, out, cap, out_len))
                return 1;
        }
        return 0;
    }

    /* Interior: each cell opens with a child page number, and the right-most child
     * is in the header rather than in a cell. Walk them all -- this does not use
     * the keys, because it is looking for one row rather than for a range. */
    for (i = 0; i < bt.ncell; i++) {
        size_t co = cell_off(&bt, page, i);

        if (co + 4 > d->usable)
            return 0;
        if (tree_find(d, be32(page + co), q, id_col, want_col, scratch,
                      scratch_cap, out, cap, out_len, depth + 1))
            return 1;
    }
    {
        uint32_t right = be32(page + bt.hdr + 8);

        if (right != 0 &&
            tree_find(d, right, q, id_col, want_col, scratch, scratch_cap,
                      out, cap, out_len, depth + 1))
            return 1;
    }
    return 0;
}

/* The root page of `table`, and its CREATE TABLE statement, from sqlite_master.
 * sqlite_master's own columns are type, name, tbl_name, rootpage, sql. */
struct master {
    int      found;
    uint32_t root;
    char     sql[SQL_MAX];
    size_t   sql_len;
};

static int master_find(const struct db *d, uint32_t pgno, const char *table,
                       size_t table_len, uint8_t *scratch, size_t scratch_cap,
                       struct master *m, int depth)
{
    struct btree bt;
    const uint8_t *page = NULL;
    uint32_t i;

    if (depth > 16 || m->found || btree_open(d, pgno, &bt, &page) != 0)
        return 0;

    if (!bt.leaf) {
        for (i = 0; i < bt.ncell; i++) {
            size_t co = cell_off(&bt, page, i);

            if (co + 4 > d->usable)
                return 0;
            master_find(d, be32(page + co), table, table_len, scratch,
                        scratch_cap, m, depth + 1);
            if (m->found)
                return 1;
        }
        {
            uint32_t right = be32(page + bt.hdr + 8);

            if (right != 0 &&
                master_find(d, right, table, table_len, scratch, scratch_cap,
                            m, depth + 1))
                return 1;
        }
        return 0;
    }

    for (i = 0; i < bt.ncell; i++) {
        size_t plen = 0, nlen = 0, slen = 0;
        uint64_t st;
        const uint8_t *v;
        int64_t root;
        size_t k;

        if (cell_payload(d, page, cell_off(&bt, page, i), scratch, scratch_cap,
                         &plen) != 0)
            continue;
        /* name (column 1) has to be this table, spelled exactly. */
        if (rec_col(scratch, plen, 1, &st, &v, &nlen) != 0)
            continue;
        if (st < 13 || (st % 2) == 0)
            continue;
        if (nlen != table_len || memcmp(v, table, table_len) != 0)
            continue;
        if (!rec_int(scratch, plen, 3, &root) || root < 1)
            continue;
        if (rec_col(scratch, plen, 4, &st, &v, &slen) != 0)
            continue;
        if (st < 13 || (st % 2) == 0 || slen >= SQL_MAX)
            continue;
        for (k = 0; k < slen; k++)
            m->sql[k] = (char)v[k];
        m->sql[slen] = '\0';
        m->sql_len = slen;
        m->root = (uint32_t)root;
        m->found = 1;
        return 1;
    }
    return 0;
}

/* ---- the entry point ------------------------------------------------------- */

int mod_sqlite_find_text(const uint8_t *db, size_t len,
                         const struct mod_sqlite_query *q,
                         uint8_t *scratch, size_t scratch_cap,
                         char *out, size_t cap, size_t *out_len)
{
    struct db d;
    struct master m;
    int id_col, want_col;

    if (!db || !q || !q->table || !q->id_column || !q->column ||
        !scratch || scratch_cap == 0 || !out || cap == 0 || !out_len)
        return 0;
    if (db_open(&d, db, len) != 0)
        return 0;
    memset(&m, 0, sizeof(m));
    if (!master_find(&d, 1, q->table, q->table_len, scratch, scratch_cap, &m, 0))
        return 0;
    id_col = column_index(m.sql, m.sql_len, q->id_column, q->id_column_len);
    want_col = column_index(m.sql, m.sql_len, q->column, q->column_len);
    if (id_col < 0 || want_col < 0)
        return 0;
    return tree_find(&d, m.root, q, id_col, want_col, scratch, scratch_cap,
                     out, cap, out_len, 0);
}
