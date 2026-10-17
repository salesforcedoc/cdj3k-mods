// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * usbsave/track_save.c - USB Save: the arm flag, the save thread, and the file.
 *
 * The shape is: the message thread notices, the worker copies.
 *
 *   mod_usbsave_pump  [message]  reads /run/cdj3k-usbsave and the loaded track's
 *                                path, and when both are new, hands the path to
 *                                the thread. Cheap and non-blocking: the flag is
 *                                stat'd on a counter and the path is a memcpy.
 *   usbsave_main      [worker]   waits for the source to finish arriving, finds a
 *                                writable stick, and copies the file onto it.
 *
 * Three decisions worth stating:
 *
 *   THE FILE IS COPIED VERBATIM, NOT RE-ENCODED. An earlier version decoded the
 *   track through the deck's own decoder and wrote FLAC or WAV through its own
 *   encoder. That never worked for the tracks this is used on. stem_decode_pull()
 *   with a NULL sink builds the chain and reads the converter's output length
 *   WITHOUT reading any audio, so the length query always succeeded and promised a
 *   healthy frame count; the real decode then failed its first read (fileRead
 *   rc=-96) and delivered 0 frames, which the save reported as "failed, nothing
 *   written" and deleted. Copying needs no decoder, keeps whatever container the
 *   track really is, and is as lossless as the source allows: these are lossy
 *   already, and re-encoding one to FLAC turned a 7 MB file into 60 MB.
 *
 *   THE SOURCE MUST HAVE STOPPED GROWING. A streamed track (Beatport caches them
 *   under /mnt/StreamingTemp/...) is still arriving while it plays, so at the
 *   moment the track is loaded the file is incomplete. Copying then would put a
 *   truncated file on the stick that looks fine until it is played, so the worker
 *   waits for the size to sit still, and copies again if the source grew while it
 *   was reading.
 *
 *   THE DESTINATION IS THE FIRST WRITABLE VOLUME, not the one holding the track.
 *   A track on a linked player arrives over Pro DJ LINK as a read-only FUSE mount,
 *   and a streamed one lives in a cache the deck may evict; both still get saved,
 *   to this deck's own stick.
 */
#include "usbsave/usbsave.h"
#include "theme/theme.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "core/mod_core.h"
#include "kit/band.h"
#include "kit/mod.h"
#include "stem/stem.h"
#include "theme/artwork.h"
#include "usbsave/json.h"
#include "usbsave/key.h"
#include "usbsave/mp4.h"
#include "usbsave/png.h"
#include "usbsave/probe.h"
#include "usbsave/sqlite.h"

/* Written by cdj3k-cfgd; its PRESENCE is the signal. /run is tmpfs, so this
 * cannot survive a boot. Its contents used to name the container to encode into;
 * the file is copied verbatim now, so only its presence is read. */
#define USBSAVE_FLAG     "/run/cdj3k-usbsave"
#define USBSAVE_DIR      "USB_SAVE"

/* How long the source must sit unchanged before it counts as fully loaded, and
 * how long we will wait for that. A streamed track arrives while it plays, so this
 * watches the deck's own download rather than a clock we control; the cap is what
 * stops a track that never settles from holding the save thread for ever. */
#define COPY_POLL_MS     500
#define COPY_STABLE_MS   5000
#define COPY_MAX_WAIT_MS 120000
#define COPY_RETRIES     2
#define COPY_BUF         (256 * 1024)   /* one read/write of a copy */

/* Where the deck keeps what it streamed: the audio file, and beside it the track's
 * metadata and cover. See read_cached_meta(). The walk over it is bounded in both
 * directions because it runs on the save thread, which must not be held for long by
 * a cache that has grown or gone deep. */
#define STREAM_ROOT        "/mnt/StreamingTemp"
#define CACHE_WALK_DEPTH   8
#define CACHE_WALK_ENTRIES 4096
#define CACHE_LIST_SHOWN   80
#define COVER_MAX_BYTES    (4 * 1024 * 1024)

/* The freeform atom a key rides in: four dashes, then the mean and name that say
 * WHICH freeform field it is. "initialkey" is the name every tagger and player
 * agrees on; the mean is Apple's, which is what that name was defined against. */
#define KEY_MEAN           "com.apple.iTunes"
#define KEY_NAME           "initialkey"

/* ---- the request, message thread -> worker --------------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static char            g_req[STEM_CACHE_PATH_MAX];
static struct kit_band_meta g_req_meta;    /* what the deck calls that track */
static int64_t         g_req_seen;         /* when the pump first saw this track */
static int             g_req_set;
static int             g_stop;

/* The last path handed over, so a track already saved is not saved again when
 * the pump next ticks. Message thread only. */
static char            g_handed[STEM_CACHE_PATH_MAX];

/* ---- finding somewhere to put it ------------------------------------------- *
 *
 * Deliberately its own walk rather than stem_media_first_root(): that one is
 * documented [worker] and lives in cache.c, whose mount bookkeeping belongs to
 * the stem worker. Reading /proc/self/mounts from a thread of our own is a few
 * lines and cannot interfere with it. */

static int is_mount_point(const char *path)
{
    char parent[STEM_CACHE_PATH_MAX];
    struct stat st, up;
    const char *slash;

    if (stat(path, &st) != 0)
        return 0;
    slash = strrchr(path, '/');
    if (!slash || slash == path)
        return 0;
    if ((size_t)(slash - path) >= sizeof(parent))
        return 0;
    memcpy(parent, path, (size_t)(slash - path));
    parent[slash - path] = '\0';
    if (stat(parent, &up) != 0)
        return 0;
    /* A directory whose device differs from its parent's is a mount. */
    return st.st_dev != up.st_dev;
}

static int is_writable(const char *path)
{
    struct statvfs vfs;
    char probe[STEM_CACHE_PATH_MAX];

    if (statvfs(path, &vfs) == 0 && (vfs.f_flag & ST_RDONLY))
        return 0;
    /* statvfs is advisory and can lie about a FUSE mount; asking the filesystem
     * itself is the answer that counts. mkdir is idempotent, so this costs
     * nothing when the directory is already there. */
    if (snprintf(probe, sizeof(probe), "%s/" USBSAVE_DIR, path) >= (int)sizeof(probe))
        return 0;
    if (mkdir(probe, 0755) == 0 || errno == EEXIST) {
        struct stat st;

        return stat(probe, &st) == 0 && S_ISDIR(st.st_mode);
    }
    return 0;
}

/* The first writable mount under the USB or SD base, or 0 when there is none. */
static int find_volume(char *out, size_t cap)
{
    static const char *const bases[] = { "/media/usb", "/media/sd" };
    size_t b;

    for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
        DIR *dir = opendir(bases[b]);
        struct dirent *de;

        if (!dir)
            continue;
        while ((de = readdir(dir)) != NULL) {
            char path[STEM_CACHE_PATH_MAX];

            if (de->d_name[0] == '.')
                continue;
            if (snprintf(path, sizeof(path), "%s/%s", bases[b], de->d_name) >=
                (int)sizeof(path))
                continue;
            if (!is_mount_point(path) || !is_writable(path))
                continue;
            closedir(dir);
            snprintf(out, cap, "%s", path);
            return 1;
        }
        closedir(dir);
    }
    return 0;
}

/* ---- naming --------------------------------------------------------------- */

/* "Artist - Title", which is what a DJ expects to find in a folder of tracks,
 * or the source's own basename when it carries no tags at all -- a stream, or a
 * file tagged by something other than rekordbox.
 *
 * Two tracks by the same artist with the same title collide on purpose: the
 * second is skipped, the same as re-loading one already saved. */
static void output_name(const char *src, const struct mod_probe *p,
                        const char *ext, char *out, size_t cap)
{
    char stem[256];
    size_t n = 0, i;

    if (p->artist[0] && p->title[0])
        snprintf(stem, sizeof(stem), "%s - %s", p->artist, p->title);
    else if (p->title[0])
        snprintf(stem, sizeof(stem), "%s", p->title);
    else {
        const char *base = strrchr(src, '/');

        base = base ? base + 1 : src;
        for (i = 0; base[i] && base[i] != '.' && i + 1 < sizeof(stem); i++)
            stem[i] = base[i];
        stem[i] = '\0';
    }

    /* Whatever a tag contained, what comes out has to be a usable filename. The
     * stick is exFAT, and a title is the deck's rather than ours -- Beatport's own
     * titles carry ':' and '*' often enough that a save failed outright with EINVAL
     * from open() on one. exFAT refuses these characters, so they are replaced
     * rather than passed through: a name that cannot be created means a track that
     * is not saved at all. `/` and the control characters were already handled;
     * these are the rest of what FAT and exFAT reject. */
    for (i = 0; stem[i] && n + 1 < cap; i++) {
        char c = stem[i];

        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            c == '\n' || c == '\r' || (unsigned char)c < 32)
            c = '_';
        out[n++] = c;
    }
    if (n == 0)
        out[n++] = '-';                     /* never an empty name */
    snprintf(out + n, cap - n, "%s", ext);
}

