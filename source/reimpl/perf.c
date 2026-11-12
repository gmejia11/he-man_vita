/*
 * perf.c -- frame profiler (PERF_LOG builds).
 *
 * Wraps a few imports of the game (draw calls, texture/shader/buffer uploads,
 * file I/O, sleeps, eglSwapBuffers) by patching their entries in the
 * default_dynlib table before the .so is resolved, and writes a report to
 * ux0:data/heman/perf.txt:
 *
 *   - every ~5 s a summary: fps, avg/max frame time, how frames split between
 *     CPU work and time blocked inside eglSwapBuffers (GPU / vsync), and a
 *     histogram of frame times
 *   - one line per slow frame (> 20 ms) with what happened during it
 *
 * The file is written by a low priority thread so the logging itself doesn't
 * show up as a slow frame.
 */
#ifdef PERF_LOG
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define SLOW_FRAME_US    20000
#define SUMMARY_FRAMES   300
#define MAX_SLOW_LINES   40     // per summary window

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

// ---------------------------------------------------------------- counters

typedef struct {
    uint32_t draws, verts;
    uint32_t tex_up, tex_bytes; uint64_t tex_us;
    uint32_t shaders; uint64_t shader_us;
    uint32_t links; uint64_t link_us;
    uint32_t buf_up, buf_bytes;
    uint32_t fopens; uint64_t fopen_us;
    uint32_t freads, fread_bytes; uint64_t fread_us16;
    uint64_t sleep_us;
} FrameStats;

static FrameStats cur;                 // render thread only
static SceUID render_thread = -1;

static inline int on_render(void) {
    return render_thread < 0 || sceKernelGetThreadId() == render_thread;
}

// ---------------------------------------------------------------- report buffer

static char out_buf[2][16 * 1024];
static int out_len[2];
static int out_cur = 0;
static SceKernelLwMutexWork out_lock;
static volatile unsigned frames_done;   // swaps seen
static int64_t timer_cost16;             // cost of one now_us() pair, 1/16 us

static void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out_printf(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
    sceKernelLockLwMutex(&out_lock, 1, NULL);
    if (out_len[out_cur] + n <= (int)sizeof(out_buf[0])) {
        memcpy(out_buf[out_cur] + out_len[out_cur], line, n);
        out_len[out_cur] += n;
    }
    sceKernelUnlockLwMutex(&out_lock, 1);
}

