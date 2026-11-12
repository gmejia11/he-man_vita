/*
 * memfile.c -- fast read-only stdio for the game's asset loading.
 *
 * The perf log showed loading stalls of 0.1-2.7 s on the render thread, all
 * in fopen()/fread():
 *   - every fopen() cost 2-6 ms: fopen_soloader() stat()s the path first (for
 *     the directory workaround) and the game probes <root>/SaveData/<file>
 *     before every asset, so each asset meant 2 failed + 2 real I/O calls
 *   - the game reads byte by byte: up to 600k fread() calls of ~6 bytes in a
 *     frame, ~4 us each through SceLibcBridge
 *
 * Files opened read-only get a light buffered stream of our own: fopen() is a
 * single FIOS open (nothing is read up front -- the game often opens files
 * just to check they exist or to read a header), small reads are served from
 * a 32 KB buffer and big ones go straight to the disk. Misses under
 * SaveData/ are answered from a cached directory listing. Writes and special
 * paths go to the original functions.
 *
 * Installed by patching the game's import table (default_dynlib) before the
 * .so is resolved, like perf.c.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * I/O goes through FIOS2, not sceIo: fios_init() installs a 8 MB RAM cache
 * filter on DATA_PATH and SceLibc's stdio benefits from it. The game re-reads
 * the same UI/asset files on every screen, so bypassing the cache with sceIo
 * made loads slower than the original stdio even with fewer calls.
 */
typedef int32_t SceFiosFH;
int32_t sceFiosFHOpenSync(const void *attr, SceFiosFH *out_fh, const char *path, const void *open_params);
int64_t sceFiosFHPreadSync(const void *attr, SceFiosFH fh, void *buf, int64_t length, int64_t offset);
int32_t sceFiosFHCloseSync(const void *attr, SceFiosFH fh);
int64_t sceFiosFHGetSize(SceFiosFH fh);

/*
 * Backend: FIOS when it checks out, plain sceIo otherwise. The first opens are
 * cross-checked against sceIo (size + first 4 KB); any mismatch switches every
 * later open to sceIo, so a FIOS API misunderstanding can't feed the game bad
 * data again (that crashed the UI loader with std::bad_alloc).
 */
volatile int memfile_use_fios = 1;          // read by perf.c
static volatile int fios_checks_left = 4;

typedef struct { int fios; int32_t h; } Handle;

static int h_pread(Handle h, void *buf, int64_t len, int64_t off) {
    if (h.fios) {
        int64_t r = sceFiosFHPreadSync(NULL, h.h, buf, len, off);
        return r < 0 ? -1 : (int)r;
    }
    return sceIoPread(h.h, buf, (SceSize)len, off);
}

static void h_close(Handle h) {
    if (h.fios < 0) return;             // no handle (content cache hit)
    if (h.fios) sceFiosFHCloseSync(NULL, h.h);
    else sceIoClose(h.h);
}

static int64_t h_size(Handle h) {
    if (h.fios) return sceFiosFHGetSize(h.h);
    SceIoStat st;
    if (sceIoGetstatByFd(h.h, &st) >= 0) return st.st_size;
    return -1;
}

static int fios_matches_sceio(SceFiosFH fh, SceUID fd) {
    static uint8_t a[4096], b[4096];
    int64_t fs = sceFiosFHGetSize(fh);
    SceIoStat st;
    if (sceIoGetstatByFd(fd, &st) < 0 || fs != st.st_size) return 0;
    int n = st.st_size < (SceOff)sizeof(a) ? (int)st.st_size : (int)sizeof(a);
    int64_t ra = sceFiosFHPreadSync(NULL, fh, a, n, 0);
    int rb = sceIoPread(fd, b, n, 0);
    return ra == n && rb == n && memcmp(a, b, n) == 0;
}

// Opens path read-only; returns 0 on success
static int h_open(const char *path, Handle *out) {
    if (memfile_use_fios) {
        SceFiosFH fh = -1;
        if (sceFiosFHOpenSync(NULL, &fh, path, NULL) >= 0 && fh >= 0) {
            if (fios_checks_left <= 0) { out->fios = 1; out->h = fh; return 0; }
            SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
            if (fd < 0) { sceFiosFHCloseSync(NULL, fh); return -1; }
            if (fios_matches_sceio(fh, fd)) {
                fios_checks_left--;
                sceIoClose(fd);
                out->fios = 1; out->h = fh;
                return 0;
            }
            sceClibPrintf("[memfile] FIOS disagrees with sceIo on %s, using sceIo\n", path);
            memfile_use_fios = 0;
            sceFiosFHCloseSync(NULL, fh);
            out->fios = 0; out->h = fd;
            return 0;
        }
        return -1;
    }
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    out->fios = 0; out->h = fd;
    return 0;
}

#define BUF_SIZE      (32 * 1024)
#define WHOLE_MAX     (64 * 1024)    // read files up to this size in one go on first read
#define MEMFILE_SLOTS 128

typedef struct {
    volatile int in_use;
    Handle fd;
    int64_t pos;            // logical stream position
    int64_t size;           // file size, -1 until needed
    uint8_t *buf;           // BUF_SIZE bytes, allocated on first small read
    int64_t buf_off;        // file offset of buf[0]
    int buf_len;            // valid bytes in buf
    int whole;              // buf holds the entire file
    int eof, err;
    int nofd;               // fd is not ours to close (content cache / pack)
    int pak;                // a file inside heman.pak: reads are clamped to size
    int64_t base;           // offset of the file's first byte in fd
    char path[160];         // for inserting into the content cache ("" = don't)
} MemFile;