/* The title bar carries a placeholder while nothing is loaded, and the pump can
 * read it in the moment before the app populates it -- a save named "Not Loaded.m4a"
 * happened once. Anything the bar shows before a track is what these are, and each
 * reads as "the bar said nothing" so the cached title (or the source's name) is
 * used instead. */
static int is_placeholder(const char *s)
{
    static const char *const words[] = { "Not Loaded", "No Track", "Loading" };
    size_t i;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcasecmp(s, words[i]) == 0)
            return 1;
    }
    return 0;
}

/* ---- waiting for the source to finish arriving ------------------------------ */

/* CLOCK_MONOTONIC, not the wall clock: the shim time-shifts gettimeofday. */
static int64_t usbsave_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* Wait for `path` to stop growing, and return the size it settled at. Returns -1
 * when the wait ended with nothing to copy -- the file never appeared, or the DJ
 * loaded something else while we waited. `gave_up` reports the one case where
 * there IS a file but we stopped waiting for it to finish.
 *
 * The size is the whole test. What a half-downloaded track looks like is a file
 * that is still being appended to, and nothing in the container says how long it
 * should be without decoding it -- which is the thing that does not work here. */
static int64_t wait_source_stable(const char *path, int *gave_up)
{
    struct stat st;
    int64_t last = -1, stable = 0, waited = 0;

    *gave_up = 0;
    for (;;) {
        if (stat(path, &st) == 0) {
            int64_t size = (int64_t)st.st_size;

            if (size == last) {
                stable += COPY_POLL_MS;
                if (stable >= COPY_STABLE_MS)
                    return size;
            } else {
                last = size;            /* still arriving: start the quiet run over */
                stable = 0;
            }
        }
        if (waited >= COPY_MAX_WAIT_MS) {
            *gave_up = last >= 0;
            return last;
        }
        /* The same generation check stem's own waits use, for the same reason: a
         * track loaded while this one is being waited for must not be held behind
         * it. This is ABORT, not BAD -- nothing is wrong with the file. */
        if (!stem_job_load_wanted())
            return -1;
        usleep(COPY_POLL_MS * 1000);
        waited += COPY_POLL_MS;
    }
}

/* ---- putting the tags into the copy -----------------------------------------
 *
 * When the deck knows something about the track, the copy is written with the
 * container's `moov` replaced by one carrying that: the same bytes with a `udta`
 * grown inside it. When the deck knows nothing, or when the container is not what
 * this understands, it is written verbatim instead -- which is exactly what
 * shipped before and what a DJ would rather have than no file at all.
 *
 * mp4.c does the splice and refuses anything it cannot verify; this is the file
 * half: find `moov`, give it the tags, and stream the result out. */

/* Read exactly `len` bytes at `off`. pread so a short read is an error rather
 * than a loop that would quietly leave a hole. */