static int perf_writer(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceUID fd = sceIoOpen(DATA_PATH "perf.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
    while (1) {
        sceKernelDelayThread(2 * 1000 * 1000);
        // No frame since the last pass: say what the file layer was doing
        static unsigned last_frames = ~0u, stall_s = 0;
        if (frames_done == last_frames) {
            extern char memfile_where[];
            extern char memfile_ev[16][160];
            extern volatile unsigned memfile_ev_n, memfile_calls;
            stall_s += 2;
            out_printf("   stall: no frame for %us, frames %u, stream calls %u, memfile: %s\n",
                       stall_s, frames_done, memfile_calls, memfile_where);
            if (stall_s == 4 || stall_s == 10) {
                unsigned n = memfile_ev_n;
                for (unsigned i = n > 16 ? n - 16 : 0; i < n; i++)
                    out_printf("     ev %u: %s\n", i, memfile_ev[i & 15]);
            }
            // Stuck for good: crash on purpose so the system writes a core
            // dump with every thread's stack (flush the log first)
            if (stall_s == 12) {
                out_printf("   stall: forcing a crash dump\n");
                sceKernelLockLwMutex(&out_lock, 1, NULL);
                int idx = out_cur, len = out_len[idx];
                out_len[idx] = 0;
                sceKernelUnlockLwMutex(&out_lock, 1);
                SceUID f2 = sceIoOpen(DATA_PATH "perf.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
                if (f2 >= 0) { sceIoWrite(f2, out_buf[idx], len); sceIoClose(f2); }
                *(volatile int *)0 = 0x57A11;
            }
        } else {
            stall_s = 0;
        }
        last_frames = frames_done;
        sceKernelLockLwMutex(&out_lock, 1, NULL);
        int idx = out_cur, len = out_len[idx];
        out_cur ^= 1;
        out_len[out_cur] = 0;
        sceKernelUnlockLwMutex(&out_lock, 1);
        if (!len) continue;
        fd = sceIoOpen(DATA_PATH "perf.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (fd >= 0) {
            sceIoWrite(fd, out_buf[idx], len);
            sceIoClose(fd);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- originals

static void (*o_glDrawArrays)(GLenum, GLint, GLsizei);
static void (*o_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*o_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*o_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*o_glCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static void (*o_glCompileShader)(GLuint);
static void (*o_glLinkProgram)(GLuint);
static void (*o_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*o_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
static void *(*o_fopen)(const char *, const char *);
static size_t (*o_fread)(void *, size_t, size_t, void *);
static int (*o_usleep)(useconds_t);
static int (*o_nanosleep)(const struct timespec *, struct timespec *);
static EGLBoolean (*o_eglSwapBuffers)(EGLDisplay, EGLSurface);

// ---------------------------------------------------------------- wrappers

static void w_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    cur.draws++; cur.verts += c;
    o_glDrawArrays(m, f, c);
}

static void w_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) {
    cur.draws++; cur.verts += c;
    o_glDrawElements(m, c, t, i);
}

static void w_glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d) {
    uint64_t t0 = now_us();
    o_glTexImage2D(t, l, ifmt, w, h, b, f, ty, d);
    cur.tex_us += now_us() - t0; cur.tex_up++; cur.tex_bytes += (uint32_t)(w * h * 4);
}

static void w_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *d) {
    uint64_t t0 = now_us();
    o_glTexSubImage2D(t, l, x, y, w, h, f, ty, d);
    cur.tex_us += now_us() - t0; cur.tex_up++; cur.tex_bytes += (uint32_t)(w * h * 4);
}

static void w_glCompressedTexImage2D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *d) {
    uint64_t t0 = now_us();
    o_glCompressedTexImage2D(t, l, ifmt, w, h, b, sz, d);
    cur.tex_us += now_us() - t0; cur.tex_up++; cur.tex_bytes += sz;
}

static void w_glCompileShader(GLuint s) {
    uint64_t t0 = now_us();
    o_glCompileShader(s);
    cur.shader_us += now_us() - t0; cur.shaders++;
}

static void w_glLinkProgram(GLuint p) {
    uint64_t t0 = now_us();
    o_glLinkProgram(p);
    cur.link_us += now_us() - t0; cur.links++;
}

static void w_glBufferData(GLenum t, GLsizeiptr sz, const void *d, GLenum u) {
    cur.buf_up++; cur.buf_bytes += sz;
    o_glBufferData(t, sz, d, u);
}

static void w_glBufferSubData(GLenum t, GLintptr off, GLsizeiptr sz, const void *d) {
    cur.buf_up++; cur.buf_bytes += sz;
    o_glBufferSubData(t, off, sz, d);
}

static void *w_fopen(const char *p, const char *m) {
    if (!on_render()) return o_fopen(p, m);
    uint64_t t0 = now_us();
    void *r = o_fopen(p, m);
    cur.fopen_us += now_us() - t0; cur.fopens++;
    return r;
}

static size_t w_fread(void *b, size_t s, size_t n, void *f) {
    if (!on_render()) return o_fread(b, s, n, f);
    // Timing every call (two clock reads) cost more than the fread itself
    // (the game reads byte by byte): time 1 call in 32 and scale, except
    // big reads, which are always timed.
    size_t r;
    if ((cur.freads++ & 31) == 0 || s * n >= 4096) {
        uint64_t t0 = now_us();
        r = o_fread(b, s, n, f);
        // Fixed-point (1/16 us): subtract the clock's own cost from samples
        int64_t dt16 = (int64_t)(now_us() - t0) * 16 - timer_cost16;
        if (dt16 < 0) dt16 = 0;
        cur.fread_us16 += s * n >= 4096 ? dt16 : dt16 * 32;
    } else {
        r = o_fread(b, s, n, f);
    }
    cur.fread_bytes += r * s;
    return r;
}

static int w_usleep(useconds_t us) {
    if (!on_render()) return o_usleep(us);
    uint64_t t0 = now_us();
    int r = o_usleep(us);
    cur.sleep_us += now_us() - t0;
    return r;
}

static int w_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!on_render()) return o_nanosleep(req, rem);
    uint64_t t0 = now_us();
    int r = o_nanosleep(req, rem);
    cur.sleep_us += now_us() - t0;
    return r;
}

// ---------------------------------------------------------------- per frame

static uint64_t last_swap_end = 0;
static unsigned frame_no = 0;

// summary window
static unsigned win_frames, win_slow_lines;
static uint64_t win_start, win_frame_us, win_cpu_us, win_swap_us, win_max_us;
static unsigned hist[6]; // <17, 17-20, 20-25, 25-34, 34-50, >=50 ms