static MemFile slots[MEMFILE_SLOTS];

static int m_pread(MemFile *m, void *buf, int64_t len, int64_t off) {
    if (m->pak) {
        if (off >= m->size) return 0;
        if (len > m->size - off) len = m->size - off;
    }
    return h_pread(m->fd, buf, len, m->base + off);
}

// Read by perf.c: how many streams took the fast path vs. the original stdio
volatile unsigned memfile_fast_opens, memfile_slow_opens, memfile_slow_freads;
#define SLOW_PATHS 16
char memfile_slow_paths[SLOW_PATHS][96];
volatile unsigned memfile_slow_path_n;

static void note_slow(const char *path) {
    memfile_slow_opens++;
    unsigned i = memfile_slow_path_n;
    if (i < SLOW_PATHS) {
        strncpy(memfile_slow_paths[i], path ? path : "(null)", sizeof(memfile_slow_paths[0]) - 1);
        memfile_slow_path_n = i + 1;
    }
}

static inline MemFile *as_mem(void *f) {
    uintptr_t p = (uintptr_t)f;
    if (p < (uintptr_t)&slots[0] || p >= (uintptr_t)&slots[MEMFILE_SLOTS]) return NULL;
    return (MemFile *)f;
}

static MemFile *slot_alloc(void) {
    for (int i = 0; i < MEMFILE_SLOTS; i++)
        if (!slots[i].in_use && __sync_bool_compare_and_swap(&slots[i].in_use, 0, 1))
            return &slots[i];
    return NULL;
}

// ---------------------------------------------------------------- originals

static void *(*o_fopen)(const char *, const char *);
static int (*o_fclose)(void *);
static size_t (*o_fread)(void *, size_t, size_t, void *);
static int (*o_fseek)(void *, long, int);
static long (*o_ftell)(void *);
static void (*o_rewind)(void *);
static int (*o_fgetc)(void *);
static int (*o_getc)(void *);
static char *(*o_fgets)(char *, int, void *);
static int (*o_ungetc)(int, void *);
static size_t (*o_fwrite)(const void *, size_t, size_t, void *);
static int (*o_fputc)(int, void *);
static int (*o_putc)(int, void *);
static int (*o_fputs)(const char *, void *);
static int (*o_fflush)(void *);
static int (*o_setvbuf)(void *, char *, int, size_t);
static int (*o_vfprintf)(void *, const char *, va_list);
static int (*o_feof)(void *);
static int (*o_ferror)(void *);
static int (*o_rename)(const char *, const char *);
static int (*o_open)(const char *, int, int);
static int (*o_remove)(const char *);
static int (*o_unlink)(const char *);

// ---------------------------------------------------------------- SaveData miss cache

#define DIRCACHE_DIRS    16
#define DIRCACHE_NAMES   256

typedef struct {
    char path[256];
    int exists;            // directory exists
    int n;
    char *names[DIRCACHE_NAMES];
} DirListing;

static DirListing dircache[DIRCACHE_DIRS];
static int dircache_n = 0;
static SceKernelLwMutexWork dircache_lock;

static void dircache_clear(void) {
    sceKernelLockLwMutex(&dircache_lock, 1, NULL);
    for (int d = 0; d < dircache_n; d++)
        for (int i = 0; i < dircache[d].n; i++) free(dircache[d].names[i]);
    dircache_n = 0;
    sceKernelUnlockLwMutex(&dircache_lock, 1);
}

// 1 = file is known to be missing, 0 = it may exist (ask the filesystem)
static int savedata_known_missing(const char *path) {
    if (!strstr(path, "/SaveData/")) return 0;
    const char *slash = strrchr(path, '/');
    size_t dlen = slash - path;
    if (dlen == 0 || dlen >= sizeof(dircache[0].path)) return 0;
    const char *name = slash + 1;

    int missing = 0;
    sceKernelLockLwMutex(&dircache_lock, 1, NULL);
    DirListing *l = NULL;
    for (int d = 0; d < dircache_n; d++)
        if (strlen(dircache[d].path) == dlen && memcmp(dircache[d].path, path, dlen) == 0) { l = &dircache[d]; break; }

    if (!l && dircache_n < DIRCACHE_DIRS) {
        l = &dircache[dircache_n];
        memcpy(l->path, path, dlen);
        l->path[dlen] = 0;
        l->n = 0;
        l->exists = 0;
        SceUID dfd = sceIoDopen(l->path);
        if (dfd >= 0) {
            l->exists = 1;
            SceIoDirent ent;
            while (sceIoDread(dfd, &ent) > 0) {
                if (l->n == DIRCACHE_NAMES) { l->exists = -1; break; } // too big: don't trust it
                l->names[l->n++] = strdup(ent.d_name);
            }
            sceIoDclose(dfd);
        }
        dircache_n++;
    }

    if (l && l->exists == 0) {
        missing = 1;
    } else if (l && l->exists == 1) {
        missing = 1;
        for (int i = 0; i < l->n; i++)
            if (strcasecmp(l->names[i], name) == 0) { missing = 0; break; }
    }
    sceKernelUnlockLwMutex(&dircache_lock, 1);
    return missing;
}