static int read_at(int fd, void *buf, size_t len, off_t off)
{
    uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(fd, p + done, len - done, off + (off_t)done);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

/* A bounded write loop. The same shape the old encoder sink had, and the reason
 * copy_range does not use `write` directly. */
static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t w = write(fd, p + done, len - done);

        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

/* Copy `len` bytes from `off` in `in` to the end of `out`. */
static int copy_range(int in, int out, off_t off, int64_t len)
{
    uint8_t buf[COPY_BUF];

    while (len > 0) {
        size_t want = len < (int64_t)sizeof(buf) ? (size_t)len : sizeof(buf);
        size_t done = 0;

        if (read_at(in, buf, want, off) != 0)
            return -1;
        while (done < want) {
            ssize_t w = write(out, buf + done, want - done);

            if (w > 0) {
                done += (size_t)w;
                continue;
            }
            if (w < 0 && errno == EINTR)
                continue;
            return -1;
        }
        off += (off_t)want;
        len -= (int64_t)want;
    }
    return 0;
}

/* A top-level box, as the header walk reads it. */
struct box_hdr {
    int64_t off;
    int64_t size;
    int64_t hdr;        /* 8, or 16 for the 64-bit size form */
    char    type[5];
};

static int read_box_hdr(int fd, int64_t off, int64_t limit, struct box_hdr *b)
{
    uint8_t raw[16];
    int i;

    if (off + 8 > limit || read_at(fd, raw, 16, off) != 0)
        return -1;
    b->off = off;
    b->hdr = 8;
    b->size = (int64_t)(((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
                        ((uint32_t)raw[2] << 8) | raw[3]);
    if (b->size == 1) {
        b->size = 0;
        b->hdr = 16;
        for (i = 0; i < 8; i++)
            b->size = (b->size << 8) | raw[8 + i];
    } else if (b->size == 0) {
        b->size = limit - off;      /* "to the end", legal only as the last box */
    }
    memcpy(b->type, raw + 4, 4);
    b->type[4] = '\0';
    if (b->size < b->hdr || off + b->size > limit)
        return -1;
    return 0;
}

/* Write `src_path`'s bytes into `dst`, with `moov` replaced by a tagged one.
 * Returns 0 when the tagged copy was written, -1 when anything at all was
 * unexpected -- `dst` is then the caller's to remove, and it falls back.
 *
 * `why` receives WHICH gate refused. Every failure here ends the same way, in a
 * verbatim copy with no tags and no cover, and the log used to say only "no tag
 * rewrite" for all of them -- so a source that is fragmented, one whose box sizes are
 * torn, and one the tag writer declined all read identically, and a whole save's worth
 * of metadata was lost with nothing to say which. One gate, one sentence. */
static int copy_with_tags(int dst, const char *src_path,
                          const struct mod_mp4_tag *tags, int ntags,
                          int64_t size, char *why, size_t why_cap)
{
    struct box_hdr moov, b;
    int64_t moov_at = -1, moov_size = 0, mdat_at = -1, at = 0;
    uint8_t *moov_buf = NULL, *new_moov = NULL;
    int64_t new_len, grew = 0;
    size_t cap;
    int in, rc = -1;

    if (why && why_cap)
        why[0] = '\0';
    /* snprintf runs before the jump, so errno is still the failing call's -- the
     * cleanup at `out` is what would lose it. */
#define FAIL(...) do { if (why && why_cap) snprintf(why, why_cap, __VA_ARGS__); \
                       goto out; } while (0)

    in = open(src_path, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        FAIL("source open: %s", strerror(errno));

    /* Walk the top level. A fragmented file (`moof`) is refused outright: its
     * sample offsets live in the fragments, not in `moov`, and getting those
     * wrong is a file that plays as noise. */
    while (at < size) {
        if (read_box_hdr(in, at, size, &b) != 0)
            FAIL("box header at %lld of %lld", (long long)at, (long long)size);
        if (strcmp(b.type, "moof") == 0)
            FAIL("fragmented: moof at %lld", (long long)b.off);
        if (strcmp(b.type, "moov") == 0) {
            moov_at = b.off;
            moov_size = b.size;
            moov.hdr = b.hdr;
        }
        if (strcmp(b.type, "mdat") == 0 && mdat_at < 0)
            mdat_at = b.off;
        at += b.size;
    }
    if (moov_at < 0)
        FAIL("no moov in %lld bytes", (long long)size);
    if (moov_size <= moov.hdr)
        FAIL("moov too small: %lld bytes", (long long)moov_size);

    moov_buf = malloc((size_t)moov_size);
    /* Room for exactly the tags being written rather than a guess: each is its
     * atom header, its `data` child's header, and the payload. A cover is the
     * reason this cannot be a small constant -- 320x240 of PNG is ~230 KB. */
    {
        size_t need = 64;               /* udta/meta/hdlr/ilst, if any is created */
        int t;

        for (t = 0; t < ntags; t++) {
            need += 24 + tags[t].value_len;
            if (tags[t].free_mean)
                need += 12 + tags[t].free_mean_len;
            if (tags[t].free_name)
                need += 12 + tags[t].free_name_len;
        }
        if ((uint64_t)need > (uint64_t)-1 - moov_size)
            FAIL("tag payload overflows a size_t");
        cap = (size_t)moov_size + need;
    }
    new_moov = malloc(cap);
    if (!moov_buf || !new_moov)
        FAIL("out of memory for %zu bytes of moov", cap);
    if (read_at(in, moov_buf, (size_t)moov_size, moov_at) != 0)
        FAIL("moov read at %lld: %s", (long long)moov_at, strerror(errno));

    {
        int ref = MOD_MP4_WHY_NONE;
        uint64_t ref_at = 0;

        new_len = mod_mp4_add_tags_why(moov_buf, (size_t)moov_size, tags, ntags,
                                       mdat_at > moov_at, new_moov, cap, &grew,
                                       &ref, &ref_at);
        /* 0 is not a refusal. It means every field is already in the file, and the
         * verbatim copy this falls back to is then exactly right -- the tags are
         * already there and nothing needed writing. Only -1 is a refusal, and `ref`
         * is which gate: the two that matter here want opposite fixes. A source whose
         * EXISTING ilst will not walk is a parsing limit worth working around; one
         * whose chunk offsets will not rewrite is a file this must not touch. */
        if (new_len == 0)
            FAIL("nothing to add (every field already present)");
        if (new_len < 0)
            FAIL("add_tags refused: %s at %llu", mod_mp4_why_name(ref),
                 (unsigned long long)ref_at);
    }

    /* The bytes before `moov` do not move, so they can go straight out; the
     * media after it moves only if `moov` was in front of it, which
     * mod_mp4_add_tags has already accounted for in the offsets it rewrote. */
    if (copy_range(in, dst, 0, moov_at) != 0)
        FAIL("prefix copy: %s", strerror(errno));
    if (write_all(dst, new_moov, (size_t)new_len) != 0)
        FAIL("moov write: %s", strerror(errno));
    if (copy_range(in, dst, moov_at + moov_size, size - (moov_at + moov_size)) != 0)
        FAIL("media copy: %s", strerror(errno));
    if (fsync(dst) != 0)
        FAIL("fsync: %s", strerror(errno));
    MDBG("usbsave: tagged %s (%lld bytes of moov, +%lld)\n", src_path,
         (long long)moov_size, (long long)grew);
    rc = 0;

out:
    free(moov_buf);
    free(new_moov);
    close(in);
    return rc;
}
#undef FAIL

/* The title atom's name, as raw bytes: 0xA9 is outside a plain char's range, so a
 * string literal for it is both wrong and a warning, and it is a four-character
 * code rather than a C string anyway. The rest are the same shape. */
static const char TAG_TITLE[4]  = { (char)0xA9, 'n', 'a', 'm' };
static const char TAG_ARTIST[4] = { (char)0xA9, 'A', 'R', 'T' };
static const char TAG_ALBUM[4]  = { (char)0xA9, 'a', 'l', 'b' };
static const char TAG_GENRE[4]  = { (char)0xA9, 'g', 'e', 'n' };
static const char TAG_DATE[4]   = { (char)0xA9, 'd', 'a', 'y' };
static const char TAG_KEY[4]    = { '-', '-', '-', '-' };
/* Comment. Nothing to do with a comment on the track: it carries the artwork's own
 * URL, taken from the deck's cached record for the release image. The deck only ever
 * fetches that image at the size it displays -- 240x240 for a streamed track -- and
 * the URL names the size, so writing it down is what makes the larger versions the
 * store offers findable again later. Chosen over the composer atom, which was here
 * first and is the field a reader is least likely to show. */
static const char TAG_COMMENT[4] = { (char)0xA9, 'c', 'm', 't' };

/* Composer. The same artwork URL as TAG_COMMENT, in BOTH fields on purpose: the two
 * are what different readers actually show, and this mod cannot know which one is
 * looking at the file. It was the composer alone first, then the comment alone, and
 * the user asked for both -- so a file now carries the pointer twice rather than
 * betting on one reader's idea of where a URL belongs. */
static const char TAG_COMPOSER[4] = { (char)0xA9, 'w', 'r', 't' };

/* The deck's BPM readout is a display string -- "112.0" -- and tmpo is hundredths
 * of a BPM. The readout carries one decimal, so the hundredths digit is a zero we
 * supply rather than one we invent. Returns 0 when there is no number there. */
static int parse_bpm(const char *s)
{
    int whole = 0, frac = -1;

    if (!s)
        return 0;
    while (*s >= '0' && *s <= '9' && whole < 10000) {
        whole = whole * 10 + (*s - '0');
        s++;
    }
    if (whole <= 0)
        return 0;
    if (*s == '.' && s[1] >= '0' && s[1] <= '9')
        frac = s[1] - '0';
    return whole * 100 + (frac < 0 ? 0 : frac * 10);
}

/* Write one copy of `path` into `tmp`: tagged when the deck said what the track
 * is, verbatim when it did not, and verbatim as well when the container turned out
 * not to be something mp4.c will vouch for. A file that saves without tags is a
 * disappointment; a file that saves corrupted is a broken track, so every
 * unexpected thing ends the same way. */
static int write_copy(const char *path, const char *tmp,
                      const struct mod_mp4_tag *tags, int ntags, int64_t size)
{
    int dst;

    if (ntags > 0) {
        char why[160];

        dst = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (dst < 0)
            return -1;
        if (copy_with_tags(dst, path, tags, ntags, size, why, sizeof(why)) == 0) {
            close(dst);
            return 0;
        }
        close(dst);
        /* The gate, not just the outcome. This line used to read the same for eleven
         * different refusals, and a verbatim copy carries neither the tags nor the
         * cover -- so "which one" is the whole of the diagnosis. */
        MDBG("usbsave: %s: no tag rewrite (%s), copying it as it is\n", path,
             why[0] ? why : "unknown");
        unlink(tmp);
    }
    return stem_copy_file(path, tmp);
}

/* ---- what the deck cached about this track --------------------------------
 *
 * A streamed track's file carries no tags, and the title bar names it but has no
 * artist on it. The deck fetched the track's metadata and its cover over HTTP and
 * cached both on its own eMMC under /mnt/StreamingTemp; this reads them back, so
 * the artist and the genre that a DJ's folder is organised by come from the
 * source the deck's own info screen drew from. NOTHING LEAVES THE MACHINE: the
 * cover's `image.uri` is a Beatport URL and it is read only to learn the cached
 * file's name, never fetched.
 *
 * Both are found by CONTENT rather than by a fixed path. The cache's layout is the
 * deck's business and it is not in any spec, so a walk looks at every JSON under
 * the root for the one whose "id" is this track's, and at every image beside it
 * for one named after the cover's own hash. A walk that finds nothing is the same
 * as a deck that has no cache -- the copy is still saved, just without the fields
 * only the cache knows.
 */

/* Read up to `cap` bytes of `path` into a fresh buffer, NUL-terminated for the
 * text callers and returned with its length for the binary one. NULL on any
 * failure. */
static char *read_capped(const char *path, size_t cap, size_t *len)
{
    char *buf = malloc(cap + 1);
    size_t done = 0;
    int fd;

    if (!buf)
        return NULL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        free(buf);
        return NULL;
    }
    while (done < cap) {
        ssize_t n = read(fd, buf + done, cap - done);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fd);
    buf[done] = '\0';
    *len = done;
    return buf;
}

/* The deck names a streamed track by its own Beatport id -- .../TrackLoad/
 * 29191181.m4a -- and that id is the only handle the cache can be asked about.
 * Returns 0 for a file whose name is not all digits, which is every other kind of
 * track and means there is nothing to look up. */
static int track_id_of(const char *path, char *out, size_t cap)
{
    const char *base = strrchr(path, '/');
    const char *dot;
    size_t n, i;

    base = base ? base + 1 : path;
    dot = strchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n + 1 > cap)
        return 0;
    for (i = 0; i < n; i++) {
        if (base[i] < '0' || base[i] > '9')
            return 0;
    }
    memcpy(out, base, n);
    out[n] = '\0';
    return 1;
}

/* A file the walk is looking for. Returns non-zero to stop the walk on it. */
typedef int (*cache_match_fn)(const char *path, const char *name, void *ctx);

/* Depth- and entry-bounded walk of the cache. On the first match, `out` receives
 * the file's full path and the walk stops. lstat, not stat: a symlink is followed
 * by nothing here, so a link loop cannot spin the save thread. */
static int walk_cache(const char *dir, int depth, int *budget,
                      cache_match_fn fn, void *ctx, char *out, size_t cap)
{
    DIR *d;
    struct dirent *de;

    if (depth > CACHE_WALK_DEPTH || *budget <= 0)
        return 0;
    d = opendir(dir);
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        char path[STEM_CACHE_PATH_MAX];
        struct stat st;

        if (de->d_name[0] == '.')
            continue;
        if (--*budget <= 0)
            break;
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >=
            (int)sizeof(path))
            continue;
        if (lstat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (walk_cache(path, depth + 1, budget, fn, ctx, out, cap)) {
                closedir(d);
                return 1;
            }
        } else if (S_ISREG(st.st_mode) && fn(path, de->d_name, ctx)) {
            snprintf(out, cap, "%s", path);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static int ext_is(const char *name, const char *const *exts)
{
    const char *dot = strrchr(name, '.');
    size_t i;

    if (!dot)
        return 0;
    for (i = 0; exts[i]; i++) {
        if (strcasecmp(dot, exts[i]) == 0)
            return 1;
    }
    return 0;
}

/* A track's metadata is NOT a .json file. Read off the deck's own cache, it is the
 * `json` column of a row in the `tracks` table inside the Beatport client's
 * database:
 *
 *   /mnt/StreamingTemp/Beatport/BeatportClient0N/812622/sqlite3.db
 *
 * with per-track rows beside it under tr/<id>.trk (0 bytes each) -- so a search for
 * a .json by name finds nothing, which is what the first deck run showed. The row
 * is found by its own id and the value read out by usbsave/sqlite.c, which walks
 * the database the way SQLite does. That module's header says why reading the file
 * as flat bytes does not work, and it is worth reading before changing anything
 * here: the short version is that a value too big for its page lives on overflow
 * pages chained by page NUMBER, so they are not next to each other in the file. */
#define META_FILE_MAX   (8 * 1024 * 1024)   /* nothing bigger is a cache database */
#define META_TOTAL_MAX  (48 * 1024 * 1024)  /* work for one save, all files together */
#define META_OBJECT_MAX (32 * 1024)         /* the json value we will accept      */
#define META_ROW_MAX    (48 * 1024)         /* one whole record while it decodes  */

/* The deck writes the media it streams under these, and they are megabytes apiece.
 * Nothing in one can be a record, and reading them would spend the walk's whole
 * budget on audio. */
static int is_media_name(const char *name)
{
    static const char *const exts[] = {
        ".m4a", ".mp3", ".wav", ".flac", ".aac", ".aif", ".aiff", ".ogg", NULL
    };

    return ext_is(name, exts);
}

/* The search, one save's worth. */
struct meta_seek {
    long    id;
    size_t  scanned;
    size_t  len;
    char    json[META_OBJECT_MAX];  /* the value that was read        */
    uint8_t row[META_ROW_MAX];      /* one whole record while it decodes */
};

static int meta_file_match(const char *path, const char *name, void *ctx)
{
    struct meta_seek *w = ctx;
    struct mod_sqlite_query q;
    struct stat st;
    uint8_t *buf;
    size_t len = 0, got = 0;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    if (is_media_name(name))
        return 0;
    if (st.st_size <= 0 || (size_t)st.st_size > META_FILE_MAX)
        return 0;
    if (w->scanned + (size_t)st.st_size > META_TOTAL_MAX)
        return 0;
    buf = (uint8_t *)read_capped(path, (size_t)st.st_size, &len);
    if (!buf)
        return 0;
    w->scanned += len;
    /* The row this track's metadata lives in, and the lengths the reader needs
     * because strlen is not in the mods' libc subset. */
    q.table = "tracks";
    q.table_len = sizeof("tracks") - 1;
    q.id_column = "id";
    q.id_column_len = sizeof("id") - 1;
    q.column = "json";
    q.column_len = sizeof("json") - 1;
    q.id = w->id;
    /* Only a database can answer this, and the reader refuses anything that is not
     * one -- so there is nothing to check here first. It returns the value whole or
     * not at all: a record it cannot read is a "no", never a partial one. */
    if (mod_sqlite_find_text(buf, len, &q, w->row, sizeof(w->row),
                             w->json, sizeof(w->json), &got)) {
        w->len = got;
        free(buf);
        return 1;
    }
    free(buf);
    return 0;
}

/* A file whose name carries this substring and whose extension is an image the
 * ilst can type -- how the cached cover is recognised, by the hash its own URL
 * names it with. Only the two formats an ilst `data` atom has a type for: a
 * webp would have to be labelled JPEG or nothing, and a mislabelled image is
 * worse than falling back to the render. */
struct cover_key {
    const char *needle;
};

static int cover_match(const char *path, const char *name, void *ctx)
{
    static const char *const exts[] = { ".jpg", ".jpeg", ".png", NULL };
    struct cover_key *k = ctx;

    (void)path;
    return ext_is(name, exts) && strstr(name, k->needle) != NULL;
}

/* What the cache adds to what the title bar showed. The cover's BYTES are not
 * held here: they are read later, where the failure paths cannot leak them. The
 * text fields are MOD_PROBE_TEXT wide because that is where they are copied to --
 * the struct the name and the tags are built from. */
struct cached_meta {
    char artist[MOD_PROBE_TEXT];
    char title[MOD_PROBE_TEXT];     /* "name (mix_name)", a fallback for the bar's */
    char genre[64];
    char album[MOD_PROBE_TEXT];
    char year[8];                   /* four digits, from the release date */
    char key[16];                   /* Camelot, "10A" */
    long bpm;
    char cover_path[STEM_CACHE_PATH_MAX];
    char cover_uri[512];           /* the artwork's own URL: the comment and the
                                    * composer field both carry it */
    int  cover_jpeg;
};

/* The artwork is release.image.uri, NOT the top-level `image`. That one is the
 * 1500x250 banner, and the deck caches the art under the release image's own hash
 * -- checked against the eMMC cache, where the file on disk is named for exactly
 * that hash and not the banner's. */
static const char *const COVER_PATH[] = { "release", "image", "uri" };

/* Where the last save's summary is left for cdj3k-cfgd to relay to the host. /run
 * is tmpfs and shared by everything in the guest, which is how a mod inside EP122
 * reaches the daemon without opening its socket. */
#define USBSAVE_LAST "/run/cdj3k-usbsave-last"

/* The basename of a URL, minus its extension: the hash the cached cover is stored
 * under. `.../1500x250/8c13fd19....png` -> `8c13fd19...`. */
static int cover_key_from_uri(const char *uri, char *out, size_t cap)
{
    const char *base = strrchr(uri, '/');
    const char *dot;
    size_t n;

    base = base ? base + 1 : uri;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n + 1 > cap)
        return 0;
    memcpy(out, base, n);
    out[n] = '\0';
    return 1;
}

/* Fill `cm` from the track's cached metadata. Every field is left empty when the
 * cache does not have it, and a track with no id, no record, or a record that is
 * not the one we lifted leaves `cm` empty and nothing more happens. */
static void read_cached_meta(const char *src_path, struct cached_meta *cm)
{
    char id[32], path[STEM_CACHE_PATH_MAX], uri[512], hash[256];
    struct meta_seek seek;
    const char *js;
    size_t len = 0;
    long want = 0;
    int budget;
    const char *p;

    memset(cm, 0, sizeof(*cm));
    if (!track_id_of(src_path, id, sizeof(id)))
        return;
    for (p = id; *p; p++)
        want = want * 10 + (*p - '0');

    memset(&seek, 0, sizeof(seek));
    seek.id = want;
    budget = CACHE_WALK_ENTRIES;
    if (!walk_cache(STREAM_ROOT, 0, &budget, meta_file_match, &seek, path, sizeof(path))) {
        MDBG("usbsave: no cached metadata for id %s (%zu bytes read)\n",
             id, seek.scanned);
        return;
    }
    js = seek.json;
    len = seek.len;
    MDBG("usbsave: metadata for id %s from %s (%zu bytes)\n", id, path, len);

    if (mod_json_str(js, len, "name", cm->title, sizeof(cm->title))) {
        char mix[64];

        if (mod_json_str(js, len, "mix_name", mix, sizeof(mix)) && mix[0]) {
            size_t t = strlen(cm->title);

            snprintf(cm->title + t, sizeof(cm->title) - t, " (%s)", mix);
        }
    }
    /* Every artist, not just the first: a track is very often credited to several,
     * and the deck's own cache keeps them comma-joined in a single column. */
    mod_json_arr_join(js, len, "artists", "name", ", ", cm->artist,
                      sizeof(cm->artist));
    mod_json_obj_str(js, len, "genre", "name", cm->genre, sizeof(cm->genre));
    /* The release is not always an object; when it is, its name is the album. */
    mod_json_obj_str(js, len, "release", "name", cm->album, sizeof(cm->album));
    mod_json_int(js, len, "bpm", &cm->bpm);
    /* The year, as the four digits of the release date the deck's own info screen
     * shows -- the re-release date when there is one, which is the one the store
     * lists the track under. */
    {
        char date[32];

        if (!mod_json_str(js, len, "new_release_date", date, sizeof(date)) &&
            !mod_json_str(js, len, "publish_date", date, sizeof(date)))
            date[0] = '\0';
        if (date[0] >= '0' && date[0] <= '9') {
            size_t k = 0;

            while (k < 4 && date[k] >= '0' && date[k] <= '9') {
                cm->year[k] = date[k];
                k++;
            }
            cm->year[k] = '\0';
        }
    }
    {
        long kn = 0;
        char letter[8];

        if (mod_json_obj_int(js, len, "key", "camelot_number", &kn) &&
            mod_json_obj_str(js, len, "key", "camelot_letter", letter, sizeof(letter)))
            snprintf(cm->key, sizeof(cm->key), "%ld%s", kn, letter);
    }

    /* The cover: the cached image whose name carries the hash of the artwork's own
     * URL -- release.image.uri, falling back to the top-level `image`. The URL is
     * read, never requested. */
    if (!mod_json_path_str(js, len, COVER_PATH, 3, uri, sizeof(uri)) &&
        !mod_json_obj_str(js, len, "image", "uri", uri, sizeof(uri)))
        uri[0] = '\0';
    if (uri[0])
        snprintf(cm->cover_uri, sizeof(cm->cover_uri), "%s", uri);
    if (uri[0] && cover_key_from_uri(uri, hash, sizeof(hash))) {
        struct cover_key key;
        const char *dot;

        key.needle = hash;
        budget = CACHE_WALK_ENTRIES;
        if (walk_cache(STREAM_ROOT, 0, &budget, cover_match, &key,
                       cm->cover_path, sizeof(cm->cover_path))) {
            dot = strrchr(cm->cover_path, '.');
            cm->cover_jpeg = dot && strcasecmp(dot, ".png") != 0;
            MDBG("usbsave: cover for id %s from %s\n", id, cm->cover_path);
        }
    }
}

/* ---- what was saved, for the host to see ----------------------------------- *
 *
 * The host cannot read the stick without mounting the live image, and mounting it
 * is exactly what causes trouble -- so the summary of each save is pushed to it
 * instead. It goes to a file in /run, which cdj3k-cfgd relays over the control
 * socket as `usbsave_saved <line>`; the same line is logged, so it is in the
 * journal and the serial console as well.
 *
 * One line, `key="value"` pairs, values flattened: a quote, a backslash or a
 * control character becomes '_', so nothing in a title can break the framing. What
 * it describes is what was actually WRITTEN, after the title bar and the cache
 * were merged -- the point is to see the result, not the inputs. */

static void flatten(char *dst, size_t cap, const char *src)
{
    size_t n = 0;

    for (; *src && n + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;

        dst[n++] = (c < 0x20 || c == 0x7f || c == '"' || c == '\\') ? '_' : (char)c;
    }
    dst[n] = '\0';
}


/* The cover is no longer dropped anywhere for inspection. `preview_cover` wrote the
 * image to /mnt/cdj3k-cover-last.<ext> so a save's embedded artwork could be seen
 * without mounting the stick, and it did its job: on 2026-10-16 it showed that image
 * to be the WRONG one. Every file was getting the same 13230-byte 240x240 -- the
 * artwork the deck happened to decode once at startup out of its own persisted cache,
 * not the loaded track's. See the cover note in save_one. */



/* session_cover_get / session_cover_put are GONE, and the reason matters more than the
 * code did. They kept the decoder capture in /run under the release's hash so a later
 * track of the same release could reuse it -- sound only if the capture were the
 * release's artwork. It was not: 2026-10-16, on the deck, every file came out with the
 * SAME image, and the user identified it as a previous track's album art. The deck
 * routes artwork through ImageFileFormat::decodeImage once per boot at most (from its
 * own persisted cache), so the capture is one arbitrary picture, and a cache of it
 * propagated that picture under every release's hash. Keying a wrong image does not
 * make it right. */

/* ---- did it actually land? ------------------------------------------------- *
 *
 * Read back the file that was just written and report which atoms are really in it.
 * Everything else this mod says about a save is what it ASKED the tag writer for: the
 * summary line is built from the same values that were passed to mod_mp4_splice, so a
 * splice that silently declined -- or a field skipped because the file already carried
 * one -- reads identically to success. Reported no image and no comment with the log
 * showing both, four times over, which is why this exists.
 *
 * Crude on purpose: a scan for the four-byte atom ids. A false positive needs those
 * exact bytes in a payload, and the interesting answer is presence or absence, not a
 * parse. */
static void verify_saved(const char *path, const char *cover_src)
{
    static uint8_t buf[65536];
    int fd;
    ssize_t n;
    size_t i;
    int have_nam = 0, have_cmt = 0, have_tmpo = 0, have_covr = 0;
    size_t covr_at = 0;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        MWARN("usbsave: written file cannot be reopened to verify it (%s)\n", path);
        return;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) {
        MWARN("usbsave: written file read back empty\n");
        return;
    }
    for (i = 0; i + 4 <= (size_t)n; i++) {
        if (buf[i] == 0xA9 && buf[i+1] == 'n' && buf[i+2] == 'a' && buf[i+3] == 'm')
            have_nam = 1;
        if (buf[i] == 0xA9 && buf[i+1] == 'c' && buf[i+2] == 'm' && buf[i+3] == 't')
            have_cmt = 1;
        if (buf[i] == 't' && buf[i+1] == 'm' && buf[i+2] == 'p' && buf[i+3] == 'o')
            have_tmpo = 1;
        if (buf[i] == 'c' && buf[i+1] == 'o' && buf[i+2] == 'v' && buf[i+3] == 'r') {
            have_covr = 1;
            if (!covr_at)
                covr_at = i;
        }
    }
    MDBG("usbsave: VERIFY %zd bytes read back: title=%d comment=%d tmpo=%d covr=%d\n",
         n, have_nam, have_cmt, have_tmpo, have_covr);
    /* What an ilst `data` atom holds for an image: [size]['data'][type:4][locale:4]
     * [payload]. The type says 13 for JPEG and 14 for PNG, and the payload's first
     * bytes say whether what went in was a whole image at all. */
    if (have_covr && covr_at + 24 <= (size_t)n) {
        const uint8_t *d = buf + covr_at;
        MDBG("usbsave:   covr data-type=%u payload starts %02x %02x %02x %02x (source=%s)\n",
             (unsigned)d[16], d[20], d[21], d[22], d[23], cover_src);
    }
}