static void end_frame(uint64_t swap_start, uint64_t swap_end) {
    if (!last_swap_end) {
        last_swap_end = swap_end;
        win_start = swap_end;
        memset(&cur, 0, sizeof(cur));
        return;
    }
    frame_no++;
    frames_done = frame_no;
    uint64_t frame = swap_end - last_swap_end;
    uint64_t swap = swap_end - swap_start;
    uint64_t cpu = swap_start - last_swap_end;   // includes the game's own sleeps
    last_swap_end = swap_end;

    win_frames++;
    win_frame_us += frame; win_cpu_us += cpu; win_swap_us += swap;
    if (frame > win_max_us) win_max_us = frame;
    int b = frame < 17000 ? 0 : frame < 20000 ? 1 : frame < 25000 ? 2 :
            frame < 34000 ? 3 : frame < 50000 ? 4 : 5;
    hist[b]++;

    if (frame > SLOW_FRAME_US && win_slow_lines < MAX_SLOW_LINES) {
        win_slow_lines++;
        out_printf("SLOW #%u %.1fms cpu=%.1f swap=%.1f sleep=%.1f | draws=%u verts=%u"
                   " tex=%u(%uKB,%.1fms) shd=%u(%.1fms) link=%u(%.1fms) buf=%u(%uKB)"
                   " fopen=%u(%.1fms) fread=%u(%uKB,%.1fms)\n",
                   frame_no, frame / 1000.0, cpu / 1000.0, swap / 1000.0, cur.sleep_us / 1000.0,
                   cur.draws, cur.verts,
                   cur.tex_up, cur.tex_bytes / 1024, cur.tex_us / 1000.0,
                   cur.shaders, cur.shader_us / 1000.0, cur.links, cur.link_us / 1000.0,
                   cur.buf_up, cur.buf_bytes / 1024,
                   cur.fopens, cur.fopen_us / 1000.0,
                   cur.freads, cur.fread_bytes / 1024, cur.fread_us16 / 16000.0);
    }

    if (win_frames >= SUMMARY_FRAMES) {
        extern volatile unsigned memfile_fast_opens, memfile_slow_opens, memfile_slow_freads;
        extern char memfile_slow_paths[][96];
        extern volatile unsigned memfile_slow_path_n;
        static unsigned paths_printed = 0;
        extern volatile int memfile_use_fios;
        extern volatile unsigned memfile_cache_hits, memfile_cache_entries, memfile_neg_hits;
        extern volatile unsigned memfile_pak_files, memfile_pak_opens;
        out_printf("   files: fast opens %u (%s), pak opens %u (pak %u files), cache hits %u (%u files),"
                   " known-missing %u, stdio opens %u, stdio freads %u\n",
                   memfile_fast_opens, memfile_use_fios ? "fios" : "sceIo",
                   memfile_pak_opens, memfile_pak_files,
                   memfile_cache_hits, memfile_cache_entries, memfile_neg_hits,
                   memfile_slow_opens, memfile_slow_freads);
        while (paths_printed < memfile_slow_path_n)
            out_printf("   stdio open: %s\n", memfile_slow_paths[paths_printed++]);
        double secs = (swap_end - win_start) / 1e6;
        out_printf("== frames %u-%u: %.1f fps | frame avg %.1f max %.1f ms | cpu avg %.1f | swap avg %.1f"
                   " | <17:%u 17-20:%u 20-25:%u 25-34:%u 34-50:%u >50:%u | draws(last) %u\n",
                   frame_no - win_frames + 1, frame_no, win_frames / secs,
                   win_frame_us / 1000.0 / win_frames, win_max_us / 1000.0,
                   win_cpu_us / 1000.0 / win_frames, win_swap_us / 1000.0 / win_frames,
                   hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], cur.draws);
        win_frames = win_slow_lines = 0;
        win_frame_us = win_cpu_us = win_swap_us = win_max_us = 0;
        memset(hist, 0, sizeof(hist));
        win_start = swap_end;
    }
    memset(&cur, 0, sizeof(cur));
}

static EGLBoolean w_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (render_thread < 0) render_thread = sceKernelGetThreadId();
    uint64_t t0 = now_us();
    EGLBoolean r = o_eglSwapBuffers(d, s);
    end_frame(t0, now_us());
    return r;
}

// ---------------------------------------------------------------- install

#define HOOK(name) { #name, (uintptr_t *)&o_##name, (uintptr_t)&w_##name }

void perf_install(so_default_dynlib *tbl, int n) {
    static const struct { const char *sym; uintptr_t *orig; uintptr_t wrap; } hooks[] = {
        HOOK(glDrawArrays), HOOK(glDrawElements),
        HOOK(glTexImage2D), HOOK(glTexSubImage2D), HOOK(glCompressedTexImage2D),
        HOOK(glCompileShader), HOOK(glLinkProgram),
        HOOK(glBufferData), HOOK(glBufferSubData),
        HOOK(fopen), HOOK(fread), HOOK(usleep), HOOK(nanosleep),
        HOOK(eglSwapBuffers),
    };
    sceKernelCreateLwMutex(&out_lock, "perf_out", 0, 0, NULL);
    int hooked = 0;
    for (unsigned h = 0; h < sizeof(hooks) / sizeof(hooks[0]); h++) {
        for (int i = 0; i < n; i++) {
            if (strcmp(tbl[i].symbol, hooks[h].sym) == 0) {
                *hooks[h].orig = tbl[i].func;
                tbl[i].func = hooks[h].wrap;
                hooked++;
                break;
            }
        }
    }
    out_printf("He-Man perf log (" __DATE__ " " __TIME__ "), %d imports wrapped\n"
               "SLOW lines: cpu = time from last swap to this swap call (game logic + GL"
               " submission + sleeps), swap = time blocked in eglSwapBuffers (GPU/vsync)\n",
               hooked);
    {
        uint64_t t0 = now_us(), sum = 0;
        for (int i = 0; i < 4096; i++) { uint64_t a = now_us(); sum += now_us() - a; }
        (void)t0;
        timer_cost16 = (int64_t)(sum * 16 / 4096);
    }
        SceUID t = sceKernelCreateThread("perf_writer", perf_writer, 0xA0, 16 * 1024, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
#endif
