/*
 * Heap corruption hunt (HEAP_WATCH builds).
 *
 * Something overwrites the heap area where vitaGL keeps its early objects
 * (around 0x82509150-0x82509950) about 12 s in, crashing SceGxm. Before vitaGL
 * initializes, grab a big "quarantine" block so vitaGL's objects land elsewhere.
 * Its inner pages are ours alone, so they can be made read-only (a stray write
 * then faults and leaves a crash dump naming the writer) and are filled with a
 * pattern that is checked every frame.
 */
#ifdef HEAP_WATCH
#include <kubridge.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <malloc.h>

#define QUARANTINE_SIZE (128 * 1024)
#define PATTERN 0xA5

static uint8_t *quarantine;
static uint8_t *probe_blk;
static uint32_t quarantine_hdr[2];
// Snapshot of the heap between the end of FIOS' guard and the probe
static uint8_t *pre_snap;
static uint8_t *pre_from, *pre_to;
static uintptr_t prot_start, prot_end;
static int reported;

static void hw_log(const char *fmt, ...) {
    static int truncated = 0;
    if (!truncated) {
        truncated = 1;
        SceUID t = sceIoOpen(DATA_PATH "heapwatch.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (t >= 0) sceIoClose(t);
    }
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
    sceClibPrintf("%s", line);
    SceUID fd = sceIoOpen(DATA_PATH "heapwatch.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}

void heapwatch_begin(void) {

    // Small probe first: shows where fresh heap allocations land right now.
    // Kept allocated and filled with a pattern so an overflow coming from the
    // block before the quarantine shows up there too.
    void *probe = malloc(16);
    probe_blk = probe;
    if (probe_blk) memset(probe_blk, 0xC3, 16);
    quarantine = malloc(QUARANTINE_SIZE);
    if (!quarantine) { hw_log("quarantine malloc failed\n"); return; }
    memset(quarantine, PATTERN, QUARANTINE_SIZE);

    prot_start = ((uintptr_t)quarantine + 0xFFF) & ~0xFFFu;
    prot_end = ((uintptr_t)quarantine + QUARANTINE_SIZE) & ~0xFFFu;
    int r = kuKernelMemProtect((void *)prot_start, prot_end - prot_start, KU_KERNEL_PROT_READ);
    memcpy(quarantine_hdr, quarantine - 8, 8);
    // Remember what the heap between FIOS' guard and the probe looks like now
    extern char *fios_ramcache_buffer;
    extern unsigned fios_ramcache_size, fios_ramcache_guard_size;
    if (fios_ramcache_buffer && probe) {
        pre_from = (uint8_t *)fios_ramcache_buffer + fios_ramcache_size + fios_ramcache_guard_size;
        pre_to = (uint8_t *)probe;
        if (pre_to > pre_from && pre_to - pre_from < 256 * 1024) {
            pre_snap = malloc(pre_to - pre_from);
            if (pre_snap) memcpy(pre_snap, pre_from, pre_to - pre_from);
        }
    }
    hw_log("build %s %s\n", __DATE__, __TIME__);
    hw_log("probe=%p quarantine=%p..%p protected=0x%08X..0x%08X r=0x%08X hdr=%08X %08X\n",
           probe, quarantine, quarantine + QUARANTINE_SIZE, (unsigned)prot_start, (unsigned)prot_end, r,
           (unsigned)quarantine_hdr[0], (unsigned)quarantine_hdr[1]);
}

extern char *fios_ramcache_buffer;
extern unsigned fios_ramcache_size, fios_ramcache_guard_size;
static int fios_reported;


void heapwatch_after_gl_init(void) {
    extern void *clear_vertex_program_patched;
    hw_log("after vitaGL init: clear_vertex_program_patched=%p\n", clear_vertex_program_patched);
    hw_log("FIOS ram cache: %p..%p (+ guard to %p)\n", fios_ramcache_buffer,
           fios_ramcache_buffer + fios_ramcache_size,
           fios_ramcache_buffer + fios_ramcache_size + fios_ramcache_guard_size);
    {
        extern JavaVM jvm; extern JNIEnv jni;
        hw_log("known: jvm table=%p jni table=%p\n", (void *)jvm, (void *)jni);
    }
}

static void check_fios_guard(unsigned frame) {
    if (!fios_ramcache_buffer || fios_reported) return;
    const uint8_t *g = (const uint8_t *)fios_ramcache_buffer + fios_ramcache_size;
    size_t last = 0, first = (size_t)-1;
    for (size_t i = 0; i < fios_ramcache_guard_size; i++) {
        if (g[i] != 0x5A) { if (first == (size_t)-1) first = i; last = i; }
    }
    if (first != (size_t)-1) {
        fios_reported = 1;
        hw_log("frame %u: FIOS ram cache overran its buffer: guard bytes 0x%X..0x%X modified\n",
               frame, (unsigned)first, (unsigned)last);
        const uint8_t *q = g + (first & ~15u);
        hw_log("  %p: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", q,
               q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
    }
}

// Allocation logging for the game's heap calls: report every block handed out
// inside the corrupted window, with its size and the game's calling address.
extern so_module so_mod;
static int alloc_logged;
static int early_logged;
static void note_alloc(const char *fn, void *p, size_t size, void *ret) {
    if (!p) return;
    uint8_t *b = (uint8_t *)p;
    // Before heapwatch_begin (static constructors, loader init): log every
    // allocation >= 256 bytes so the buffer that later overflows can be named.
    if (!pre_from) {
        if (size < 256 || early_logged >= 200) return;
        early_logged++;
        uintptr_t r = (uintptr_t)ret;
        if (r >= so_mod.text_base && r < so_mod.text_base + so_mod.text_size)
            hw_log("  early %s(%u) = %p..%p  from so+0x%X\n", fn, (unsigned)size, p, b + size, (unsigned)(r - so_mod.text_base));
        else
            hw_log("  early %s(%u) = %p..%p  from 0x%08X\n", fn, (unsigned)size, p, b + size, (unsigned)r);
        return;
    }
    if (alloc_logged >= 120) return;
    if (b + size < pre_from || b >= (uint8_t *)quarantine + 0x800) return;
    alloc_logged++;
    uintptr_t r = (uintptr_t)ret;
    if (r >= so_mod.text_base && r < so_mod.text_base + so_mod.text_size)
        hw_log("  alloc %s(%u) = %p..%p  from so+0x%X\n", fn, (unsigned)size, p, b + size, (unsigned)(r - so_mod.text_base));
    else
        hw_log("  alloc %s(%u) = %p..%p  from 0x%08X\n", fn, (unsigned)size, p, b + size, (unsigned)r);
}
// Log the game return addresses found on the stack (call chain of an allocation)
static void __attribute__((noinline)) log_stack_chain(size_t size) {
    uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
    char line[256];
    int n = snprintf(line, sizeof(line), "    chain for %u-byte alloc:", (unsigned)size);
    int found = 0;
    for (int i = 0; i < 256 && found < 12 && n < (int)sizeof(line) - 16; i++) {
        uint32_t v = sp[i];
        // Thumb return addresses into the game's .text
        if ((v & 1) && v >= so_mod.text_base && v < so_mod.text_base + so_mod.text_size) {
            n += snprintf(line + n, sizeof(line) - n, " so+0x%X", (unsigned)(v - 1 - so_mod.text_base));
            found++;
        }
    }
    hw_log("%s\n", line);
}

void *hw_malloc(size_t size) {
    void *p = malloc(size);
    note_alloc("malloc", p, size, __builtin_return_address(0));
    if (size == 8208 || size == 32784)
        log_stack_chain(size);
    return p;
}
void *hw_calloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    note_alloc("calloc", p, n * size, __builtin_return_address(0));
    return p;
}
void *hw_realloc(void *old, size_t size) {
    void *p = realloc(old, size);
    note_alloc("realloc", p, size, __builtin_return_address(0));
    return p;
}
void *hw_memalign(size_t align, size_t size) {
    void *p = memalign(align, size);
    note_alloc("memalign", p, size, __builtin_return_address(0));
    return p;
}
void *hw_new(size_t size) {
    void *p = malloc(size ? size : 1);
    note_alloc("new", p, size, __builtin_return_address(0));
    return p;
}

// Called once per frame (from eglSwapBuffers)
void heapwatch_check(unsigned frame) {
    check_fios_guard(frame);
    if (!quarantine || reported) return;
    size_t first = (size_t)-1, last = 0, count = 0;
    for (size_t i = 0; i < QUARANTINE_SIZE; i++) {
        if (quarantine[i] != PATTERN) {
            if (first == (size_t)-1) first = i;
            last = i;
            count++;
        }
    }
    if (first == (size_t)-1) return;
    reported = 1;
    uint32_t hdr[2];
    memcpy(hdr, quarantine - 8, 8);
    int probe_changed = 0;
    for (int i = 0; probe_blk && i < 16; i++)
        if (probe_blk[i] != 0xC3) probe_changed = 1;
    // Walk back from the probe through the snapshot: where does the zeroed run start?
    if (pre_snap) {
        size_t n = pre_to - pre_from;
        size_t i = n;
        while (i > 0 && pre_from[i - 1] == 0) i--;
        hw_log("  zero run before probe starts at %p (%u bytes before probe); snapshot there was:\n",
               pre_from + i, (unsigned)(n - i));
        // First byte that differs from the snapshot = where the overflowing write started
        size_t d = 0;
        while (d < n && pre_from[d] == pre_snap[d]) d++;
        hw_log("  first byte changed since startup: %p (snapshot covers %p..%p)\n", pre_from + d, pre_from, pre_to);
        if (d < n) {
            size_t r0 = d >= 32 ? (d - 32) & ~15u : 0;
            for (int k = 0; k < 4 && r0 < n; k++, r0 += 16) {
                const uint8_t *now = pre_from + r0, *was = pre_snap + r0;
                hw_log("  %p was %02X%02X%02X%02X %02X%02X%02X%02X now %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", now,
                       was[0], was[1], was[2], was[3], was[4], was[5], was[6], was[7],
                       now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11], now[12], now[13], now[14], now[15]);
            }
        }
        size_t row = i >= 32 ? (i - 32) & ~15u : 0;
        for (int k = 0; k < 6 && row < n; k++, row += 16) {
            const uint8_t *now = pre_from + row, *was = pre_snap + row;
            hw_log("  %p was %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X now %02X%02X%02X%02X %02X%02X%02X%02X\n", now,
                   was[0], was[1], was[2], was[3], was[4], was[5], was[6], was[7], was[8], was[9], was[10], was[11], was[12], was[13], was[14], was[15],
                   now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7]);
        }
    }
    hw_log("frame %u: quarantine modified: offsets 0x%X..0x%X (%u bytes changed)\n",
           frame, (unsigned)first, (unsigned)last, (unsigned)count);
    hw_log("  malloc header before: %08X %08X now: %08X %08X | probe(16B before) changed=%d\n",
           (unsigned)quarantine_hdr[0], (unsigned)quarantine_hdr[1], (unsigned)hdr[0], (unsigned)hdr[1], probe_changed);
    if (probe_blk) {
        const uint8_t *q = probe_blk;
        hw_log("  probe %p: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", q,
               q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
    }
    size_t dump_from[2] = { first & ~15u, last & ~15u };
    for (int k = 0; k < 2; k++) {
        if (k == 1 && dump_from[1] <= dump_from[0] + 48) break;
        for (size_t row = dump_from[k]; row < dump_from[k] + 64 && row < QUARANTINE_SIZE; row += 16) {
            const uint8_t *q = quarantine + row;
            hw_log("  +0x%05X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", (unsigned)row,
                   q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
        }
    }
}
#endif

#ifdef HEAP_WATCH
/*
 * Whole-program allocator tracing (-Wl,--wrap=malloc,free,calloc,realloc,memalign):
 * catches allocations made by loader-side libraries (OpenSLES, FalsoJNI, vitaGL)
 * that don't go through the game's imports. Only blocks touching the window of
 * the overflowing 8208-byte WStringBuffer are reported.
 */
void *__real_malloc(size_t);
void __real_free(void *);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t);

#define WATCH_LO ((uintptr_t)0x82545d00)
#define WATCH_HI ((uintptr_t)0x82549800)
static volatile int in_wrap;
static int wrap_logged;

static void wrap_note(const char *fn, void *p, size_t size, void *ret) {
    uintptr_t b = (uintptr_t)p;
    if (!p || in_wrap || wrap_logged >= 150) return;
    if (b + size < WATCH_LO || b >= WATCH_HI) return;
    in_wrap = 1;
    wrap_logged++;
    uintptr_t r = (uintptr_t)ret;
    if (r >= so_mod.text_base && r < so_mod.text_base + so_mod.text_size)
        hw_log("  [all] %s(%u) = %p..%p from so+0x%X\n", fn, (unsigned)size, p, (void *)(b + size), (unsigned)(r - so_mod.text_base));
    else
        hw_log("  [all] %s(%u) = %p..%p from eboot 0x%08X\n", fn, (unsigned)size, p, (void *)(b + size), (unsigned)r);
    in_wrap = 0;
}

void *__wrap_malloc(size_t size) { void *p = __real_malloc(size); wrap_note("malloc", p, size, __builtin_return_address(0)); return p; }
void *__wrap_calloc(size_t n, size_t s) { void *p = __real_calloc(n, s); wrap_note("calloc", p, n * s, __builtin_return_address(0)); return p; }
void *__wrap_realloc(void *o, size_t s) { void *p = __real_realloc(o, s); wrap_note("realloc", p, s, __builtin_return_address(0)); return p; }
void *__wrap_memalign(size_t a, size_t s) { void *p = __real_memalign(a, s); wrap_note("memalign", p, s, __builtin_return_address(0)); return p; }
void __wrap_free(void *p) {
    uintptr_t b = (uintptr_t)p;
    if (p && !in_wrap && wrap_logged < 150 && b >= WATCH_LO && b < WATCH_HI) {
        in_wrap = 1;
        wrap_logged++;
        uintptr_t r = (uintptr_t)__builtin_return_address(0);
        if (r >= so_mod.text_base && r < so_mod.text_base + so_mod.text_size)
            hw_log("  [all] free(%p) from so+0x%X\n", p, (unsigned)(r - so_mod.text_base));
        else
            hw_log("  [all] free(%p) from eboot 0x%08X\n", p, (unsigned)r);
        in_wrap = 0;
    }
    __real_free(p);
}
#endif