static void report_saved(const char *file, const struct mod_probe *tagged,
                         const struct cached_meta *cm, int bpm_int,
                         const char *key, const char *cover, int64_t took_ms,
                         const char *status)
{
    char line[768], tmpn[STEM_CACHE_PATH_MAX];
    char f[256], t[256], a[256], al[256], g[128], k[64], y[16], c[16], u[512];
    int fd;

    flatten(f, sizeof(f), file);
    flatten(t, sizeof(t), tagged->title[0] ? tagged->title : "-");
    flatten(a, sizeof(a), tagged->artist[0] ? tagged->artist : "-");
    flatten(al, sizeof(al), cm->album[0] ? cm->album : "-");
    flatten(g, sizeof(g), cm->genre[0] ? cm->genre : "-");
    flatten(k, sizeof(k), key[0] ? key : "-");
    flatten(y, sizeof(y), cm->year[0] ? cm->year : "-");
    flatten(c, sizeof(c), cover);
    flatten(u, sizeof(u), cm->cover_uri[0] ? cm->cover_uri : "-");

    snprintf(line, sizeof(line),
             "file=\"%s\" title=\"%s\" artist=\"%s\" album=\"%s\" genre=\"%s\" "
             "year=\"%s\" key=\"%s\" bpm=%d cover=%s art=\"%s\" took=%lldms status=%s",
             f, t, a, al, g, y, k, bpm_int, c, u, (long long)took_ms,
             status ? status : "-");
    MDBG("usbsave: saved %s\n", line);

    /* Written whole and then renamed, so cfgd can never relay half a line. */
    if (snprintf(tmpn, sizeof(tmpn), "%s.tmp", USBSAVE_LAST) >= (int)sizeof(tmpn))
        return;
    fd = open(tmpn, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write_all(fd, line, strlen(line)) != 0 || write_all(fd, "\n", 1) != 0) {
        close(fd);
        unlink(tmpn);
        return;
    }
    close(fd);
    if (rename(tmpn, USBSAVE_LAST) != 0)
        unlink(tmpn);
}