// ---------------------------------------------------------------- asset pack
/*
 * The rest of the open cost is the memory card itself: 1-3.5 ms per open on
 * the exFAT directories with hundreds of files (screen-large/geometry has
 * 414), and a level load opens ~500 of them. heman.pak (made on the PC from
 * main.obb by tools/make_pak.py) holds every asset <= 256 KB of screen-large/,
 * sounds/ and the root: opened once, after that an asset "open" is a hash
 * lookup and its reads are preads into the pack. Without the file nothing
 * changes. Lookups are case-insensitive and ignore doubled slashes, like the
 * filesystem.
 */
typedef struct { uint32_t hash, off, size, name; } PakEnt;
static Handle pak_fd;
static PakEnt *pak_ents;
static char *pak_names;
static uint32_t pak_n, pak_mask;            // open-addressing table of pak_mask+1 slots
static int32_t *pak_table;
static volatile int pak_state;              // 0 = not tried, 1 = loading, 2 = ready, 3 = none
volatile unsigned memfile_pak_files, memfile_pak_opens;   // perf.c

static int h_open(const char *path, Handle *out);

// Last thing memfile did, for perf.c's stall report
char memfile_where[200];
#define WHERE(...) snprintf(memfile_where, sizeof(memfile_where), __VA_ARGS__)
// History of the last opens/closes, and stream calls since then
char memfile_ev[16][160];
volatile unsigned memfile_ev_n, memfile_calls;
#define EV(...) do { unsigned _i = __sync_fetch_and_add(&memfile_ev_n, 1) & 15; \
    snprintf(memfile_ev[_i], sizeof(memfile_ev[0]), __VA_ARGS__); } while (0)

// Normalizes the part of path after DATA_PATH into out; 0 if not under it
static int pak_key(const char *path, char *out, size_t cap) {
    size_t dl = sizeof(DATA_PATH) - 1;
    if (strncasecmp(path, DATA_PATH, dl) != 0) return 0;
    const char *p = path + dl;
    size_t n = 0;
    char prev = '/';
    for (; *p; p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/' && prev == '/') continue;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (n + 1 >= cap) return 0;
        out[n++] = prev = c;
    }
    out[n] = 0;
    return n > 0;
}

static uint32_t key_hash(const char *k) {
    uint32_t h = 2166136261u;
    for (; *k; k++) h = (h ^ (uint8_t)*k) * 16777619u;
    return h;
}

static int pak_load(void) {
    Handle fd;
    WHERE("pak: open");
    if (h_open(DATA_PATH "heman.pak", &fd) < 0) { EV("pak: not found"); return 0; }
    WHERE("pak: header");
    struct { char magic[4]; uint32_t version, count, names_size; } hdr;
    if (h_pread(fd, &hdr, sizeof(hdr), 0) != sizeof(hdr) || memcmp(hdr.magic, "HMPK", 4) != 0 ||
        hdr.version != 1 || hdr.count == 0 || hdr.count > 100000 || hdr.names_size > (16 << 20)) {
        sceClibPrintf("[memfile] heman.pak: bad header\n");
        h_close(fd);
        return 0;
    }
    uint32_t ents_size = hdr.count * sizeof(PakEnt);
    pak_ents = malloc(ents_size);
    pak_names = malloc(hdr.names_size);
    uint32_t slots = 1;
    while (slots < hdr.count * 2) slots <<= 1;
    pak_table = malloc(slots * sizeof(int32_t));
    if (!pak_ents || !pak_names || !pak_table ||
        h_pread(fd, pak_ents, ents_size, sizeof(hdr)) != (int)ents_size ||
        h_pread(fd, pak_names, hdr.names_size, sizeof(hdr) + ents_size) != (int)hdr.names_size ||
        pak_names[hdr.names_size - 1] != 0) {
        sceClibPrintf("[memfile] heman.pak: can't read the index\n");
        free(pak_ents); free(pak_names); free(pak_table);
        h_close(fd);
        return 0;
    }
    WHERE("pak: index read, %u files", hdr.count);
    int64_t pak_size = h_size(fd);
    memset(pak_table, 0xff, slots * sizeof(int32_t));
    pak_mask = slots - 1;
    for (uint32_t i = 0; i < hdr.count; i++) {
        PakEnt *e = &pak_ents[i];
        if (e->name >= hdr.names_size || (pak_size >= 0 && (int64_t)e->off + e->size > pak_size)) {
            sceClibPrintf("[memfile] heman.pak: bad entry %u\n", i);
            free(pak_ents); free(pak_names); free(pak_table);
            h_close(fd);
            return 0;
        }
        uint32_t j = e->hash & pak_mask;
        while (pak_table[j] >= 0) j = (j + 1) & pak_mask;
        pak_table[j] = (int32_t)i;
    }
    pak_fd = fd;
    pak_n = hdr.count;
    memfile_pak_files = pak_n;
    EV("pak: ready, %u files", pak_n);
    return 1;
}