/* A walk of the whole cache, printed once. DIAGNOSTIC: it is what shows the
 * layout the content match above is designed around, and what would say so if a
 * deck version ever cached its metadata somewhere the match does not reach. It
 * prints the head of every small JSON, flattened so a newline cannot split a log
 * line. */
static void list_cache_rec(const char *dir, int depth, int *budget)
{
    DIR *d;
    struct dirent *de;

    if (depth > 6 || *budget <= 0)
        return;
    d = opendir(dir);
    if (!d) {
        MDBG("usbsave: %s: cannot list (%s)\n", dir, strerror(errno));
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char full[STEM_CACHE_PATH_MAX];
        struct stat st;

        if (de->d_name[0] == '.')
            continue;
        if (--*budget <= 0)
            break;
        if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >=
            (int)sizeof(full))
            continue;
        if (lstat(full, &st) != 0)
            continue;
        /* Directories, and files that hold something: the tr/ directory is full of
         * 0-byte <id>.trk placeholders and listing them buries the rest. */
        if (!S_ISDIR(st.st_mode) && st.st_size == 0)
            continue;
        MDBG("usbsave:   %s  %lld bytes%s\n", full, (long long)st.st_size,
             S_ISDIR(st.st_mode) ? "  (dir)" : "");
        if (S_ISDIR(st.st_mode))
            list_cache_rec(full, depth + 1, budget);
    }
    closedir(d);
}

static void list_streaming_dirs(void)
{
    static int done;
    int budget = CACHE_LIST_SHOWN;

    if (done)
        return;
    done = 1;
    list_cache_rec(STREAM_ROOT, 0, &budget);
    MDBG("usbsave: cache listing done (%d entries shown)\n", CACHE_LIST_SHOWN - budget);
}

/* ---- copying one track ----------------------------------------------------- */

/* How many numbered alternatives a taken name is worth trying before giving up
 * and using the source's own basename instead. A collision means two different
 * tracks share a title, which wants a handful of tries, not a directory scan. */
#define COPY_NAME_TRIES 20

/* "Artist - Title.m4a" -> a path ending in "Artist - Title (2).m4a". The number
 * goes before the extension, so the container stays last and the file keeps
 * opening as whatever it is. */
static int numbered_name(const char *name, int n, const char *dir,
                         char *out, size_t cap)
{
    const char *dot = strrchr(name, '.');
    size_t stem = dot ? (size_t)(dot - name) : strlen(name);
    int wrote = snprintf(out, cap, "%s/%.*s (%d)%s", dir, (int)stem, name, n,
                         dot ? dot : "");

    return (wrote < 0 || (size_t)wrote >= cap) ? -1 : 0;
}

/* Leave the image that went into the file where it can be LOOKED AT, named for the
 * track it was saved with.
 *
 * /mnt is the deck's own eMMC, which the host can read from a copy of emmc.qcow2
 * without mounting the stick at all. This exists because looking is the only check
 * that has ever caught a wrong cover here: the size of the write and a `covr=1`
 * read-back both say an image arrived, and neither says WHICH image. Named per track
 * so several can be compared rather than the last one overwriting the rest. */
static void cover_preview(const char *src_path, const uint8_t *bytes, size_t len)
{
    char id[32], out[96];
    int fd;

    if (!bytes || len == 0)
        return;
    if (!track_id_of(src_path, id, sizeof(id)))
        snprintf(id, sizeof(id), "unknown");
    if (snprintf(out, sizeof(out), "/mnt/cdj3k-drawn-%s.png", id) >= (int)sizeof(out))
        return;
    fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write_all(fd, bytes, len) == 0)
        MDBG("usbsave: cover left for inspection at %s (%zu bytes)\n", out, len);
    close(fd);
}