static PakEnt *pak_find(const char *path) {
    if (pak_state != 2) {
        if (pak_state == 3) return NULL;
        if (__sync_bool_compare_and_swap(&pak_state, 0, 1)) {
            int ok = pak_load();
            __sync_synchronize();
            pak_state = ok ? 2 : 3;
        }
        while (pak_state == 1) sceKernelDelayThread(1000);
        if (pak_state != 2) return NULL;
    }
    char key[256];
    if (!pak_key(path, key, sizeof(key))) return NULL;
    uint32_t h = key_hash(key);
    for (uint32_t j = h & pak_mask; pak_table[j] >= 0; j = (j + 1) & pak_mask) {
        PakEnt *e = &pak_ents[pak_table[j]];
        if (e->hash == h && strcmp(pak_names + e->name, key) == 0) return e;
    }
    return NULL;
}

// A write outside SaveData would make the pack stale: stop using it
static void pak_disable(void) {
    if (pak_state == 2) {
        pak_state = 3;
        sceClibPrintf("[memfile] write under the data dir, heman.pak disabled\n");
    }
}

// ---------------------------------------------------------------- content cache
/*
 * Every fopen() costs 1-3 ms on the memory card even through FIOS, and the
 * game opens the same small files (UI layouts, sprite/anim tables, scripts)
 * again on every screen and level: one load was 580 opens = 830 ms. Keep
 * files that were read whole (<= WHOLE_MAX) in RAM, LRU, and serve repeat
 * opens without touching the disk. SaveData is never cached. Also remember
 * paths that don't exist (outside SaveData): each miss was a failed FIOS open
 * plus the original fopen's stat + open. Anything that writes outside
 * SaveData drops both caches.
 */
#define CACHE_ENTS    1536
#define CACHE_BUDGET  (12 * 1024 * 1024)
#define NEG_ENTS      512

typedef struct { uint32_t hash; char *path; uint8_t *data; int size; uint32_t stamp; } CacheEnt;
static CacheEnt cache[CACHE_ENTS];
static int cache_bytes;
static uint32_t cache_clock;
static struct { uint32_t hash; char *path; } neg[NEG_ENTS];
static int neg_next;
static SceKernelLwMutexWork cache_lock;
volatile unsigned memfile_cache_hits, memfile_cache_entries, memfile_neg_hits;  // perf.c