static void save_one(const char *path, const struct kit_band_meta *meta,
                     int64_t seen_ms)
{
    char volume[STEM_CACHE_PATH_MAX], dir[STEM_CACHE_PATH_MAX];
    char name[256], dest[STEM_CACHE_PATH_MAX], tmp[STEM_CACHE_PATH_MAX];
    char ext[64], key_buf[16];
    struct cached_meta cm;
    struct mod_probe tagged;
    /* Sized by the module's own limit rather than by a number chosen here: this list
     * and that capacity drifted apart once already -- see MOD_MP4_MAX_TAGS. This
     * builds TEN (title, artist, album, genre, year, tmpo, the key, the artwork's URL
     * in both the comment and the composer field, and covr), so the constant must stay
     * above that. */
    struct mod_mp4_tag tags[MOD_MP4_MAX_TAGS];
    uint8_t bpm_be[2];
    uint8_t *art = NULL;            /* the cover: a cached JPEG, or the drawn PNG */
    size_t art_len = 0;
    int art_jpeg = 0;
    const char *cover_src = "none";
    char cover_src_buf[32];         /* "<source>-<w>x<h>", named in the log line */
    int32_t aw = 0, ah = 0;         /* the drawn bitmap's size, when that is the source */
    const char *base, *dot, *key;
    int64_t size = 0, after, t0, tags_payload = 0;
    int gave_up = 0, attempt, n, tags_n = 0, bpm_x100 = 0;

    if (!find_volume(volume, sizeof(volume))) {
        MDBG("usbsave: %s loaded but no writable medium to save it to\n", path);
        return;
    }
    if (snprintf(dir, sizeof(dir), "%s/" USBSAVE_DIR, volume) >= (int)sizeof(dir))
        return;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return;

    /* Wait for the source to finish arriving first: the size it settles at does
     * what tells a name collision from the same track saved in an earlier boot,
     * so the wait has to happen before the name is resolved. */
    t0 = usbsave_now_ms();
    size = wait_source_stable(path, &gave_up);
    if (size < 0) {
        MDBG("usbsave: %s: nothing to copy, or the deck moved on while waiting\n",
             path);
        return;
    }
    if (gave_up)
        MWARN("usbsave: %s still growing after %d s (%lld bytes); copying anyway\n",
              path, COPY_MAX_WAIT_MS / 1000, (long long)size);
    else
        MDBG("usbsave: %s settled at %lld bytes after %lld ms\n", path,
             (long long)size, (long long)(usbsave_now_ms() - t0));

    /* What the deck cached about this track: the artist and genre the title bar
     * cannot show, and a title and BPM to fall back on. Empty when there is no
     * cache or no id, which leaves everything to the title bar. */
    read_cached_meta(path, &cm);

    /* The name: what the deck calls this track when it knows, and the source's own
     * basename when it does not -- which is the streamed case, and the reason these
     * files used to arrive as bare numbers. The extension is always the source's,
     * because the file is copied verbatim and the container is whatever it really
     * is; and probe.c is not consulted, because it reads MPEG headers and would
     * take a false sync inside a non-MPEG container for a real one.
     *
     * output_name() stays the only place a name is built, so it does the
     * sanitising: a title carrying a '/' cannot write into another directory. */
    memset(&tagged, 0, sizeof(tagged));
    if (meta && meta->title[0] && !is_placeholder(meta->title))
        snprintf(tagged.title, sizeof(tagged.title), "%s", meta->title);
    else if (cm.title[0])
        snprintf(tagged.title, sizeof(tagged.title), "%s", cm.title);
    if (cm.artist[0])
        snprintf(tagged.artist, sizeof(tagged.artist), "%s", cm.artist);
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    dot = strchr(base, '.');
    snprintf(ext, sizeof(ext), "%s", dot ? dot : "");
    output_name(path, &tagged, ext, name, sizeof(name));

    if (snprintf(dest, sizeof(dest), "%s/%s", dir, name) >= (int)sizeof(dest))
        return;

    /* The cover, in the order the user asked for: the 240x240 the deck drew if there
     * is one, else the larger 320x240.
     *
     * But FIRST, when it exists at all: the Beatport client's cached image whose NAME
     * carries the hash of the release's own artwork URL. That one is the only source
     * that cannot be another release's picture, because the deck's record for this
     * track is what named it. It is almost never there (Artwork/ is usually empty),
     * which is why the drawn bitmap below has to exist at all. */
    if (cm.cover_path[0]) {
        art = (uint8_t *)read_capped(cm.cover_path, COVER_MAX_BYTES, &art_len);
        if (art && art_len > 0) {
            art_jpeg = cm.cover_jpeg;
            cover_src = cm.cover_jpeg ? "cache-jpg" : "cache-png";
            MDBG("usbsave: cover for this release from %s (%zu bytes)\n",
                 cm.cover_path, art_len);
        } else {
            free(art);
            art = NULL;
            art_len = 0;
        }
    }

    /* And when the cache has nothing for this release -- which is almost always --
     * the drawn bitmap. These are the pixels juce handed to drawImage, at
     * the size the spot that showed them uses -- so it is the artwork as displayed
     * rather than the file that was fetched. Two things follow, and both are stated
     * rather than hidden: it is a re-render (scaled, and the player view's 320x240 is
     * stretched to 4:3 where the 240x240 is square and truer), and WHICH image it is
     * is not knowable here -- the browse view draws artwork for the row it has
     * highlighted, not for the track being saved. So the image that would be written
     * is also dumped to /mnt (see cover_preview below) where it can be looked at.
     *
     * The badge that once came out as a numeral is excluded by size: theme/artwork.c
     * keeps only these two sizes, and the deck draws that badge at 20x30. */
    if (theme_artwork_size(&aw, &ah) && aw > 0 && ah > 0) {
        size_t need = MOD_PNG_MAX((size_t)aw, (size_t)ah);
        uint8_t *rgb = malloc((size_t)aw * (size_t)ah * 3);

        art = (uint8_t *)malloc(need);
        if (rgb && art) {
            int64_t png;

            if (theme_artwork_rgb(rgb, (size_t)aw * (size_t)ah * 3, &aw, &ah) > 0)
                png = mod_png_rgb(rgb, (size_t)aw, (size_t)ah, art, need);
            else
                png = -1;
            free(rgb);
            if (png > 0) {
                art_len = (size_t)png;
                art_jpeg = 0;                       /* the encoder writes PNG */
                snprintf(cover_src_buf, sizeof(cover_src_buf), "drawn-%ldx%ld",
                         (long)aw, (long)ah);
                cover_src = cover_src_buf;
                MDBG("usbsave: cover from the drawn bitmap: %ldx%ld -> %zu bytes of PNG\n",
                     (long)aw, (long)ah, art_len);
            }
        }
        if (art_len == 0) {
            free(art);
            art = NULL;
        }
    }

    /* What the deck knows about this track, as tags. Anything it could not tell us
     * is simply not written, and a field the file already carries is left alone --
     * mp4.c gap-fills per field, which is the "only if it is missing" rule.
     *
     * Built BEFORE the collision check below, because that check weighs an
     * existing file against what this copy would weigh -- which is the source plus
     * these tags, not the source alone. */
    memset(tags, 0, sizeof(tags));
    tags_n = 0;
    tags_payload = 0;
    if (tagged.title[0]) {
        memcpy(tags[tags_n].name, TAG_TITLE, 4);
        tags[tags_n].type = MOD_MP4_TEXT;
        tags[tags_n].value = tagged.title;
        tags[tags_n].value_len = strlen(tagged.title);
        tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len;
        tags_n++;
    }
    if (cm.artist[0]) {
        memcpy(tags[tags_n].name, TAG_ARTIST, 4);
        tags[tags_n].type = MOD_MP4_TEXT;
        tags[tags_n].value = cm.artist;
        tags[tags_n].value_len = strlen(cm.artist);
        tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len;
        tags_n++;
    }
    if (cm.album[0]) {
        memcpy(tags[tags_n].name, TAG_ALBUM, 4);
        tags[tags_n].type = MOD_MP4_TEXT;
        tags[tags_n].value = cm.album;
        tags[tags_n].value_len = strlen(cm.album);
        tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len;
        tags_n++;
    }
    if (cm.genre[0]) {
        memcpy(tags[tags_n].name, TAG_GENRE, 4);
        tags[tags_n].type = MOD_MP4_TEXT;
        tags[tags_n].value = cm.genre;
        tags[tags_n].value_len = strlen(cm.genre);
        tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len;
        tags_n++;
    }
    if (cm.year[0]) {
        memcpy(tags[tags_n].name, TAG_DATE, 4);
        tags[tags_n].type = MOD_MP4_TEXT;
        tags[tags_n].value = cm.year;
        tags[tags_n].value_len = strlen(cm.year);
        tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len;
        tags_n++;
    }
    bpm_x100 = meta ? parse_bpm(meta->bpm) : 0;
    if (bpm_x100 == 0 && cm.bpm > 0 && cm.bpm <= 10000)
        bpm_x100 = (int)cm.bpm * 100;
    /* THE WHOLE BPM, not hundredths. Apple's `tmpo` is defined as hundredths -- 112
     * BPM is 11200 -- and a reader that follows that divides by 100 and shows 112.
     * The readers this has to satisfy do not: the saved file read back showed
     * "11200" where the deck's own screen said 112, so what goes in is the number
     * a person would say. The cost, stated plainly: a strictly-conforming reader
     * would show 1.12 BPM for a track of 112. That is the trade being made, and the
     * user has confirmed the whole number is what they want -- so this is settled,
     * and a "BPM does not show" report is NOT this line. */
    {
        /* Rounded, not truncated: the deck's readout carries a decimal, so a
         * track at 112.5 would otherwise be tagged 112 rather than 113. */
        int bpm_int = (bpm_x100 + 50) / 100;

        if (bpm_int > 0 && bpm_int <= 0xffff) {
            bpm_be[0] = (uint8_t)(bpm_int >> 8);
            bpm_be[1] = (uint8_t)bpm_int;
            memcpy(tags[tags_n].name, "tmpo", 4);
            tags[tags_n].type = MOD_MP4_INT16;
            tags[tags_n].value = bpm_be;
            tags[tags_n].value_len = sizeof(bpm_be);
            tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len;
            tags_n++;
        }
    }
    /* The key, in ONE notation: the metadata's Camelot when the lookup hit, and
     * otherwise the deck's own readout put through the wheel. The deck draws some
     * tracks as Camelot ("12A") and others as the musical key ("Ebm"), and a tag
     * that mixed the two would not sort. It rides in the freeform atom, which is
     * where a field iTunes does not define belongs. */
    key_buf[0] = '\0';
    if (cm.key[0]) {
        snprintf(key_buf, sizeof(key_buf), "%s", cm.key);
    } else if (meta && meta->key[0] &&
               !mod_key_camelot(meta->key, key_buf, sizeof(key_buf))) {
        /* Not decidable -- "D" is D major or D minor -- so the deck's own words are
         * kept rather than a side being invented. */
        snprintf(key_buf, sizeof(key_buf), "%s", meta->key);
    }
    key = key_buf;
    if (key[0]) {
        memcpy(tags[tags_n].name, TAG_KEY, 4);
        tags[tags_n].type = MOD_MP4_TEXT;
        tags[tags_n].value = key;
        tags[tags_n].value_len = strlen(key);
        tags[tags_n].free_mean = KEY_MEAN;
        tags[tags_n].free_mean_len = strlen(KEY_MEAN);
        tags[tags_n].free_name = KEY_NAME;
        tags[tags_n].free_name_len = strlen(KEY_NAME);
        tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len + 12 +
                        (int64_t)tags[tags_n].free_mean_len + 12 +
                        (int64_t)tags[tags_n].free_name_len;
        tags_n++;
    }
    /* The artwork's URL, in the comment field AND the composer field. The deck fetches
     * the cover only at the size it displays, and that size is in the URL, so this is
     * the pointer back to the larger versions the store has -- 500x500, 1400x1400 --
     * without this project ever contacting the store itself. Written after the tags
     * that came from the deck's own screens. Both fields carry it because which one a
     * reader displays is not knowable from here; see TAG_COMPOSER. */
    if (cm.cover_uri[0]) {
        memcpy(tags[tags_n].name, TAG_COMMENT, 4);
        tags[tags_n].type = MOD_MP4_TEXT;
        tags[tags_n].value = cm.cover_uri;
        tags[tags_n].value_len = strlen(cm.cover_uri);
        tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len;
        tags_n++;
        memcpy(tags[tags_n].name, TAG_COMPOSER, 4);
        tags[tags_n].type = MOD_MP4_TEXT;
        tags[tags_n].value = cm.cover_uri;
        tags[tags_n].value_len = strlen(cm.cover_uri);
        tags_payload += 8 + 16 + (int64_t)tags[tags_n].value_len;
        tags_n++;
    }
    if (art && art_len > 0) {
        memcpy(tags[tags_n].name, "covr", 4);
        tags[tags_n].type = art_jpeg ? MOD_MP4_JPEG : MOD_MP4_PNG;
        tags[tags_n].value = art;
        tags[tags_n].value_len = art_len;
        tags_payload += 8 + 16 + (int64_t)art_len;
        tags_n++;
    }

    list_streaming_dirs();

    /* Already saved, or a name that collides with a different track?
     *
     * Metadata names can repeat in a way bare numbers never did, and dropping the
     * second track silently is not acceptable. A re-load of the SAME track never
     * reaches here (the pump filters it on path), so an existing name is either
     * that track from an earlier boot or a genuine collision -- and the size is
     * what tells them apart.
     *
     * The comparison is against what OUR copy of this source would weigh: the
     * source PLUS the tags above, not the source alone. A tagged file is bigger
     * than the stream it came from, so comparing against the source by itself
     * would never match our own copy and every re-load across boots would grow a
     * (2), a (3), and so on. The residual is a field the source already carried,
     * which mp4.c does not re-add: that makes our copy smaller than this sum and
     * the same track read as a collision. */
    for (n = 2; n <= COPY_NAME_TRIES + 1; n++) {
        struct stat st;

        if (stat(dest, &st) != 0)
            break;                              /* the name is free */
        /* A RANGE, not an equality, because the exact size is not knowable here: what
         * the splice adds is the tag atoms PLUS any container it has to create MINUS
         * any field the source already carried, since mp4.c gap-fills. An equality
         * therefore recognises our own copy only by luck, and three tracks today were
         * written a second, third and fourth time because their own copies from
         * minutes earlier did not match the number computed for them.
         *
         * The bounds are the two things that CAN be reasoned about: a copy is never
         * smaller than the source it came from, and never larger than the source plus
         * every tag. The slack covers the container the splice may have to build. The
         * residual is a DIFFERENT track that generates the same name and lands inside
         * that window, which is far narrower than the old test's failure. */
        if ((int64_t)st.st_size >= size &&
            (int64_t)st.st_size <= size + tags_payload + 512) {
            const char *final = strrchr(dest, '/');

            MDBG("usbsave: %s already saved\n", name);
            /* Reported, not only logged. A skipped track is still a track the DJ
             * loaded, and a panel that says nothing about it is indistinguishable
             * from a save that failed. The name is the one it matched, which may
             * carry a "(2)" -- that is the file that was found, and is worth seeing. */
            verify_saved(dest, cover_src);
            report_saved(final ? final + 1 : dest, &tagged, &cm, bpm_x100 / 100,
                         key, cover_src, usbsave_now_ms() - seen_ms,
                         "already-saved");
            free(art);
            return;
        }
        if (numbered_name(name, n, dir, dest, sizeof(dest)) != 0)
            break;
        MDBG("usbsave: %s is a different track (%lld bytes); trying %s\n",
             name, (long long)st.st_size, dest);
    }

    if (snprintf(tmp, sizeof(tmp), "%s.part", dest) >= (int)sizeof(tmp)) {
        free(art);
        return;
    }

    /* Copied with the primitive the stem cache uses to move a finished stem from
     * tmpfs to the stick: bounded chunks, EINTR-safe, fsync before it reports
     * success, and its own output unlinked on any failure. */
    for (attempt = 0; ; attempt++) {
        struct stat st;

        t0 = usbsave_now_ms();
        if (write_copy(path, tmp, tags, tags_n, size) != 0) {
            int e = errno;
            char why[64];
            const char *final = strrchr(dest, '/');

            /* The name goes in the message: a copy that fails before the first byte
             * is usually the destination being refused rather than the source being
             * unreadable, and which of the two is only answerable if the log says
             * what it was trying to write. */
            MDBG("usbsave: cannot copy %s: %s (writing \"%s\")\n", path,
                 strerror(e), name);
            /* And the panel is told, with the reason: a save that fails silently is
             * what made a broken filename look like the deck never loading the
             * track. */
            snprintf(why, sizeof(why), "failed: %s", strerror(e));
            verify_saved(dest, cover_src);
            report_saved(final ? final + 1 : dest, &tagged, &cm, bpm_x100 / 100,
                         key, cover_src, usbsave_now_ms() - seen_ms, why);
            free(art);
            return;
        }
        /* Quiet before the copy is not quiet during it: if the deck appended to
         * the source while we were reading, what we have is torn. */
        after = -1;
        if (stat(path, &st) == 0)
            after = (int64_t)st.st_size;
        if (after < 0 || after == size)
            break;
        if (attempt + 1 >= COPY_RETRIES) {
            MWARN("usbsave: %s grew while copying (%lld -> %lld bytes); keeping a "
                  "torn copy\n", path, (long long)size, (long long)after);
            break;
        }
        MDBG("usbsave: %s grew while copying (%lld -> %lld bytes); copying again\n",
             path, (long long)size, (long long)after);
        size = after;
    }

    free(art);                  /* written into the copy; nothing else needs it */
    art = NULL;

    /* A stick can be pulled mid-write, and a file that is visible but not on the
     * medium is worse than none: the deck would find it and play a truncated
     * track. stem_copy_file()/the tag path has already fsync'd, so the rename is
     * all that is left. */
    if (rename(tmp, dest) != 0) {
        unlink(tmp);
        MDBG("usbsave: cannot rename onto %s: %s\n", dest, strerror(errno));
        return;
    }
    {
        struct stat st;
        /* The size ON THE STICK. It equals the source's only when nothing was
         * tagged, because the splice grows the file -- and this is the number
         * someone checking the stick will compare against, so it is the one to
         * report rather than the one we started from. */
        int64_t wrote = (stat(dest, &st) == 0) ? (int64_t)st.st_size : size;
        /* Two numbers, and the second is the one to watch. The first is the copy
         * alone; the second is everything the DJ waited -- from the mods first
         * seeing the track to the file being on the stick, which is the settle
         * window, the cache walk, the splice and the copy together. */
        int64_t copy_ms = usbsave_now_ms() - t0;
        int64_t took_ms = usbsave_now_ms() - seen_ms;

        MDBG("usbsave: wrote %s (%lld bytes, from %s in %lld ms; %lld ms from the "
             "track appearing)\n", dest, (long long)wrote, path,
             (long long)copy_ms, (long long)took_ms);

        /* What the deck (and the host's Debug panel) gets to see, from the stick's
         * side rather than by mounting it. Reported only once the rename has landed,
         * so a line never describes a file that is not there -- and it names the file
         * that was actually written, which is `dest`, not the name it was asked for: a
         * collision may have added a "(2)" along the way. */
        {
            const char *final = strrchr(dest, '/');

            /* ONCE. This pair was duplicated here -- verify, preview, preview,
             * verify -- which is why every successful save logged its VERIFY line
             * twice and wrote the preview image to the eMMC twice. Found while
             * removing preview_cover, 2026-10-16; nothing depended on the doubling. */
            verify_saved(dest, cover_src);
            /* The image that went in, where it can be looked at. Looked at, not
             * trusted -- see cover_preview. */
            cover_preview(path, art, art_len);
            report_saved(final ? final + 1 : dest, &tagged, &cm, bpm_x100 / 100, key,
                         cover_src, took_ms, "written");
        }
    }
}

/* ---- the save thread -------------------------------------------------------- */

static void *usbsave_main(void *unused)
{
    (void)unused;

    for (;;) {
        char path[STEM_CACHE_PATH_MAX];
        struct kit_band_meta meta;
        int64_t seen;

        pthread_mutex_lock(&g_lock);
        while (!g_req_set && !g_stop)
            pthread_cond_wait(&g_cond, &g_lock);
        if (g_stop) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        memcpy(path, g_req, sizeof(path));
        meta = g_req_meta;
        seen = g_req_seen;
        g_req_set = 0;
        pthread_mutex_unlock(&g_lock);

        save_one(path, &meta, seen);
    }
    return NULL;
}

/* ---- the message-thread half ------------------------------------------------- */

/* Read the flag. Returns 1 when armed. The contents are no longer interpreted:
 * they used to name the container to encode into, and nothing is encoded now. */
static int arm_state(void)
{
    char buf[16];
    ssize_t n;
    int fd = open(USBSAVE_FLAG, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    /* Present but empty is not an arm: cfgd writes a container name into it, so a
     * zero-length read means that write has not landed yet. */
    return n > 0;
}

void mod_usbsave_pump(void)
{
    char path[STEM_CACHE_PATH_MAX];
    struct kit_band_meta meta;

    /* NO COUNTER HERE, and deliberately. The call site already throttles this --
     * theme.c gates setFill at 1 in 1024 -- so a second gate of its own did not
     * throttle anything, it MULTIPLIED the two: one pump per 65536 setFill calls.
     * Measured on the deck, that is a period near 35 s, so a track loaded just after
     * a check waited most of a minute before its save even began, while one loaded
     * just before a check started at once. The two syscalls below, at the call
     * site's own roughly-twice-a-second rate, are not worth a delay like that. */
    if (!arm_state())
        return;

    stem_current_track_path(path, sizeof(path));
    if (!path[0] || strcmp(path, g_handed) == 0)
        return;

    /* What the deck calls this track, read HERE because it is a message-thread
     * read of the app's own component tree -- the save thread must not touch
     * that, and does not: it is handed the strings with the path. */
    kit_band_read_meta(&meta);
    kit_band_dump_labels();     /* diagnostic: what the title bar actually holds */

    /* Remembered before the hand-off, not after: the worker may finish and the
     * next pump run before this thread returns, and a second queue entry for the
     * same track would rewrite it. */
    memcpy(g_handed, path, sizeof(g_handed));

    pthread_mutex_lock(&g_lock);
    memcpy(g_req, path, sizeof(g_req));
    g_req_meta = meta;
    /* From HERE, not from the worker picking it up: this is the first instant the
     * mods know about the track, so the number the caller reports is the whole of
     * what the DJ waits -- detection, the settle window, the cache walk and the
     * copy -- rather than only the part the save thread owns. */
    g_req_seen = usbsave_now_ms();
    g_req_set = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

/* ---- install ----------------------------------------------------------------- */

static int usbsave_install(void)
{
    pthread_t th;

    /* No EP122 symbol to resolve: the loaded track's path and the decode both
     * come through stem's public API, and the flag through the filesystem. So
     * the only thing that can fail here is the thread. */
    if (pthread_create(&th, NULL, usbsave_main, NULL) != 0) {
        MDBG("usbsave: cannot start the save thread\n");
        return -1;
    }
    pthread_detach(th);
    MDBG("usbsave: armed, watching %s\n", USBSAVE_FLAG);
    return 0;
}

/* After stem, so the path it reads is being kept up to date. */
KIT_MOD(k_mod_usbsave,
        .name = "usbsave", .prio = 60, .install = usbsave_install,
        .what = "write each loaded track to USB_SAVE/ on the stick");