static uint32_t path_hash(const char *p) {
    uint32_t h = 2166136261u;
    for (; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static int is_savedata(const char *p) { return strstr(p, "/SaveData/") != NULL; }

static void cache_clear_all(void) {
    sceKernelLockLwMutex(&cache_lock, 1, NULL);
    for (int i = 0; i < CACHE_ENTS; i++) {
        free(cache[i].path); free(cache[i].data);
        cache[i].path = NULL; cache[i].data = NULL;
    }
    for (int i = 0; i < NEG_ENTS; i++) { free(neg[i].path); neg[i].path = NULL; }
    cache_bytes = 0;
    memfile_cache_entries = 0;
    sceKernelUnlockLwMutex(&cache_lock, 1);
}

// A write somewhere: SaveData only affects the SaveData listing cache
static void note_write(const char *path) {
    dircache_clear();
    if (!path || !is_savedata(path)) {
        cache_clear_all();
        // Only game data matters; our own logs etc. live outside these dirs
        char key[256];
        if (!path || (pak_key(path, key, sizeof(key)) && pak_state == 2 && pak_find(path)))
            pak_disable();
    }
}

// Returns a malloc'd copy of the cached file, or NULL
static uint8_t *cache_get(const char *path, int *size) {
    uint32_t h = path_hash(path);
    uint8_t *copy = NULL;
    sceKernelLockLwMutex(&cache_lock, 1, NULL);
    for (int i = 0; i < CACHE_ENTS; i++) {
        CacheEnt *e = &cache[i];
        if (e->path && e->hash == h && strcmp(e->path, path) == 0) {
            if ((copy = malloc(e->size ? e->size : 1))) {
                sceClibMemcpy(copy, e->data, e->size);
                *size = e->size;
                e->stamp = ++cache_clock;
                memfile_cache_hits++;
            }
            break;
        }
    }
    sceKernelUnlockLwMutex(&cache_lock, 1);
    return copy;
}

static void cache_put(const char *path, const uint8_t *data, int size) {
    if (!path[0] || is_savedata(path) || size > WHOLE_MAX) return;
    uint32_t h = path_hash(path);
    sceKernelLockLwMutex(&cache_lock, 1, NULL);
    int free_slot = -1;
    for (int i = 0; i < CACHE_ENTS; i++) {
        CacheEnt *e = &cache[i];
        if (!e->path) { if (free_slot < 0) free_slot = i; continue; }
        if (e->hash == h && strcmp(e->path, path) == 0) goto out;   // already there
    }
    // Evict least recently used entries until the file fits
    while (free_slot < 0 || cache_bytes + size > CACHE_BUDGET) {
        int lru = -1;
        for (int i = 0; i < CACHE_ENTS; i++)
            if (cache[i].path && (lru < 0 || (int32_t)(cache[i].stamp - cache[lru].stamp) < 0)) lru = i;
        if (lru < 0) goto out;
        cache_bytes -= cache[lru].size;
        free(cache[lru].path); free(cache[lru].data);
        cache[lru].path = NULL; cache[lru].data = NULL;
        memfile_cache_entries--;
        if (free_slot < 0) free_slot = lru;
    }
    {
        char *p = strdup(path);
        uint8_t *d = malloc(size ? size : 1);
        if (!p || !d) { free(p); free(d); goto out; }
        sceClibMemcpy(d, data, size);
        CacheEnt *e = &cache[free_slot];
        e->hash = h; e->path = p; e->data = d; e->size = size; e->stamp = ++cache_clock;
        cache_bytes += size;
        memfile_cache_entries++;
    }
out:
    sceKernelUnlockLwMutex(&cache_lock, 1);
}

static int neg_known(const char *path) {
    uint32_t h = path_hash(path);
    int hit = 0;
    sceKernelLockLwMutex(&cache_lock, 1, NULL);
    for (int i = 0; i < NEG_ENTS; i++)
        if (neg[i].path && neg[i].hash == h && strcmp(neg[i].path, path) == 0) { hit = 1; break; }
    sceKernelUnlockLwMutex(&cache_lock, 1);
    if (hit) memfile_neg_hits++;
    return hit;
}

static void neg_add(const char *path) {
    char *p = strdup(path);
    if (!p) return;
    sceKernelLockLwMutex(&cache_lock, 1, NULL);
    int i = neg_next++ % NEG_ENTS;
    free(neg[i].path);
    neg[i].hash = path_hash(path);
    neg[i].path = p;
    sceKernelUnlockLwMutex(&cache_lock, 1);
}

// ---------------------------------------------------------------- wrappers

static int is_read_only_mode(const char *mode) {
    return mode && mode[0] == 'r' && !strchr(mode, '+');
}

static void *w_fopen2(const char *path, const char *mode);

// "ux0:data/heman/SaveData//../sounds/x.wav" -> "ux0:data/heman/sounds/x.wav".
// The game looks up sounds through SaveData/.., which hid them from the pack
// and the caches and listed the data root as a SaveData directory.
static const char *canon_path(const char *path, char *out, size_t cap) {
    if (!strstr(path, "//") && !strstr(path, "/.")) return path;
    size_t len = strlen(path);
    if (len >= cap) return path;
    size_t n = 0;
    const char *p = path;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t cl = e ? (size_t)(e - p) : strlen(p);
        if (cl == 0 || (cl == 1 && p[0] == '.')) {
            // skip empty and "." components
        } else if (cl == 2 && p[0] == '.' && p[1] == '.') {
            // drop the previous component, never the device ("ux0:")
            size_t k = n;
            while (k > 0 && out[k - 1] != '/') k--;
            if (k == 0 || (n > 0 && out[n - 1] == ':')) return path;
            n = k > 0 ? k - 1 : 0;
        } else {
            if (n > 0) out[n++] = '/';
            memcpy(out + n, p, cl);
            n += cl;
        }
        if (!e) break;
        p = e + 1;
    }
    if (len > 0 && path[len - 1] == '/') out[n++] = '/';
    out[n] = 0;
    return out;
}

static void *w_fopen(const char *path, const char *mode) {
    if (!path || !mode) return o_fopen(path, mode);
    char canon[256];
    path = canon_path(path, canon, sizeof(canon));
    WHERE("fopen %.150s %s", path, mode);
    void *res = w_fopen2(path, mode);
    MemFile *rm = as_mem(res);
    EV("fopen %.110s %s -> %s%p sz=%lld", path, mode,
       !res ? "NULL " : !rm ? "stdio " : rm->pak ? "pak " : rm->nofd ? "cache " : "disk ",
       res, rm ? (long long)rm->size : -1LL);
    WHERE("fopen returned: %.150s", path);
    return res;
}

static void *w_fopen2(const char *path, const char *mode) {
    if (!is_read_only_mode(mode)) {
        note_slow(path);
        note_write(path);               // something may be created or replaced
        return o_fopen(path, mode);
    }
    WHERE("fopen: savedata check %.140s", path);
    if (savedata_known_missing(path)) {
        errno = ENOENT;
        return NULL;
    }
    WHERE("fopen: caches %.140s", path);
    int plain = strncmp(path, "ux0:", 4) == 0 && !is_savedata(path);
    if (plain && neg_known(path)) {
        errno = ENOENT;
        return NULL;
    }

    // Repeat open of a small file: straight from RAM
    int csize;
    uint8_t *cdata = plain ? cache_get(path, &csize) : NULL;
    if (cdata) {
        MemFile *m = slot_alloc();
        if (m) {
            memfile_fast_opens++;
            m->fd.fios = -1; m->fd.h = -1;
            m->nofd = 1;
            m->pak = 0;
            m->base = 0;
            m->whole = 1;
            m->pos = 0;
            m->size = csize;
            m->buf = cdata;
            m->buf_off = 0;
            m->buf_len = csize;
            m->eof = m->err = 0;
            m->path[0] = 0;
            return m;
        }
        free(cdata);
    }

    PakEnt *pe = plain ? pak_find(path) : NULL;
    if (pe) {
        MemFile *m = slot_alloc();
        if (m) {
            memfile_fast_opens++;
            memfile_pak_opens++;
            m->fd = pak_fd;
            m->nofd = 1;
            m->pak = 1;
            m->base = pe->off;
            m->whole = 0;
            m->pos = 0;
            m->size = pe->size;
            m->buf = NULL;
            m->buf_off = 0;
            m->buf_len = 0;
            m->eof = m->err = 0;
            if (strlen(path) < sizeof(m->path)) strcpy(m->path, path);
            else m->path[0] = 0;
            return m;
        }
    }

    Handle fd;
    WHERE("fopen: disk open %.140s", path);
    if (h_open(path, &fd) < 0) {
        WHERE("fopen: disk miss %.140s", path);
        if (plain) {
            // Only a directory still needs the original fopen (empty-stream
            // emulation); a plain miss is remembered.
            SceIoStat st;
            if (sceIoGetstat(path, &st) < 0) {
                neg_add(path);
                errno = ENOENT;
                return NULL;
            }
        }
        WHERE("fopen: original fopen %.140s", path);
        void *r = o_fopen(path, mode);  // directories, /proc/..., odd paths
        if (r) note_slow(path);
        return r;
    }
    MemFile *m = slot_alloc();
    if (!m) {
        h_close(fd);
        note_slow(path);
        return o_fopen(path, mode);
    }
    memfile_fast_opens++;
    m->whole = 0;
    m->nofd = 0;
    m->pak = 0;
    m->base = 0;
    m->fd = fd;
    m->pos = 0;
    m->size = -1;
    m->buf = NULL;
    m->buf_off = 0;
    m->buf_len = 0;
    m->eof = m->err = 0;
    if (plain && strlen(path) < sizeof(m->path)) strcpy(m->path, path);
    else m->path[0] = 0;
    return m;
}

static int w_fclose(void *f) {
    MemFile *m = as_mem(f);
    EV("fclose %p%s", f, m ? "" : " (stdio)");
    if (!m) return o_fclose(f);
    if (!m->nofd) h_close(m->fd);
    free(m->buf);
    m->buf = NULL;
    __sync_synchronize();
    m->in_use = 0;
    return 0;
}

static int64_t mem_size(MemFile *m) {
    if (m->size < 0) {
        int64_t size = h_size(m->fd);
        if (size >= 0) {
            m->size = size;
        } else {
            // No size: find the end by reading (rare, only for SEEK_END)
            static uint8_t scratch[BUF_SIZE];
            int64_t off = 0;
            int r;
            while ((r = m_pread(m, scratch, BUF_SIZE, off)) > 0) off += r;
            m->size = off;
        }
    }
    return m->size;
}

// Make the buffer hold the bytes at m->pos; returns available bytes there
static int mem_fill(MemFile *m) {
    if (m->buf && m->pos >= m->buf_off && m->pos < m->buf_off + m->buf_len)
        return (int)(m->buf_off + m->buf_len - m->pos);
    if (m->whole) return 0;                 // past the end of a fully read file
    if (!m->buf) {
        // First read of a small file: take all of it in one I/O
        int64_t size = mem_size(m);
        if (size >= 0 && size <= WHOLE_MAX && (m->buf = malloc(size ? (size_t)size : 1))) {
            int64_t got = 0;
            while (got < size) {
                int r = m_pread(m, m->buf + got, (SceSize)(size - got), got);
                if (r <= 0) break;
                got += r;
            }
            if (got == size) {
                cache_put(m->path, m->buf, (int)got);
                m->whole = 1;
                m->buf_off = 0;
                m->buf_len = (int)got;
                return m->pos < got ? (int)(got - m->pos) : 0;
            }
            // Short read: don't trust it, use the plain buffered path
            free(m->buf);
            m->buf = NULL;
        }
    }
    if (!m->buf && !(m->buf = malloc(BUF_SIZE))) { m->err = 1; return 0; }
    int r = m_pread(m, m->buf, BUF_SIZE, m->pos);
    if (r < 0) { m->err = 1; r = 0; }
    m->buf_off = m->pos;
    m->buf_len = r;
    if (r < BUF_SIZE && m->size < 0) m->size = m->pos + r;   // hit the end: now we know
    return r;
}

static size_t mem_read(MemFile *m, uint8_t *dst, size_t want) {
    size_t done = 0;
    while (done < want) {
        // Large remainder and nothing buffered at pos: read it directly
        int buffered = m->buf && m->pos >= m->buf_off && m->pos < m->buf_off + m->buf_len;
        if (!buffered && want - done >= BUF_SIZE) {
            int r = m_pread(m, dst + done, want - done, m->pos);
            if (r <= 0) { if (r < 0) m->err = 1; break; }
            done += r;
            m->pos += r;
            continue;   // a short read means EOF: the next pass reads 0 and stops
        }
        int avail = mem_fill(m);
        if (avail <= 0) break;
        size_t n = want - done < (size_t)avail ? want - done : (size_t)avail;
        const uint8_t *src = m->buf + (m->pos - m->buf_off);
        if (n == 1) dst[done] = *src;
        else sceClibMemcpy(dst + done, src, n);
        done += n;
        m->pos += n;
    }
    if (done < want) m->eof = 1;
    return done;
}

static size_t w_fread(void *buf, size_t sz, size_t n, void *f) {
    memfile_calls++;
    MemFile *m = as_mem(f);
    if (!m) { memfile_slow_freads++; return o_fread(buf, sz, n, f); }
    if (!sz || !n) return 0;
    size_t want = sz * n;
    // Fast path: tiny read fully inside the buffer
    if (m->buf && m->pos >= m->buf_off && m->pos + (int64_t)want <= m->buf_off + m->buf_len) {
        const uint8_t *src = m->buf + (m->pos - m->buf_off);
        if (want == 1) *(uint8_t *)buf = *src;
        else sceClibMemcpy(buf, src, want);
        m->pos += want;
        return n;
    }
    size_t got = mem_read(m, buf, want);
    return got / sz;
}

static int w_fseek(void *f, long off, int whence) {
    memfile_calls++;
    MemFile *m = as_mem(f);
    if (!m) return o_fseek(f, off, whence);
    int64_t base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = m->pos;
    else if (whence == SEEK_END) base = mem_size(m);
    else { errno = EINVAL; return -1; }
    if (base < 0 || base + off < 0) { errno = EINVAL; return -1; }
    m->pos = base + off;
    m->eof = 0;
    return 0;
}

static long w_ftell(void *f) {
    MemFile *m = as_mem(f);
    if (!m) return o_ftell(f);
    return (long)m->pos;
}

static void w_rewind(void *f) {
    MemFile *m = as_mem(f);
    if (!m) { o_rewind(f); return; }
    m->pos = 0;
    m->eof = m->err = 0;
}

static int mem_getc(MemFile *m) {
    if (!(m->buf && m->pos >= m->buf_off && m->pos < m->buf_off + m->buf_len) && mem_fill(m) <= 0) {
        m->eof = 1;
        return EOF;
    }
    return m->buf[m->pos++ - m->buf_off];
}

static int w_fgetc(void *f) {
    memfile_calls++;
    MemFile *m = as_mem(f);
    return m ? mem_getc(m) : o_fgetc(f);
}

static int w_getc(void *f) {
    memfile_calls++;
    MemFile *m = as_mem(f);
    return m ? mem_getc(m) : o_getc(f);
}

static char *w_fgets(char *s, int n, void *f) {
    memfile_calls++;
    MemFile *m = as_mem(f);
    if (!m) return o_fgets(s, n, f);
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = mem_getc(m);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = 0;
    return s;
}

static int w_ungetc(int c, void *f) {
    MemFile *m = as_mem(f);
    if (!m) return o_ungetc(c, f);
    if (c == EOF || m->pos == 0) return EOF;
    // Only possible over buffered data (the usual getc/ungetc pattern)
    if (!(m->buf && m->pos - 1 >= m->buf_off && m->pos - 1 < m->buf_off + m->buf_len)) return EOF;
    m->pos--;
    m->buf[m->pos - m->buf_off] = (uint8_t)c;
    m->eof = 0;
    return c & 0xFF;
}

static int w_feof(void *f) {
    memfile_calls++;
    MemFile *m = as_mem(f);
    return m ? m->eof : o_feof(f);
}

static int w_ferror(void *f) {
    MemFile *m = as_mem(f);
    return m ? m->err : o_ferror(f);
}

// Writes to a read-only stream fail, like they would on a real "r" FILE
static size_t w_fwrite(const void *b, size_t sz, size_t n, void *f) {
    MemFile *m = as_mem(f);
    if (!m) return o_fwrite(b, sz, n, f);
    m->err = 1;
    return 0;
}

static int w_fputc(int c, void *f) {
    MemFile *m = as_mem(f);
    if (!m) return o_fputc(c, f);
    m->err = 1;
    return EOF;
}

static int w_putc(int c, void *f) {
    MemFile *m = as_mem(f);
    if (!m) return o_putc(c, f);
    m->err = 1;
    return EOF;
}

static int w_fputs(const char *s, void *f) {
    MemFile *m = as_mem(f);
    if (!m) return o_fputs(s, f);
    m->err = 1;
    return EOF;
}

static int w_fflush(void *f) {
    MemFile *m = as_mem(f);
    return m ? 0 : o_fflush(f);
}

static int w_setvbuf(void *f, char *buf, int mode, size_t size) {
    MemFile *m = as_mem(f);
    return m ? 0 : o_setvbuf(f, buf, mode, size);
}

static int w_vfprintf(void *f, const char *fmt, va_list ap) {
    MemFile *m = as_mem(f);
    if (!m) return o_vfprintf(f, fmt, ap);
    m->err = 1;
    return -1;
}

static int w_fprintf(void *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = w_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

// Anything that can create, replace or delete a file drops the SaveData cache
static int w_rename(const char *a, const char *b) {
    note_write(a);
    note_write(b);
    return o_rename(a, b);
}
static int w_remove(const char *p) { note_write(p); return o_remove(p); }
static int w_unlink(const char *p) { note_write(p); return o_unlink(p); }
static int w_open(const char *p, int flags, int mode) {
    if (flags & 0x3) note_write(p);     // O_WRONLY / O_RDWR (same on bionic)
    return o_open(p, flags, mode);
}

// ---------------------------------------------------------------- install

#define HOOK(name) { #name, (uintptr_t *)&o_##name, (uintptr_t)&w_##name }

void memfile_install(so_default_dynlib *tbl, int n) {
    static const struct { const char *sym; uintptr_t *orig; uintptr_t wrap; } hooks[] = {
        HOOK(fopen), HOOK(fclose), HOOK(fread), HOOK(fseek), HOOK(ftell), HOOK(rewind),
        HOOK(fgetc), HOOK(getc), HOOK(fgets), HOOK(ungetc), HOOK(feof), HOOK(ferror),
        HOOK(fwrite), HOOK(fputc), HOOK(putc), HOOK(fputs), HOOK(fflush), HOOK(setvbuf),
        HOOK(vfprintf), HOOK(rename), HOOK(remove), HOOK(unlink), HOOK(open),
    };
    sceKernelCreateLwMutex(&dircache_lock, "memfile_dir", 0, 0, NULL);
    sceKernelCreateLwMutex(&cache_lock, "memfile_cache", 0, 0, NULL);

    for (unsigned h = 0; h < sizeof(hooks) / sizeof(hooks[0]); h++) {
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(tbl[i].symbol, hooks[h].sym) == 0) {
                if (!found) *hooks[h].orig = tbl[i].func;
                tbl[i].func = hooks[h].wrap;
                found = 1;
            }
        }
        if (!found) {
            // Without the original we can't fall back: leave stdio alone
            sceClibPrintf("[memfile] '%s' not in import table, memfile disabled\n", hooks[h].sym);
            for (unsigned k = 0; k < h; k++)
                for (int i = 0; i < n; i++)
                    if (tbl[i].func == hooks[k].wrap) tbl[i].func = *hooks[k].orig;
            return;
        }
    }
    // fprintf is variadic: route it through our vfprintf
    for (int i = 0; i < n; i++)
        if (strcmp(tbl[i].symbol, "fprintf") == 0) tbl[i].func = (uintptr_t)&w_fprintf;
}

// ---------------------------------------------------------------- helo_file readers
/*
 * The engine reads its binary assets through helo_file_read_u8/u16/u32/...
 * (helo_file_t: +0 FILE *, +16 eof flag), one fread() of 1-4 bytes per value:
 * a level load is ~540k calls, each through the import stub and our fread
 * wrapper. Replace them with versions that copy straight from the stream's
 * buffer. Same semantics as the originals, including when they set eof:
 * s32/f32/crc32 and s16 need the full value, the unsigned/s8 ones only
 * fail on 0 bytes. The value starts zeroed and keeps any partial bytes.
 */
extern so_module so_mod;

typedef struct { void *f; int pad[3]; uint8_t eof; } HeloFile;

static inline size_t hf_get(void *f, void *dst, size_t want) {
    MemFile *m = as_mem(f);
    if (!m) return o_fread(dst, 1, want, f);
    if (m->buf && m->pos >= m->buf_off && m->pos + (int64_t)want <= m->buf_off + m->buf_len) {
        const uint8_t *src = m->buf + (m->pos - m->buf_off);
        uint8_t *d = dst;
        for (size_t i = 0; i < want; i++) d[i] = src[i];
        m->pos += want;
        return want;
    }
    return mem_read(m, dst, want);
}

#define HF_READER(name, type, need)                                  \
    static type name(HeloFile *h) {                                   \
        type v = 0;                                                   \
        if (h->eof) return v;                                         \
        if (hf_get(h->f, &v, sizeof(type)) < (need)) h->eof = 1;      \
        return v;                                                     \
    }
HF_READER(hf_read_s32, int32_t, 4)
HF_READER(hf_read_f32, uint32_t, 4)   // float bits: the game is softfp, return in r0
HF_READER(hf_read_crc32, uint32_t, 4)
HF_READER(hf_read_s16, int16_t, 2)
HF_READER(hf_read_u32, uint32_t, 1)
HF_READER(hf_read_u16, uint16_t, 1)
HF_READER(hf_read_u8, uint8_t, 1)
HF_READER(hf_read_s8, uint8_t, 1)     // the original zero-extends too

static size_t hf_read(HeloFile *h, void *buf, unsigned n) {
    if (h->eof) return 0;
    size_t r = hf_get(h->f, buf, n);
    if (r == 0 && n != 0) h->eof = 1;
    return r;
}

static void hf_skip(HeloFile *h, unsigned n) {
    uint8_t tmp[256];
    while (n && !h->eof) {
        unsigned c = n < sizeof(tmp) ? n : sizeof(tmp);
        size_t r = hf_get(h->f, tmp, c);
        if (r < c) h->eof = 1;      // the original's per-byte u8 reads hit eof here
        n -= c;
    }
}

void memfile_patch_helo(void) {
    if (!o_fread) return;   // memfile not installed: leave the engine alone
    static const struct { const char *sym; void *fn; } hooks[] = {
        { "_Z18helo_file_read_s32P12_helo_file_t", (void *)hf_read_s32 },
        { "_Z18helo_file_read_f32P12_helo_file_t", (void *)hf_read_f32 },
        { "_Z10read_crc32P12_helo_file_t",         (void *)hf_read_crc32 },
        { "_Z18helo_file_read_s16P12_helo_file_t", (void *)hf_read_s16 },
        { "_Z18helo_file_read_u32P12_helo_file_t", (void *)hf_read_u32 },
        { "_Z18helo_file_read_u16P12_helo_file_t", (void *)hf_read_u16 },
        { "_Z17helo_file_read_u8P12_helo_file_t",  (void *)hf_read_u8 },
        { "_Z17helo_file_read_s8P12_helo_file_t",  (void *)hf_read_s8 },
        { "_Z14helo_file_readP12_helo_file_tPvj",  (void *)hf_read },
        { "_Z14helo_file_skipP12_helo_file_tj",    (void *)hf_skip },
    };
    uintptr_t addr[sizeof(hooks) / sizeof(hooks[0])];
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
        if (!(addr[i] = (uintptr_t)so_symbol(&so_mod, hooks[i].sym))) {
            sceClibPrintf("[memfile] %s not found, helo readers not patched\n", hooks[i].sym);
            return;
        }
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
        hook_addr(addr[i], (uintptr_t)hooks[i].fn);
}
