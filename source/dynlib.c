/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dynlib.c
 * @brief Resolving dynamic imports of the .so.
 */

#include <psp2/kernel/clib.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <locale.h>
#include <poll.h>

#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <so_util/so_util.h>
#include <utime.h>

#include "utils/glutil.h"
#include "utils/utils.h"
#include "utils/logger.h"

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "reimpl/errno.h"
#include "reimpl/io.h"
#include "reimpl/log.h"
#include "reimpl/mem.h"
#include "reimpl/pthr.h"
#include "reimpl/sys.h"
#include "reimpl/egl.h"
#include "reimpl/time64.h"
#include "reimpl/asset_manager.h"

#ifdef NDK_PORT
#include <falso_ndk/FalsoNDK.h>
#include <psp2/kernel/processmgr.h>
#endif

const unsigned int __page_size = PAGE_SIZE;

extern void * _ZNSt9exceptionD2Ev;
extern void * _ZSt17__throw_bad_allocv;
extern void * _ZSt9terminatev;
extern void * _ZdaPv;
extern void * _ZdlPv;
extern void * _Znaj;
extern void * __cxa_allocate_exception;
extern void * __cxa_begin_catch;
extern void * __cxa_end_catch;
extern void * __cxa_free_exception;
extern void * __cxa_rethrow;
extern void * __cxa_throw;
extern void * __gxx_personality_v0;
extern void *_ZNSt8bad_castD1Ev;
extern void *_ZTISt8bad_cast;
extern void *_ZTISt9exception;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
extern void *_Znwj;
extern void *__aeabi_atexit;
extern void *__aeabi_d2lz;
extern void *__aeabi_d2ulz;
extern void *__aeabi_dadd;
extern void *__aeabi_dcmpgt;
extern void *__aeabi_dcmplt;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_f2lz;
extern void *__aeabi_f2ulz;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ldivmod;
extern void *__aeabi_memclr;
extern void *__aeabi_memcpy;
extern void *__aeabi_memmove;
extern void *__aeabi_memset4;
extern void *__aeabi_memset8;
extern void *__aeabi_memset;
extern void *__aeabi_ui2d;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;
extern void *__aeabi_unwind_cpp_pr0;
extern void *__aeabi_unwind_cpp_pr1;
extern void *__cxa_atexit;
extern void *__cxa_call_unexpected;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__gnu_ldivmod_helper;
extern void *__gnu_unwind_frame;
extern void *__srget;
extern void *__stack_chk_guard;
extern void *__swbuf;

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static FILE __sF_fake[3];

void *dlsym_soloader(void * handle, const char * symbol);

// Bionic float classification helpers (not exported by newlib under these names)
static int __isfinite_soloader(double x) { return isfinite(x); }
static int __isfinitef_soloader(float x) { return isfinite(x); }
static int __isinf_soloader(double x) { return isinf(x); }
static int __isinff_soloader(float x) { return isinf(x); }
static int __isnan_soloader(double x) { return isnan(x); }
static int __isnanf_soloader(float x) { return isnan(x); }

// GLES1 features vitaGL doesn't implement. The game checks glGetError() after
// texture setup and throws the texture away on any error, so these must not
// raise GL_INVALID_ENUM.
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif
static int gles1_unsupported_cap(GLenum cap) {
	switch (cap) {
	case 0x0B10: // GL_POINT_SMOOTH
	case 0x0B20: // GL_LINE_SMOOTH
	case 0x0BD0: // GL_DITHER
	case 0x0BA1: // GL_NORMALIZE
	case 0x803A: // GL_RESCALE_NORMAL
	case 0x809D: // GL_MULTISAMPLE
		return 1;
	}
	return 0;
}
// Vita3K workaround: a color-only glClear after the splash makes Vita3K's
// sceGxmReserve*DefaultUniformBuffer return NULL and the emulator crashes at
// the next swap. The game redraws the whole screen every frame, so skipping
// color-only clears is invisible. Real hardware: build with -DVITA3K_WORKAROUNDS=OFF.
static void glClear_shim(GLbitfield mask) {
#ifdef VITA3K_NO_COLOR_CLEAR
	if (mask == GL_COLOR_BUFFER_BIT)
		return;
#endif
	glClear(mask);
}
static void glEnable_shim(GLenum cap) { if (!gles1_unsupported_cap(cap)) glEnable(cap); }
static void glDisable_shim(GLenum cap) { if (!gles1_unsupported_cap(cap)) glDisable(cap); }
static void glHint_shim(GLenum target, GLenum mode) {
	if (target >= 0x0C50 && target <= 0x0C54) return; // GLES1 *_SMOOTH_HINT / PERSPECTIVE / FOG
	if (target == 0x8192) return; // GL_GENERATE_MIPMAP_HINT
	glHint(target, mode);
}
// GLES1 GL_GENERATE_MIPMAP: vitaGL rejects it, but the game still selects
// mipmapped min filters (e.g. GL_LINEAR_MIPMAP_LINEAR) on those textures, and
// sampling missing mip levels crashes Vita3K. Remember the flag per texture
// and build the mip chain with glGenerateMipmap after each upload.
#define AUTOMIP_MAX 8192
static uint8_t automip[AUTOMIP_MAX];
static GLuint bound_tex2d[8];
static int active_unit = 0;

static void set_automip(GLenum target, int on) {
	GLuint t = bound_tex2d[active_unit];
	if (target == GL_TEXTURE_2D && t < AUTOMIP_MAX)
		automip[t] = on ? 1 : 0;
}
static void glBindTexture_shim(GLenum target, GLuint tex) {
	if (target == GL_TEXTURE_2D)
		bound_tex2d[active_unit] = tex;
	glBindTexture(target, tex);
}
static void glActiveTexture_shim(GLenum unit) {
	int u = (int)unit - GL_TEXTURE0;
	if (u >= 0 && u < 8)
		active_unit = u;
	glActiveTexture(unit);
}
static void glGenTextures_shim(GLsizei n, GLuint *tex) {
	glGenTextures(n, tex);
	for (GLsizei i = 0; i < n; i++)
		if (tex[i] < AUTOMIP_MAX)
			automip[tex[i]] = 0;
}
static void glTexImage2D_shim(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                              GLint border, GLenum fmt, GLenum type, const void *data) {
	glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
	GLuint t = bound_tex2d[active_unit];
	if (target == GL_TEXTURE_2D && level == 0 && t < AUTOMIP_MAX && automip[t])
		glGenerateMipmap(GL_TEXTURE_2D);
}
static void glTexSubImage2D_shim(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                 GLenum fmt, GLenum type, const void *data) {
	glTexSubImage2D(target, level, x, y, w, h, fmt, type, data);
	GLuint t = bound_tex2d[active_unit];
	if (target == GL_TEXTURE_2D && level == 0 && t < AUTOMIP_MAX && automip[t])
		glGenerateMipmap(GL_TEXTURE_2D);
}
static void glTexParameterf_shim(GLenum target, GLenum pname, GLfloat param) {
	if (pname == GL_GENERATE_MIPMAP) { set_automip(target, param != 0.0f); return; }
	glTexParameterf(target, pname, param);
}
static void glTexParameteri_shim(GLenum target, GLenum pname, GLint param) {
	if (pname == GL_GENERATE_MIPMAP) { set_automip(target, param != 0); return; }
	glTexParameteri(target, pname, param);
}
static void glTexParameteriv_shim(GLenum target, GLenum pname, const GLint *params) {
	if (pname == GL_GENERATE_MIPMAP) { set_automip(target, params && params[0] != 0); return; }
	glTexParameteriv(target, pname, params);
}

#ifdef DEBUG_SOLOADER
static int glt_verbose;      // defined with the GL trace helpers below
static int glt_verbose_left;
#endif

// Diagnostic: never hand deleted textures back to vitaGL (no GPU memory reuse).
// Testing whether reused texture memory overlaps GXM's ring buffers.
#define TEX_NEVER_FREE 0

// Deferred texture deletion: the game deletes textures that were drawn in the
// frame still in flight. Hold deletions for a few swaps before handing them to
// vitaGL so the GPU (and Vita3K) never sees a freed texture.
#define TEX_DEFER_FRAMES 3
#define TEX_DEFER_MAX 1024
static GLuint tex_defer[TEX_DEFER_FRAMES][TEX_DEFER_MAX];
static int tex_defer_n[TEX_DEFER_FRAMES];
static int tex_defer_cur = 0;

static void glDeleteTextures_deferred(GLsizei n, const GLuint *textures) {
	for (GLsizei i = 0; i < n; i++) {
		if (!textures[i])
			continue;
		int *cnt = &tex_defer_n[tex_defer_cur];
		if (*cnt == TEX_DEFER_MAX) { // bucket full: delete right away
#if !TEX_NEVER_FREE
			glDeleteTextures(1, &textures[i]);
#endif
			continue;
		}
		tex_defer[tex_defer_cur][(*cnt)++] = textures[i];
	}
}

static EGLBoolean eglSwapBuffers_deferred(EGLDisplay display, EGLSurface surface) {
#ifdef DEBUG_SOLOADER
	if (glt_verbose && glt_verbose_left > 0) { glt_verbose_left--; sceClibPrintf("[gltrace] eglSwapBuffers()\n"); }
#endif
	EGLBoolean ret = eglSwapBuffers(display, surface);
#ifdef HEAP_WATCH
	{
		extern void heapwatch_check(unsigned frame);
		static unsigned frame = 0;
		heapwatch_check(++frame);
	}
#endif
	// Free the oldest bucket, then make it the current one
	tex_defer_cur = (tex_defer_cur + 1) % TEX_DEFER_FRAMES;
	if (tex_defer_n[tex_defer_cur]) {
#if !TEX_NEVER_FREE
		glDeleteTextures(tex_defer_n[tex_defer_cur], tex_defer[tex_defer_cur]);
#endif
		tex_defer_n[tex_defer_cur] = 0;
	}
	return ret;
}

#ifdef DEBUG_SOLOADER
// Trace wrappers for framebuffer/texture calls (debug builds only)
#define GLT(fmt, ...) sceClibPrintf("[gltrace] " fmt "\n", ##__VA_ARGS__)
static void t_glBindFramebuffer(GLenum t, GLuint fb) { GLT("glBindFramebuffer(0x%x, %u)", t, fb); glBindFramebuffer(t, fb); }
static void t_glBindRenderbuffer(GLenum t, GLuint rb) { GLT("glBindRenderbuffer(0x%x, %u)", t, rb); glBindRenderbuffer(t, rb); }
static GLenum t_glCheckFramebufferStatus(GLenum t) { GLenum r = glCheckFramebufferStatus(t); GLT("glCheckFramebufferStatus(0x%x) = 0x%x", t, r); return r; }
static void t_glDeleteFramebuffers(GLsizei n, const GLuint *f) { GLT("glDeleteFramebuffers(%d, first=%u)", n, n > 0 ? f[0] : 0); glDeleteFramebuffers(n, f); }
static void t_glDeleteTextures(GLsizei n, const GLuint *t) { glt_verbose = 1; GLT("glDeleteTextures(%d, first=%u) [deferred]", n, n > 0 ? t[0] : 0); glDeleteTextures_deferred(n, t); }
static void t_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb) { GLT("glFramebufferRenderbuffer(0x%x, 0x%x, 0x%x, %u)", t, a, rt, rb); glFramebufferRenderbuffer(t, a, rt, rb); }
static void t_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { GLT("glFramebufferTexture2D(0x%x, 0x%x, 0x%x, %u, %d)", t, a, tt, tex, l); glFramebufferTexture2D(t, a, tt, tex, l); }
static void t_glGenFramebuffers(GLsizei n, GLuint *f) { glGenFramebuffers(n, f); GLT("glGenFramebuffers(%d) -> %u", n, n > 0 ? f[0] : 0); }
static void t_glGenRenderbuffers(GLsizei n, GLuint *r) { glGenRenderbuffers(n, r); GLT("glGenRenderbuffers(%d) -> %u", n, n > 0 ? r[0] : 0); }
static void t_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { GLT("glRenderbufferStorage(0x%x, 0x%x, %d, %d)", t, f, w, h); glRenderbufferStorage(t, f, w, h); }
static void t_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d) { GLT("glTexImage2D(0x%x, %d, 0x%x, %d, %d, fmt=0x%x, type=0x%x, data=%p)%s", t, l, i, w, h, f, ty, d, (bound_tex2d[active_unit] < AUTOMIP_MAX && automip[bound_tex2d[active_unit]]) ? " +mipmaps" : ""); glTexImage2D_shim(t, l, i, w, h, b, f, ty, d); }
static void t_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { GLT("glViewport(%d, %d, %d, %d)", x, y, w, h); glViewport(x, y, w, h); }

// Once the game first deletes a texture (a few ms before the Vita3K crash,
// right after the splash), log every call instead of only new combinations.
static int glt_verbose = 0;
static int glt_verbose_left = 3000;
// Log each distinct (call, args) combination only once
static int glt_seen(uint32_t a, uint32_t b, uint32_t c) {
	if (glt_verbose && glt_verbose_left > 0) { glt_verbose_left--; return 0; }
	static uint32_t seen[4096][3];
	static int n = 0;
	for (int i = 0; i < n; i++)
		if (seen[i][0] == a && seen[i][1] == b && seen[i][2] == c) return 1;
	if (n < 4096) { seen[n][0] = a; seen[n][1] = b; seen[n][2] = c; n++; }
	return 0;
}
#define GLT1(id, a, b, fmt, ...) do { if (!glt_seen(id, (uint32_t)(a), (uint32_t)(b))) GLT(fmt, ##__VA_ARGS__); } while (0)
static uint32_t f2u(float f) { union { float f; uint32_t u; } x = { f }; return x.u; }

// Client state tracking so each draw logs its full vertex setup
static uint32_t glt_cs_mask; // bit0 vertex, bit1 normal, bit2 color, bit3 texcoord
static uint32_t glt_ptr_fmt[4]; // (size << 16) | type
static GLuint glt_bound_buffer;
static int glt_tex2d;
static int glt_cs_bit(GLenum a) { return a == 0x8074 ? 0 : a == 0x8075 ? 1 : a == 0x8076 ? 2 : a == 0x8078 ? 3 : -1; }
static void t_glVertexPointer(GLint sz, GLenum ty, GLsizei st, const void *p) { glt_ptr_fmt[0] = (sz << 16) | ty; GLT1(20, glt_ptr_fmt[0], st, "glVertexPointer(size=%d, type=0x%x, stride=%d) vbo=%u", sz, ty, st, glt_bound_buffer); glVertexPointer(sz, ty, st, p); }
static void t_glColorPointer(GLint sz, GLenum ty, GLsizei st, const void *p) { glt_ptr_fmt[2] = (sz << 16) | ty; GLT1(21, glt_ptr_fmt[2], st, "glColorPointer(size=%d, type=0x%x, stride=%d) vbo=%u", sz, ty, st, glt_bound_buffer); glColorPointer(sz, ty, st, p); }
static void t_glTexCoordPointer(GLint sz, GLenum ty, GLsizei st, const void *p) { glt_ptr_fmt[3] = (sz << 16) | ty; GLT1(22, glt_ptr_fmt[3], st, "glTexCoordPointer(size=%d, type=0x%x, stride=%d) vbo=%u", sz, ty, st, glt_bound_buffer); glTexCoordPointer(sz, ty, st, p); }
static void t_glBindBuffer(GLenum t, GLuint b) { if (t == 0x8892) glt_bound_buffer = b; GLT1(23, t, b ? 1 : 0, "glBindBuffer(0x%x, %s)", t, b ? "nonzero" : "0"); glBindBuffer(t, b); }
static void t_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { GLT1(24, 0, 0, "glColor4f(...) first use"); glColor4f(r, g, b, a); }
static void glt_draw(const char *fn, GLenum m, GLsizei c, GLenum it) {
	uint32_t key = (m << 24) ^ (glt_cs_mask << 16) ^ (glt_tex2d << 15) ^ (it & 0x7fff);
	uint32_t fmt = glt_ptr_fmt[0] ^ (glt_ptr_fmt[2] << 3) ^ (glt_ptr_fmt[3] << 7);
	if (!glt_seen(30, key, fmt))
		GLT("%s(mode=0x%x, count=%d, idxtype=0x%x) arrays=[%s%s%s%s] tex2d=%d vtx=%d/0x%x col=%d/0x%x tc=%d/0x%x vbo=%u",
			fn, m, c, it,
			(glt_cs_mask & 1) ? "V" : "", (glt_cs_mask & 2) ? "N" : "", (glt_cs_mask & 4) ? "C" : "", (glt_cs_mask & 8) ? "T" : "",
			glt_tex2d, glt_ptr_fmt[0] >> 16, glt_ptr_fmt[0] & 0xffff, glt_ptr_fmt[2] >> 16, glt_ptr_fmt[2] & 0xffff,
			glt_ptr_fmt[3] >> 16, glt_ptr_fmt[3] & 0xffff, glt_bound_buffer);
}
static void t_glEnable(GLenum c) { if (c == 0xde1) glt_tex2d = 1; GLT1(1, c, 0, "glEnable(0x%x)%s", c, gles1_unsupported_cap(c) ? " [ignored]" : ""); glEnable_shim(c); }
static void t_glDisable(GLenum c) { if (c == 0xde1) glt_tex2d = 0; GLT1(2, c, 0, "glDisable(0x%x)", c); glDisable_shim(c); }
static void t_glTexEnvf(GLenum t, GLenum p, GLfloat v) { GLT1(3, p, f2u(v), "glTexEnvf(0x%x, 0x%x, %f)", t, p, v); glTexEnvf(t, p, v); }
static void t_glTexEnvi(GLenum t, GLenum p, GLint v) { GLT1(4, p, v, "glTexEnvi(0x%x, 0x%x, 0x%x)", t, p, v); glTexEnvi(t, p, v); }
static void t_glTexEnvfv(GLenum t, GLenum p, const GLfloat *v) { GLT1(5, p, 0, "glTexEnvfv(0x%x, 0x%x, {%f,%f,%f,%f})", t, p, v[0], v[1], v[2], v[3]); glTexEnvfv(t, p, v); }
static void t_glDrawArrays(GLenum m, GLint f, GLsizei c) { glt_draw("glDrawArrays", m, c, 0); glDrawArrays(m, f, c); }
static void t_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) { glt_draw("glDrawElements", m, c, t); glDrawElements(m, c, t, i); }
static void t_glPointSize(GLfloat v) { GLT1(8, f2u(v), 0, "glPointSize(%f)", v); glPointSize(v); }
static void t_glLineWidth(GLfloat v) { GLT1(9, f2u(v), 0, "glLineWidth(%f)", v); glLineWidth(v); }
static void t_glEnableClientState(GLenum a) { int b = glt_cs_bit(a); if (b >= 0) glt_cs_mask |= 1u << b; GLT1(10, a, 0, "glEnableClientState(0x%x)", a); glEnableClientState(a); }
static void t_glDisableClientState(GLenum a) { int b = glt_cs_bit(a); if (b >= 0) glt_cs_mask &= ~(1u << b); GLT1(11, a, 0, "glDisableClientState(0x%x)", a); glDisableClientState(a); }
static void t_glActiveTexture(GLenum t) { GLT1(12, t, 0, "glActiveTexture(0x%x)", t); glActiveTexture_shim(t); }
static void t_glClientActiveTexture(GLenum t) { GLT1(13, t, 0, "glClientActiveTexture(0x%x)", t); glClientActiveTexture(t); }
static void t_glBlendFunc(GLenum a, GLenum b) { GLT1(14, a, b, "glBlendFunc(0x%x, 0x%x)", a, b); glBlendFunc(a, b); }

static void t_glBindTexture(GLenum t, GLuint tex) { GLT1(40, t, tex, "glBindTexture(0x%x, %u)", t, tex); glBindTexture_shim(t, tex); }
static void t_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { GLT1(41, (x << 16) ^ y, (w << 16) ^ h, "glScissor(%d, %d, %d, %d)", x, y, w, h); glScissor(x, y, w, h); }
static void t_glClear(GLbitfield m) { GLT1(42, m, 0, "glClear(0x%x)", m); glClear_shim(m); }
static void t_glDepthMask(GLboolean f) { GLT1(43, f, 0, "glDepthMask(%d)", f); glDepthMask(f); }
static void t_glDepthFunc(GLenum f) { GLT1(44, f, 0, "glDepthFunc(0x%x)", f); glDepthFunc(f); }
static void t_glMatrixMode(GLenum m) { GLT1(45, m, 0, "glMatrixMode(0x%x)", m); glMatrixMode(m); }
static void t_glLoadIdentity(void) { GLT1(46, 0, 0, "glLoadIdentity()"); glLoadIdentity(); }
static void t_glLoadMatrixf(const GLfloat *m) { GLT1(47, 0, 0, "glLoadMatrixf(%f %f %f %f | %f %f %f %f)", m[0], m[5], m[10], m[15], m[12], m[13], m[14], m[3]); glLoadMatrixf(m); }
static void t_glMultMatrixf(const GLfloat *m) { GLT1(48, 0, 0, "glMultMatrixf(%f %f %f %f | %f %f %f %f)", m[0], m[5], m[10], m[15], m[12], m[13], m[14], m[3]); glMultMatrixf(m); }
static void t_glGenTextures(GLsizei n, GLuint *t) { glGenTextures_shim(n, t); GLT1(49, 0, 0, "glGenTextures(%d) -> %u", n, n > 0 ? t[0] : 0); }
static void t_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *d) { GLT1(50, (w << 16) ^ h, f, "glTexSubImage2D(%d,%d %dx%d fmt=0x%x type=0x%x)", x, y, w, h, f, ty); glTexSubImage2D_shim(t, l, x, y, w, h, f, ty, d); }
static void t_glFrontFace(GLenum m) { GLT1(51, m, 0, "glFrontFace(0x%x)", m); glFrontFace(m); }
#define GLWRAP(fn) t_##fn
#else
#define GLWRAP(fn) fn
#endif

#ifdef HEAP_WATCH
void *hw_malloc(size_t); void *hw_calloc(size_t, size_t); void *hw_realloc(void *, size_t);
void *hw_memalign(size_t, size_t);
#endif
#ifdef USE_SCELIBC_IO
// Trace I/O on .ui files (UI systems come up with 0 widgets): which FILE*s
// belong to .ui files is recorded by fopen_soloader().
extern void *ui_trace_files[8];
void game_log_file(const char *tag, const char *text);
// .ui read tracing found the rewind() bug; keep it off so game_log.txt stays readable
#define UI_IO_TRACE 0
static int ui_traced(void *f) {
#if UI_IO_TRACE
	for (int i = 0; i < 8; i++) if (f && ui_trace_files[i] == f) return 1;
#endif
	(void)f;
	return 0;
}
static size_t trace_fread(void *buf, size_t sz, size_t n, FILE *f) {
	size_t r = sceLibcBridge_fread(buf, sz, n, f);
	if (ui_traced(f)) {
		char m[160];
		snprintf(m, sizeof(m), "fread(%p, %u, %u, %p) = %u", buf, (unsigned)sz, (unsigned)n, f, (unsigned)r);
		game_log_file("uiio", m);
	}
	return r;
}
static int trace_fseek(FILE *f, long off, int whence) {
	int r = sceLibcBridge_fseek(f, off, whence);
	if (ui_traced(f)) {
		char m[160];
		snprintf(m, sizeof(m), "fseek(%p, %ld, %d) = %d", f, off, whence, r);
		game_log_file("uiio", m);
	}
	return r;
}
static long trace_ftell(FILE *f) {
	long r = sceLibcBridge_ftell(f);
	if (ui_traced(f)) {
		char m[160];
		snprintf(m, sizeof(m), "ftell(%p) = %ld", f, r);
		game_log_file("uiio", m);
	}
	return r;
}
#endif

#ifdef USE_SCELIBC_IO
static void rewind_soloader(FILE *f) {
	// rewind() == fseek(f, 0, SEEK_SET) plus clearing the error indicator
	sceLibcBridge_fseek(f, 0, SEEK_SET);
}
#endif

// The window covers the whole screen, so raw (screen) == window coordinates
static float AMotionEvent_getRawX_soloader(const AInputEvent *e, size_t i) { return AMotionEvent_getX(e, i); }
static float AMotionEvent_getRawY_soloader(const AInputEvent *e, size_t i) { return AMotionEvent_getY(e, i); }
// Android event times are in nanoseconds (SYSTEM_TIME_MONOTONIC)
static int64_t AKeyEvent_getEventTime_soloader(const AInputEvent *e) {
	(void)e;
	return (int64_t)sceKernelGetProcessTimeWide() * 1000;
}

so_default_dynlib default_dynlib[] = {
        // Common C/C++ internals
        { "_ZNSt8bad_castD1Ev", (uintptr_t)&_ZNSt8bad_castD1Ev },
        { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
        { "_ZSt17__throw_bad_allocv", (uintptr_t)&_ZSt17__throw_bad_allocv },
        { "_ZSt9terminatev", (uintptr_t)&_ZSt9terminatev },
        { "_ZTISt8bad_cast", (uintptr_t)&_ZTISt8bad_cast },
        { "_ZTISt9exception", (uintptr_t)&_ZTISt9exception },
        { "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE },
        { "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE },
        { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv121__vmi_class_type_infoE },
        { "_ZdaPv", (uintptr_t)&_ZdaPv },
        { "_ZdlPv", (uintptr_t)&_ZdlPv },
        { "_Znaj", (uintptr_t)&_Znaj },
        { "_Znwj", (uintptr_t)&_Znwj },
        { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
        { "__aeabi_d2lz", (uintptr_t)&__aeabi_d2lz },
        { "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
        { "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
        { "__aeabi_dcmpgt", (uintptr_t)&__aeabi_dcmpgt },
        { "__aeabi_dcmplt", (uintptr_t)&__aeabi_dcmplt },
        { "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
        { "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
        { "__aeabi_f2lz", (uintptr_t)&__aeabi_f2lz },
        { "__aeabi_f2ulz", (uintptr_t)&__aeabi_f2ulz },
        { "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
        { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
        { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
        { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
        { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
        { "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
        { "__aeabi_memclr", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr4", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr8", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memmove", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove4", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove8", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memset", (uintptr_t)&__aeabi_memset },
        { "__aeabi_memset4",  (uintptr_t)&__aeabi_memset4 },
        { "__aeabi_memset8", (uintptr_t)&__aeabi_memset8 },
        { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
        { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
        { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
        { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
        { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
        { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
        { "__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0 },
        { "__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1 },
        { "__atomic_cmpxchg", (uintptr_t)&__atomic_cmpxchg },
        { "__atomic_dec", (uintptr_t)&__atomic_dec },
        { "__atomic_inc", (uintptr_t)&__atomic_inc },
        { "__atomic_swap", (uintptr_t)&__atomic_swap },
        { "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception },
        { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
        { "__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch },
        { "__cxa_begin_cleanup", (uintptr_t)&ret0 },
        { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
        { "__cxa_end_catch", (uintptr_t)&__cxa_end_catch },
        { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
        { "__cxa_free_exception", (uintptr_t)&__cxa_free_exception },
        { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
        { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
        { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
        { "__cxa_rethrow", (uintptr_t)&__cxa_rethrow },
        { "__cxa_throw", (uintptr_t)&__cxa_throw },
        { "__cxa_type_match", (uintptr_t)&ret0 },
        { "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
        { "__gnu_ldivmod_helper", (uintptr_t)&__gnu_ldivmod_helper },
        { "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
        { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
        { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
        { "__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0 },
        { "__isfinite", (uintptr_t)&__isfinite_soloader },
        { "__isfinitef", (uintptr_t)&__isfinitef_soloader },
        { "__isinf", (uintptr_t)&__isinf_soloader },
        { "__isinff", (uintptr_t)&__isinff_soloader },
        { "__isnan", (uintptr_t)&__isnan_soloader },
        { "__isnanf", (uintptr_t)&__isnanf_soloader },
        { "__page_size", (uintptr_t)&__page_size },
        { "__sF", (uintptr_t)&__sF_fake },
        { "__srget", (uintptr_t)&__srget },
        { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_soloader },
        { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
        { "__swbuf", (uintptr_t)&__swbuf },
        { "__system_property_get", (uintptr_t)&__system_property_get_soloader },
        { "__assert2", (uintptr_t)&ret0 }, // TODO: stub/impl
        { "dl_unwind_find_exidx", (uintptr_t)&ret0 }, // TODO: stub/impl


        // ctype
        { "_ctype_", (uintptr_t)&BIONIC_ctype_ },
        { "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_ },
        { "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_ },
        { "isalnum", (uintptr_t)&isalnum },
        { "isalpha", (uintptr_t)&isalpha },
        { "isblank", (uintptr_t)&isblank },
        { "iscntrl", (uintptr_t)&iscntrl },
        { "isgraph", (uintptr_t)&isgraph },
        { "islower", (uintptr_t)&islower },
        { "isprint", (uintptr_t)&isprint },
        { "ispunct", (uintptr_t)&ispunct },
        { "isspace", (uintptr_t)&isspace },
        { "isupper", (uintptr_t)&isupper },
        { "isxdigit", (uintptr_t)&isxdigit },
        { "tolower", (uintptr_t)&tolower },
        { "toupper", (uintptr_t)&toupper },


        // Android SDK standard logging
        { "__android_log_assert", (uintptr_t)&__android_log_assert },
        { "__android_log_print", (uintptr_t)&__android_log_print },
        { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
        { "__android_log_write", (uintptr_t)&__android_log_write },


        // AAssetManager
        { "AAsset_close", (uintptr_t)&AAsset_close },
        { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
        { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength },
        { "AAsset_read", (uintptr_t)&AAsset_read },
        { "AAsset_seek", (uintptr_t)&AAsset_seek },
        { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor },
        { "AAssetDir_close", (uintptr_t)&AAssetDir_close },
        { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
        { "AAssetManager_fromJava", (uintptr_t)&ret1 },
        { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
        { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },

        
#ifdef NDK_PORT
        // FalsoNDK
        {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete},
        {"AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager},
        {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry},
        {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage},
        {"AConfiguration_new", (uintptr_t)&AConfiguration_new},
        {"AInputEvent_getDeviceId", (uintptr_t)&AInputEvent_getDeviceId},
        {"AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource},
        {"AInputEvent_getType", (uintptr_t)&AInputEvent_getType},
        {"AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper},
        {"AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper},
        {"AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent},
        {"AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent},
        {"AInputQueue_hasEvents", (uintptr_t)&AInputQueue_hasEvents},
        {"AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent},
        {"AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction},
        {"AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode},
        {"ALooper_addFd", (uintptr_t)&ALooper_addFd},
        {"ALooper_pollAll", (uintptr_t)&ALooper_pollAll},
        {"ALooper_prepare", (uintptr_t)&ALooper_prepare},
        {"AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction},
        {"AMotionEvent_getAxisValue", (uintptr_t)&AMotionEvent_getAxisValue},
        {"AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount},
        {"AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX},
        // Imports the game uses that were unresolved (touch at the Start screen
        // crashed in reloc_err via AMotionEvent_getRawX)
        {"AMotionEvent_getRawX", (uintptr_t)&AMotionEvent_getRawX_soloader},
        {"AMotionEvent_getRawY", (uintptr_t)&AMotionEvent_getRawY_soloader},
        {"AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId},
        {"AKeyEvent_getRepeatCount", (uintptr_t)&AKeyEvent_getRepeatCount},
        {"AKeyEvent_getEventTime", (uintptr_t)&AKeyEvent_getEventTime_soloader},
        {"ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor},
        {"ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor},
        {"ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate},
        {"AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY},
        {"ANativeActivity_finish", (uintptr_t)&ANativeActivity_finish},
        {"ANativeActivity_setWindowFlags", (uintptr_t)&ANativeActivity_setWindowFlags},
        {"ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight},
        {"ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth},
        {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry},
#endif


        // Math
        { "acos", (uintptr_t)&acos },
        { "acosf", (uintptr_t)&acosf },
        { "arc4random", (uintptr_t)&arc4random },
        { "asin", (uintptr_t)&asin },
        { "asinf", (uintptr_t)&asinf },
        { "atan", (uintptr_t)&atan },
        { "atan2", (uintptr_t)&atan2 },
        { "atan2f", (uintptr_t)&atan2f },
        { "atanf", (uintptr_t)&atanf },
        { "ceil", (uintptr_t)&ceil },
        { "ceilf", (uintptr_t)&ceilf },
        { "cos", (uintptr_t)&cos },
        { "cosf", (uintptr_t)&cosf },
        { "cosh", (uintptr_t)&cosh },
        { "exp", (uintptr_t)&exp },
        { "exp2", (uintptr_t)&exp2 },
        { "exp2f", (uintptr_t)&exp2f },
        { "expf", (uintptr_t)&expf },
        { "floor", (uintptr_t)&floor },
        { "floorf", (uintptr_t)&floorf },
        { "fmax", (uintptr_t)&fmax },
        { "fmaxf", (uintptr_t)&fmaxf },
        { "fmin", (uintptr_t)&fmin },
        { "fminf", (uintptr_t)&fminf },
        { "frexpf", (uintptr_t)&frexpf },
        { "fmod", (uintptr_t)&fmod },
        { "fmodf", (uintptr_t)&fmodf },
        { "frexp", (uintptr_t)&frexp },
        { "ldexp", (uintptr_t)&ldexp },
        { "ldexpf", (uintptr_t)&ldexpf },
        { "log", (uintptr_t)&log },
        { "log10", (uintptr_t)&log10 },
        { "log10f", (uintptr_t)&log10f },
        { "logf", (uintptr_t)&logf },
        { "lrint", (uintptr_t)&lrint },
        { "lrintf", (uintptr_t)&lrintf },
        { "lround", (uintptr_t)&lround },
        { "lroundf", (uintptr_t)&lroundf },
        { "modf", (uintptr_t)&modf },
        { "pow", (uintptr_t)&pow },
        { "powf", (uintptr_t)&powf },
        { "rint", (uintptr_t)&rint },
        { "rintf", (uintptr_t)&rintf },
        { "round", (uintptr_t)&round },
        { "roundf", (uintptr_t)&roundf },
        { "scalbn", (uintptr_t)&scalbn },
        { "scalbnf", (uintptr_t)&scalbnf },
        { "sin", (uintptr_t)&sin },
        { "sincos", (uintptr_t)&sincos },
        { "sincosf", (uintptr_t)&sincosf },
        { "sinf", (uintptr_t)&sinf },
        { "sinh", (uintptr_t)&sinh },
        { "sinhf", (uintptr_t)&sinhf },
        { "sqrt", (uintptr_t)&sqrt },
        { "sqrtf", (uintptr_t)&sqrtf },
        { "tan", (uintptr_t)&tan },
        { "tanf", (uintptr_t)&tanf },
        { "tanh", (uintptr_t)&tanh },
        { "trunc", (uintptr_t)&trunc },
        { "truncf", (uintptr_t)&truncf },


        // Sockets
        { "accept", (uintptr_t)&accept },
        { "bind", (uintptr_t)&bind },
        { "connect", (uintptr_t)&connect },
        { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
        { "gai_strerror", (uintptr_t)&ret0 },
        { "getaddrinfo", (uintptr_t)&getaddrinfo },
        { "gethostbyaddr", (uintptr_t)&gethostbyaddr },
        { "gethostbyname", (uintptr_t)&gethostbyname },
        { "gethostname", (uintptr_t)&gethostname },
        { "getpeername", (uintptr_t)&getpeername },
        { "getservbyname", (uintptr_t)&getservbyname },
        { "getsockname", (uintptr_t)&getsockname },
        { "getsockopt", (uintptr_t)&getsockopt },
        { "inet_aton", (uintptr_t)&inet_aton },
        { "inet_pton", (uintptr_t)&inet_pton },
        { "inet_ntoa", (uintptr_t)&inet_ntoa },
        { "inet_ntop", (uintptr_t)&inet_ntop },
        { "listen", (uintptr_t)&listen },
        { "poll", (uintptr_t)&poll },
        { "recv", (uintptr_t)&recv },
        { "recvfrom", (uintptr_t)&recvfrom },
        { "recvmsg", (uintptr_t)&recvmsg },
        { "select", (uintptr_t)&select },
        { "send", (uintptr_t)&send },
        { "sendmsg", (uintptr_t)&sendmsg },
        { "sendto", (uintptr_t)&sendto },
        { "setsockopt", (uintptr_t)&setsockopt },
        { "shutdown", (uintptr_t)&shutdown },
        { "socket", (uintptr_t)&socket },


        // Memory
#ifdef HEAP_WATCH
        { "calloc", (uintptr_t)&hw_calloc },
#else
        { "calloc", (uintptr_t)&calloc },
#endif
        { "free", (uintptr_t)&free },
#ifdef HEAP_WATCH
        { "malloc", (uintptr_t)&hw_malloc },
#else
        { "malloc", (uintptr_t)&malloc },
#endif
#ifdef HEAP_WATCH
        { "memalign", (uintptr_t)&hw_memalign },
#else
        { "memalign", (uintptr_t)&memalign },
#endif
        { "memcmp", (uintptr_t)&memcmp },
        { "memcpy", (uintptr_t)&sceClibMemcpy },
        { "memmem", (uintptr_t)&memmem },
        { "memmove", (uintptr_t)&memmove },
        { "memset", (uintptr_t)&memset },
        { "mmap", (uintptr_t)&mmap },
        { "__mmap2", (uintptr_t)&mmap },
        { "munmap", (uintptr_t)&munmap },
#ifdef HEAP_WATCH
        { "realloc", (uintptr_t)&hw_realloc },
#else
        { "realloc", (uintptr_t)&realloc },
#endif
        { "valloc", (uintptr_t)&valloc },


        // IO
#ifndef NDK_PORT
        { "close", (uintptr_t)&close_soloader },
#else   
        { "close", (uintptr_t)&fndk_close },
#endif
        { "closedir", (uintptr_t)&closedir_soloader },
        { "execv", (uintptr_t)&ret0 },
        { "fclose", (uintptr_t)&fclose_soloader },
        { "fcntl", (uintptr_t)&fcntl_soloader },
        { "fopen", (uintptr_t)&fopen_soloader },
        { "fstat", (uintptr_t)&fstat_soloader },
        { "fsync", (uintptr_t)&fsync_soloader },
        { "ioctl", (uintptr_t)&ioctl_soloader },
        { "__open_2", (uintptr_t)&open_soloader },
        { "open", (uintptr_t)&open_soloader },
        { "opendir", (uintptr_t)&opendir_soloader },
        { "readdir", (uintptr_t)&readdir_soloader },
        { "readdir_r", (uintptr_t)&readdir_r_soloader },
        { "stat", (uintptr_t)&stat_soloader },
        { "utime", (uintptr_t)&utime },

        #ifdef USE_SCELIBC_IO
            { "fdopen", (uintptr_t)&sceLibcBridge_fdopen },
            { "feof", (uintptr_t)&sceLibcBridge_feof },
            { "ferror", (uintptr_t)&sceLibcBridge_ferror },
            { "fflush", (uintptr_t)&sceLibcBridge_fflush },
            { "fgetc", (uintptr_t)&sceLibcBridge_fgetc },
            { "fgetpos", (uintptr_t)&sceLibcBridge_fgetpos },
            { "fgets", (uintptr_t)&sceLibcBridge_fgets },
            { "fileno", (uintptr_t)&sceLibcBridge_fileno },
            { "fputc", (uintptr_t)&sceLibcBridge_fputc },
            { "fputs", (uintptr_t)&sceLibcBridge_fputs },
            { "fread", (uintptr_t)&trace_fread },
            { "freopen", (uintptr_t)&sceLibcBridge_freopen },
            { "fseek", (uintptr_t)&trace_fseek },
            { "fsetpos", (uintptr_t)&sceLibcBridge_fsetpos },
            { "ftell", (uintptr_t)&trace_ftell },
            { "fwide", (uintptr_t)&sceLibcBridge_fwide },
            { "fwrite", (uintptr_t)&sceLibcBridge_fwrite },
            { "getc", (uintptr_t)&sceLibcBridge_getc },
            { "getwc", (uintptr_t)&sceLibcBridge_getwc },
            { "putc", (uintptr_t)&sceLibcBridge_putc },
            { "putchar", (uintptr_t)&sceLibcBridge_putchar },
            { "puts", (uintptr_t)&sceLibcBridge_puts },
            { "putwc", (uintptr_t)&sceLibcBridge_putwc },
            { "setvbuf", (uintptr_t)&sceLibcBridge_setvbuf },
            { "ungetc", (uintptr_t)&sceLibcBridge_ungetc },
            { "ungetwc", (uintptr_t)&sceLibcBridge_ungetwc },
        #else
            { "fdopen", (uintptr_t)&fdopen },
            { "feof", (uintptr_t)&feof },
            { "ferror", (uintptr_t)&ferror },
            { "fflush", (uintptr_t)&fflush },
            { "fgetc", (uintptr_t)&fgetc },
            { "fgetpos", (uintptr_t)&fgetpos },
            { "fgets", (uintptr_t)&fgets },
            { "fileno", (uintptr_t)&fileno },
            { "fputc", (uintptr_t)&fputc },
            { "fputs", (uintptr_t)&fputs },
            { "fread", (uintptr_t)&fread },
            { "freopen", (uintptr_t)&freopen },
            { "fseek", (uintptr_t)&fseek },
            { "fsetpos", (uintptr_t)&fsetpos },
            { "ftell", (uintptr_t)&ftell },
            { "fwide", (uintptr_t)&fwide },
            { "fwrite", (uintptr_t)&fwrite },
            { "getc", (uintptr_t)&getc },
            { "getwc", (uintptr_t)&getwc },
            { "putc", (uintptr_t)&putc },
            { "putchar", (uintptr_t)&putchar },
            { "puts", (uintptr_t)&puts },
            { "putwc", (uintptr_t)&putwc },
            { "setvbuf", (uintptr_t)&setvbuf },
            { "ungetc", (uintptr_t)&ungetc },
            { "ungetwc", (uintptr_t)&ungetwc },
        #endif

        { "access", (uintptr_t)&access },
        { "basename", (uintptr_t)&basename },
        { "chdir", (uintptr_t)&chdir },
        { "chmod", (uintptr_t)&chmod },
        { "dup", (uintptr_t)&dup },
        { "fseeko", (uintptr_t)&fseeko }, // TODO: wrap normal fseek for SceLibc version?
        { "ftello", (uintptr_t)&ftello },
        { "ftruncate", (uintptr_t)&ftruncate },
        { "getcwd", (uintptr_t)&getcwd },
        { "lseek", (uintptr_t)&lseek },
        { "lseek64", (uintptr_t)&ret0 }, // TODO: implement or stub with warning
        { "lstat", (uintptr_t)&lstat },
        { "mkdir", (uintptr_t)&mkdir },
#ifndef NDK_PORT
        { "pipe", (uintptr_t)&pipe },
        { "read", (uintptr_t)&read },
#else
        { "pipe", (uintptr_t)&fndk_pipe },
        { "read", (uintptr_t)&fndk_read },
#endif
        { "realpath", (uintptr_t)&realpath },
        { "remove", (uintptr_t)&remove },
        { "rename", (uintptr_t)&rename },
#ifdef USE_SCELIBC_IO
        // FILE*s come from sceLibcBridge_fopen: newlib's rewind() doesn't
        // understand them (the game's fseek-END/ftell/rewind/fread size probe
        // read 0 bytes, so every .ui file loaded as an empty UI system).
        { "rewind", (uintptr_t)&rewind_soloader },
#else
        { "rewind", (uintptr_t)&rewind },
#endif
        { "rmdir", (uintptr_t)&rmdir },
        { "truncate", (uintptr_t)&truncate },
        { "unlink", (uintptr_t)&unlink },
#ifndef NDK_PORT
        { "write", (uintptr_t)&write },
#else
        { "write", (uintptr_t)&fndk_write },
#endif


        // *printf, *scanf
        { "snprintf", (uintptr_t)&snprintf },
        { "sprintf", (uintptr_t)&sprintf },
        { "vasprintf", (uintptr_t)&vasprintf },
        { "vprintf", (uintptr_t)&vprintf },
        { "vsnprintf", (uintptr_t)&vsnprintf },
        { "vsprintf", (uintptr_t)&vsprintf },
        { "vsscanf", (uintptr_t)&vsscanf },
        { "vswprintf", (uintptr_t)&vswprintf },
        { "printf", (uintptr_t)&sceClibPrintf },
        { "swprintf", (uintptr_t)&swprintf },

        #ifdef USE_SCELIBC_IO
            { "fprintf", (uintptr_t)&sceLibcBridge_fprintf },
            { "fscanf", (uintptr_t)&sceLibcBridge_fscanf },
            { "sscanf", (uintptr_t)&sceLibcBridge_sscanf },
            { "vfprintf", (uintptr_t)&sceLibcBridge_vfprintf },
        #else
            { "fprintf", (uintptr_t)&fprintf },
            { "fscanf", (uintptr_t)&fscanf },
            { "sscanf", (uintptr_t)&sscanf },
            { "vfprintf", (uintptr_t)&vfprintf },
        #endif


        // EGL
        { "eglBindAPI", (uintptr_t)&eglBindAPI },
        { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
        { "eglCreateContext", (uintptr_t)&eglCreateContext },
        { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
        { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
        { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
        { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
        { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
        { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
        { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
        { "eglGetError", (uintptr_t)&eglGetError },
        { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
        { "eglInitialize", (uintptr_t)&eglInitialize },
        { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
        { "eglQueryContext", (uintptr_t)&eglQueryContext },
        { "eglQueryString", (uintptr_t)&eglQueryString },
        { "eglQuerySurface", (uintptr_t)&eglQuerySurface },
        { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers_deferred },
        { "eglTerminate", (uintptr_t)&eglTerminate },


        // OpenGL
        #ifdef DEBUG_SOLOADER
        { "glActiveTexture", (uintptr_t)&t_glActiveTexture },
#else
        { "glActiveTexture", (uintptr_t)&glActiveTexture_shim },
#endif
        { "glAlphaFunc", (uintptr_t)&glAlphaFunc },
        { "glAlphaFuncx", (uintptr_t)&glAlphaFuncx },
        { "glAttachShader", (uintptr_t)&glAttachShader },
        { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
        { "glBindBuffer", (uintptr_t)&GLWRAP(glBindBuffer) },
        { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
        { "glBindFramebufferOES", (uintptr_t)&GLWRAP(glBindFramebuffer) },
        { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
        { "glBindRenderbufferOES", (uintptr_t)&GLWRAP(glBindRenderbuffer) },
        #ifdef DEBUG_SOLOADER
        { "glBindTexture", (uintptr_t)&t_glBindTexture },
#else
        { "glBindTexture", (uintptr_t)&glBindTexture_shim },
#endif
        { "glBlendColor", (uintptr_t)&ret0 },
        { "glBlendEquation", (uintptr_t)&glBlendEquation },
        { "glBlendEquationOES", (uintptr_t)&glBlendEquation },
        { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendEquationSeparateOES", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendFunc", (uintptr_t)&GLWRAP(glBlendFunc) },
        { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
        { "glBlendFuncSeparateOES", (uintptr_t)&glBlendFuncSeparate },
        { "glBufferData", (uintptr_t)&glBufferData },
        { "glBufferSubData", (uintptr_t)&glBufferSubData },
        { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
        { "glCheckFramebufferStatusOES", (uintptr_t)&GLWRAP(glCheckFramebufferStatus) },
        #ifdef DEBUG_SOLOADER
        { "glClear", (uintptr_t)&t_glClear },
#else
        { "glClear", (uintptr_t)&glClear_shim },
#endif
        { "glClearColor", (uintptr_t)&glClearColor },
        { "glClearColorx", (uintptr_t)&glClearColorx },
        { "glClearDepthf", (uintptr_t)&glClearDepthf },
        { "glClearDepthx", (uintptr_t)&glClearDepthx },
        { "glClearStencil", (uintptr_t)&glClearStencil },
        { "glClientActiveTexture", (uintptr_t)&GLWRAP(glClientActiveTexture) },
        { "glClipPlanef", (uintptr_t)&glClipPlanef },
        { "glClipPlanex", (uintptr_t)&glClipPlanex },
        { "glColor4f", (uintptr_t)&GLWRAP(glColor4f) },
        { "glColor4ub", (uintptr_t)&glColor4ub },
        { "glColor4x", (uintptr_t)&glColor4x },
        { "glColorMask", (uintptr_t)&glColorMask },
        { "glColorPointer", (uintptr_t)&GLWRAP(glColorPointer) },
        { "glCompileShader", (uintptr_t)&glCompileShader_soloader },
        { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
        { "glCompressedTexSubImage2D", (uintptr_t)&ret0 },
        { "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D },
        { "glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D },
        { "glCreateProgram", (uintptr_t)&glCreateProgram },
        { "glCreateShader", (uintptr_t)&glCreateShader },
        { "glCullFace", (uintptr_t)&glCullFace },
        { "glCurrentPaletteMatrixOES", (uintptr_t)&ret0 },
        { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
        { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteFramebuffersOES", (uintptr_t)&GLWRAP(glDeleteFramebuffers) },
        { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
        { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteRenderbuffersOES", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteShader", (uintptr_t)&glDeleteShader },
        #ifdef DEBUG_SOLOADER
        { "glDeleteTextures", (uintptr_t)&t_glDeleteTextures },
#else
        { "glDeleteTextures", (uintptr_t)&glDeleteTextures_deferred },
#endif
        { "glDepthFunc", (uintptr_t)&GLWRAP(glDepthFunc) },
        { "glDepthMask", (uintptr_t)&GLWRAP(glDepthMask) },
        { "glDepthRangef", (uintptr_t)&glDepthRangef },
        { "glDepthRangex", (uintptr_t)&glDepthRangex },
        { "glDetachShader", (uintptr_t)&ret0 },
        #ifdef DEBUG_SOLOADER
        { "glDisable", (uintptr_t)&t_glDisable },
#else
        { "glDisable", (uintptr_t)&glDisable_shim },
#endif
        { "glDisableClientState", (uintptr_t)&GLWRAP(glDisableClientState) },
        { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
        { "glDrawArrays", (uintptr_t)&GLWRAP(glDrawArrays) },
        { "glDrawElements", (uintptr_t)&GLWRAP(glDrawElements) },
        { "glDrawTexfOES", (uintptr_t)&ret0 },
        { "glDrawTexfvOES", (uintptr_t)&ret0 },
        { "glDrawTexiOES", (uintptr_t)&ret0 },
        { "glDrawTexivOES", (uintptr_t)&ret0 },
        { "glDrawTexsOES", (uintptr_t)&ret0 },
        { "glDrawTexsvOES", (uintptr_t)&ret0 },
        { "glDrawTexxOES", (uintptr_t)&ret0 },
        { "glDrawTexxvOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetRenderbufferStorageOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetTexture2DOES", (uintptr_t)&ret0 },
        #ifdef DEBUG_SOLOADER
        { "glEnable", (uintptr_t)&t_glEnable },
#else
        { "glEnable", (uintptr_t)&glEnable_shim },
#endif
        { "glEnableClientState", (uintptr_t)&GLWRAP(glEnableClientState) },
        { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
        { "glFinish", (uintptr_t)&glFinish },
        { "glFlush", (uintptr_t)&glFlush },
        { "glFogf", (uintptr_t)&glFogf },
        { "glFogfv", (uintptr_t)&glFogfv },
        { "glFogx", (uintptr_t)&glFogx },
        { "glFogxv", (uintptr_t)&glFogxv },
        { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferRenderbufferOES", (uintptr_t)&GLWRAP(glFramebufferRenderbuffer) },
        { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
        { "glFramebufferTexture2DOES", (uintptr_t)&GLWRAP(glFramebufferTexture2D) },
        { "glFrontFace", (uintptr_t)&GLWRAP(glFrontFace) },
        { "glFrustumf", (uintptr_t)&glFrustumf },
        { "glFrustumx", (uintptr_t)&glFrustumx },
        { "glGenBuffers", (uintptr_t)&glGenBuffers },
        { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
        { "glGenerateMipmapOES", (uintptr_t)&glGenerateMipmap },
        { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
        { "glGenFramebuffersOES", (uintptr_t)&GLWRAP(glGenFramebuffers) },
        { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
        { "glGenRenderbuffersOES", (uintptr_t)&GLWRAP(glGenRenderbuffers) },
        #ifdef DEBUG_SOLOADER
        { "glGenTextures", (uintptr_t)&t_glGenTextures },
#else
        { "glGenTextures", (uintptr_t)&glGenTextures_shim },
#endif
        { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib },
        { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
        { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
        { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
        { "glGetBufferParameteriv", (uintptr_t)&glGetBufferParameteriv },
        { "glGetBufferPointervOES", (uintptr_t)&ret0 },
        { "glGetClipPlanef", (uintptr_t)&ret0 },
        { "glGetClipPlanex", (uintptr_t)&ret0 },
        { "glGetError", (uintptr_t)&glGetError },
        { "glGetFixedv", (uintptr_t)&ret0 },
        { "glGetFloatv", (uintptr_t)&glGetFloatv },
        { "glGetFramebufferAttachmentParameterivOES", (uintptr_t)&glGetFramebufferAttachmentParameteriv },
        { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
        { "glGetLightfv", (uintptr_t)&ret0 },
        { "glGetLightxv", (uintptr_t)&ret0 },
        { "glGetMaterialfv", (uintptr_t)&ret0 },
        { "glGetMaterialxv", (uintptr_t)&ret0 },
        { "glGetPointerv", (uintptr_t)&ret0 },
        { "glGetRenderbufferParameterivOES", (uintptr_t)&ret0 },
        { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
        { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
        { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
        { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
        { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
        { "glGetString", (uintptr_t)&glGetString },
        { "glGetTexEnvfv", (uintptr_t)&ret0 },
        { "glGetTexEnviv", (uintptr_t)&glGetTexEnviv },
        { "glGetTexEnvxv", (uintptr_t)&ret0 },
        { "glGetTexGenfvOES", (uintptr_t)&ret0 },
        { "glGetTexGenivOES", (uintptr_t)&ret0 },
        { "glGetTexGenxvOES", (uintptr_t)&ret0 },
        { "glGetTexParameterfv", (uintptr_t)&ret0 },
        { "glGetTexParameteriv", (uintptr_t)&ret0 },
        { "glGetTexParameterxv", (uintptr_t)&ret0 },
        { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
        { "glHint", (uintptr_t)&glHint_shim },
        { "glIsBuffer", (uintptr_t)&ret0 },
        { "glIsRenderbuffer", (uintptr_t)&glIsRenderbuffer },
        { "glIsEnabled", (uintptr_t)&glIsEnabled },
        { "glIsFramebufferOES", (uintptr_t)&glIsFramebuffer },
        { "glIsRenderbufferOES", (uintptr_t)&glIsRenderbuffer },
        { "glIsTexture", (uintptr_t)&glIsTexture },
        { "glLightf", (uintptr_t)&ret0 },
        { "glLightfv", (uintptr_t)&glLightfv },
        { "glLightModelf", (uintptr_t)&ret0 },
        { "glLightModelfv", (uintptr_t)&glLightModelfv },
        { "glLightModelx", (uintptr_t)&ret0 },
        { "glLightModelxv", (uintptr_t)&glLightModelxv },
        { "glLightx", (uintptr_t)&ret0 },
        { "glLightxv", (uintptr_t)&glLightxv },
        { "glLineWidth", (uintptr_t)&GLWRAP(glLineWidth) },
        { "glLineWidthx", (uintptr_t)&glLineWidthx },
        { "glLinkProgram", (uintptr_t)&glLinkProgram },
        { "glLoadIdentity", (uintptr_t)&GLWRAP(glLoadIdentity) },
        { "glLoadMatrixf", (uintptr_t)&GLWRAP(glLoadMatrixf) },
        { "glLoadMatrixx", (uintptr_t)&glLoadMatrixx },
        { "glLoadPaletteFromModelViewMatrixOES", (uintptr_t)&ret0 },
        { "glLogicOp", (uintptr_t)&ret0 },
        { "glMapBuffer", (uintptr_t)&glMapBuffer },
        { "glMapBufferOES", (uintptr_t)&glMapBuffer },
        { "glMaterialf", (uintptr_t)&glMaterialf },
        { "glMaterialfv", (uintptr_t)&glMaterialfv },
        { "glMaterialx", (uintptr_t)&glMaterialx },
        { "glMaterialxv", (uintptr_t)&glMaterialxv },
        { "glMatrixIndexPointerOES", (uintptr_t)&ret0 },
        { "glMatrixMode", (uintptr_t)&GLWRAP(glMatrixMode) },
        { "glMultiTexCoord4f", (uintptr_t)&ret0 },
        { "glMultiTexCoord4x", (uintptr_t)&ret0},
        { "glMultMatrixf", (uintptr_t)&GLWRAP(glMultMatrixf) },
        { "glMultMatrixx", (uintptr_t)&glMultMatrixx },
        { "glNormal3f", (uintptr_t)&glNormal3f },
        { "glNormal3x", (uintptr_t)&glNormal3x },
        { "glNormalPointer", (uintptr_t)&glNormalPointer },
        { "glOrthof", (uintptr_t)&glOrthof },
        { "glOrthox", (uintptr_t)&glOrthox },
        { "glPixelStorei", (uintptr_t)&glPixelStorei },
        { "glPointParameterf", (uintptr_t)&ret0 },
        { "glPointParameterfv", (uintptr_t)&ret0 },
        { "glPointParameterx", (uintptr_t)&ret0 },
        { "glPointParameterxv", (uintptr_t)&ret0 },
        { "glPointSize", (uintptr_t)&GLWRAP(glPointSize) },
        { "glPointSizePointerOES", (uintptr_t)&ret0 },
        { "glPointSizex", (uintptr_t)&glPointSizex },
        { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
        { "glPolygonOffsetx", (uintptr_t)&glPolygonOffsetx },
        { "glPopMatrix", (uintptr_t)&glPopMatrix },
        { "glPushMatrix", (uintptr_t)&glPushMatrix },
        { "glQueryMatrixxOES", (uintptr_t)&ret0 },
        { "glReadPixels", (uintptr_t)&glReadPixels },
        { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
        { "glRenderbufferStorageOES", (uintptr_t)&GLWRAP(glRenderbufferStorage) },
        { "glRotatef", (uintptr_t)&glRotatef },
        { "glRotatex", (uintptr_t)&glRotatex },
        { "glSampleCoverage", (uintptr_t)&ret0 },
        { "glSampleCoveragex", (uintptr_t)&ret0 },
        { "glScalef", (uintptr_t)&glScalef },
        { "glScalex", (uintptr_t)&glScalex },
        { "glScissor", (uintptr_t)&GLWRAP(glScissor) },
        { "glShadeModel", (uintptr_t)&glShadeModel },
        { "glShaderSource", (uintptr_t)&glShaderSource_soloader },
        { "glStencilFunc", (uintptr_t)&glStencilFunc },
        { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
        { "glStencilMask", (uintptr_t)&glStencilMask },
        { "glStencilOp", (uintptr_t)&glStencilOp },
        { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
        { "glTexCoordPointer", (uintptr_t)&GLWRAP(glTexCoordPointer) },
        { "glTexEnvf", (uintptr_t)&GLWRAP(glTexEnvf) },
        { "glTexEnvfv", (uintptr_t)&GLWRAP(glTexEnvfv) },
        { "glTexEnvi", (uintptr_t)&GLWRAP(glTexEnvi) },
        { "glTexEnviv", (uintptr_t)&ret0 },
        { "glTexEnvx", (uintptr_t)&glTexEnvx },
        { "glTexEnvxv", (uintptr_t)&glTexEnvxv },
        { "glTexGenfOES", (uintptr_t)&ret0 },
        { "glTexGenfvOES", (uintptr_t)&ret0 },
        { "glTexGeniOES", (uintptr_t)&ret0 },
        { "glTexGenivOES", (uintptr_t)&ret0 },
        { "glTexGenxOES", (uintptr_t)&ret0 },
        { "glTexGenxvOES", (uintptr_t)&ret0 },
        #ifdef DEBUG_SOLOADER
        { "glTexImage2D", (uintptr_t)&t_glTexImage2D },
#else
        { "glTexImage2D", (uintptr_t)&glTexImage2D_shim },
#endif
        { "glTexParameterf", (uintptr_t)&glTexParameterf_shim },
        { "glTexParameterfv", (uintptr_t)&ret0 },
        { "glTexParameteri", (uintptr_t)&glTexParameteri_shim },
        { "glTexParameteriv", (uintptr_t)&glTexParameteriv_shim },
        { "glTexParameterx", (uintptr_t)&glTexParameterx },
        { "glTexParameterxv", (uintptr_t)&ret0 },
        #ifdef DEBUG_SOLOADER
        { "glTexSubImage2D", (uintptr_t)&t_glTexSubImage2D },
#else
        { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D_shim },
#endif
        { "glTranslatef", (uintptr_t)&glTranslatef },
        { "glTranslatex", (uintptr_t)&glTranslatex },
        { "glUniform1f", (uintptr_t)&glUniform1f },
        { "glUniform1fv", (uintptr_t)&glUniform1fv },
        { "glUniform1i", (uintptr_t)&glUniform1i },
        { "glUniform1iv", (uintptr_t)&glUniform1iv },
        { "glUniform2i", (uintptr_t)&glUniform2i },
        { "glUniform2f", (uintptr_t)&glUniform2f },
        { "glUniform2fv", (uintptr_t)&glUniform2fv },
        { "glUniform2iv", (uintptr_t)&glUniform2iv },
        { "glUniform3i", (uintptr_t)&glUniform3i },
        { "glUniform3f", (uintptr_t)&glUniform3f },
        { "glUniform3fv", (uintptr_t)&glUniform3fv },
        { "glUniform3iv", (uintptr_t)&glUniform3iv },
        { "glUniform4i", (uintptr_t)&glUniform4i },
        { "glUniform4f", (uintptr_t)&glUniform4f },
        { "glUniform4fv", (uintptr_t)&glUniform4fv },
        { "glUniform4iv", (uintptr_t)&glUniform4iv },
        { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
        { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
        { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
        { "glUnmapBuffer", (uintptr_t)&glUnmapBuffer },
        { "glUnmapBufferOES", (uintptr_t)&glUnmapBuffer },
        { "glUseProgram", (uintptr_t)&glUseProgram },
        { "glValidateProgram", (uintptr_t)&ret0 },
        { "glVertexAttrib4f", (uintptr_t)&glVertexAttrib4f },
        { "glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv },
        { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
        { "glVertexPointer", (uintptr_t)&GLWRAP(glVertexPointer) },
        { "glViewport", (uintptr_t)&GLWRAP(glViewport) },
        { "glWeightPointerOES", (uintptr_t)&ret0 },


        // OpenSLES
        { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
        { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
        { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
        { "SL_IID_METADATAEXTRACTION", (uintptr_t)&SL_IID_METADATAEXTRACTION },
        { "SL_IID_PITCH", (uintptr_t)&SL_IID_PITCH },
        { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
        { "SL_IID_PREFETCHSTATUS", (uintptr_t)&SL_IID_PREFETCHSTATUS },
        { "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
        { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
        { "slCreateEngine", (uintptr_t)&slCreateEngine },


        // Pthread
        { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
        { "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
        { "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
        { "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
        { "pthread_attr_setschedparam", (uintptr_t) &ret0 },

        { "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
        { "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
        { "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
        { "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
        { "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
        { "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },

        { "pthread_create", (uintptr_t) &pthread_create_soloader },
        { "pthread_detach", (uintptr_t) &pthread_detach_soloader },
        { "pthread_equal", (uintptr_t) &pthread_equal_soloader },
        { "pthread_exit", (uintptr_t)&pthread_exit },
        { "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
        { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
        { "pthread_join", (uintptr_t) &pthread_join_soloader },
        { "pthread_key_create", (uintptr_t)&pthread_key_create },
        { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
        { "pthread_kill", (uintptr_t)&pthread_kill_soloader },

        { "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
        { "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
        { "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
        { "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader },
        { "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
        { "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader },
        { "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader },
        { "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader },
        { "pthread_mutexattr_setpshared", (uintptr_t) &ret0 },
        { "pthread_once", (uintptr_t)&pthread_once_soloader },

        { "pthread_self", (uintptr_t) &pthread_self_soloader },
        { "pthread_setname_np", (uintptr_t) &pthread_setname_np_soloader },
        { "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
        { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
        { "pthread_sigmask", (uintptr_t)&ret0 },

        { "sem_destroy", (uintptr_t) &sem_destroy_soloader },
        { "sem_getvalue", (uintptr_t) &sem_getvalue_soloader },
        { "sem_init", (uintptr_t) &sem_init_soloader },
        { "sem_post", (uintptr_t) &sem_post_soloader },
        { "sem_timedwait", (uintptr_t) &sem_timedwait_soloader },
        { "sem_trywait", (uintptr_t) &sem_trywait_soloader },
        { "sem_wait", (uintptr_t) &sem_wait_soloader },

        { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max },
        { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min },
        { "sched_yield", (uintptr_t)&sched_yield },


        // wchar, wctype
        { "btowc", (uintptr_t)&btowc },
        { "iswalpha", (uintptr_t)&iswalpha },
        { "iswcntrl", (uintptr_t)&iswcntrl },
        { "iswctype", (uintptr_t)&iswctype },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswlower", (uintptr_t)&iswlower },
        { "iswprint", (uintptr_t)&iswprint },
        { "iswpunct", (uintptr_t)&iswpunct },
        { "iswspace", (uintptr_t)&iswspace },
        { "iswupper", (uintptr_t)&iswupper },
        { "iswxdigit", (uintptr_t)&iswxdigit },
        { "mbrlen", (uintptr_t)&mbrlen },
        { "mbrtowc", (uintptr_t)&mbrtowc },
        { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
        { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
        { "mbstowcs", (uintptr_t)&mbstowcs },
        { "mbtowc", (uintptr_t)&mbtowc },
        { "towlower", (uintptr_t)&towlower },
        { "towupper", (uintptr_t)&towupper },
        { "wcrtomb", (uintptr_t)&wcrtomb },
        { "wcscasecmp", (uintptr_t)&wcscasecmp },
        { "wcscmp", (uintptr_t)&wcscmp },
        { "wcscoll", (uintptr_t)&wcscoll },
        { "wcscpy", (uintptr_t)&wcscpy },
        { "wcsftime", (uintptr_t)&wcsftime },
        { "wcslcat", (uintptr_t)&wcslcat },
        { "wcslcpy", (uintptr_t)&wcslcpy },
        { "wcslen", (uintptr_t)&wcslen },
        { "wcsncasecmp", (uintptr_t)&wcsncasecmp },
        { "wcsncmp", (uintptr_t)&wcsncmp },
        { "wcsncpy", (uintptr_t)&wcsncpy },
        { "wcsnlen", (uintptr_t)&wcsnlen },
        { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
        { "wcsstr", (uintptr_t)&wcsstr },
        { "wcstod", (uintptr_t)&wcstod },
        { "wcstof", (uintptr_t)&wcstof },
        { "wcstol", (uintptr_t)&wcstol },
        { "wcstoll", (uintptr_t)&wcstoll },
        { "wcstombs", (uintptr_t)&wcstombs },
        { "wcstoul", (uintptr_t)&wcstoul },
        { "wcstoull", (uintptr_t)&wcstoull },
        { "wcsxfrm", (uintptr_t)&wcsxfrm },
        { "wctob", (uintptr_t)&wctob },
        { "wctype", (uintptr_t)&wctype },
        { "wmemchr", (uintptr_t)&wmemchr },
        { "wmemcmp", (uintptr_t)&wmemcmp },
        { "wmemcpy", (uintptr_t)&wmemcpy },
        { "wmemmove", (uintptr_t)&wmemmove },
        { "wmemset", (uintptr_t)&wmemset },


        // libdl
        { "dlclose", (uintptr_t)&ret0 },
        { "dlerror", (uintptr_t)&ret0 },
        { "dlopen", (uintptr_t)&ret1 },
        { "dlsym", (uintptr_t)&dlsym_soloader },


        // Errno
        { "__errno", (uintptr_t)&__errno_soloader },
        { "strerror", (uintptr_t)&strerror_soloader },
        { "strerror_r", (uintptr_t)&strerror_r_soloader },
        { "perror", (uintptr_t)&perror }, // TODO: errno translation


        // Strings
        { "memchr", (uintptr_t)&memchr },
        { "memrchr", (uintptr_t)&memrchr },
        { "strcasecmp", (uintptr_t)&strcasecmp },
        { "strcat", (uintptr_t)&strcat },
        { "strchr", (uintptr_t)&strchr },
        { "strcmp", (uintptr_t)&strcmp },
        { "strcoll", (uintptr_t)&strcoll },
        { "strcpy", (uintptr_t)&strcpy },
        { "strcspn", (uintptr_t)&strcspn },
        { "strdup", (uintptr_t)&strdup },
        { "strlcat", (uintptr_t)&strlcat },
        { "strlcpy", (uintptr_t)&strlcpy },
        { "strlen", (uintptr_t)&strlen },
        { "strncasecmp", (uintptr_t)&strncasecmp },
        { "strncat", (uintptr_t)&strncat },
        { "strncmp", (uintptr_t)&strncmp },
        { "strncpy", (uintptr_t)&strncpy },
        { "strnlen", (uintptr_t)&strnlen },
        { "strpbrk", (uintptr_t)&strpbrk },
        { "strrchr", (uintptr_t)&strrchr },
        { "strspn", (uintptr_t)&strspn },
        { "strstr", (uintptr_t)&strstr },
        { "strtok", (uintptr_t)&strtok },
        { "strtok_r", (uintptr_t)&strtok_r },
        { "strxfrm", (uintptr_t)&strxfrm },


        // Syscalls
        { "fork", (uintptr_t)&fork },
        { "getpagesize", (uintptr_t)&getpagesize },
        { "getpid", (uintptr_t)&getpid },
        { "sbrk", (uintptr_t)&sbrk },
        { "syscall", (uintptr_t)&syscall },
        { "sysconf", (uintptr_t)&ret0 },
        { "system", (uintptr_t)&system },
        { "waitpid", (uintptr_t)&ret0 },


        // Time
        { "clock", (uintptr_t)&clock_soloader },
        { "clock_getres", (uintptr_t)&clock_getres_soloader },
        { "clock_gettime", (uintptr_t)&clock_gettime_soloader },
        { "difftime", (uintptr_t)&difftime },
        { "gettimeofday", (uintptr_t)&gettimeofday },
        { "gmtime", (uintptr_t)&gmtime },
        { "gmtime64", (uintptr_t)&gmtime64 },
        { "gmtime_r", (uintptr_t)&gmtime_r },
        { "localtime", (uintptr_t)&localtime },
        { "localtime64", (uintptr_t)&localtime64 },
        { "localtime_r", (uintptr_t)&localtime_r },
        { "mktime", (uintptr_t)&mktime },
        { "mktime64", (uintptr_t)&mktime64 },
        { "nanosleep", (uintptr_t)&nanosleep },
        { "strftime", (uintptr_t)&strftime },
        { "time", (uintptr_t)&time },
        { "tzset", (uintptr_t)&tzset },


        // Temp
        { "mkstemp", (uintptr_t)&mkstemp },
        { "mktemp", (uintptr_t)&mktemp },
        { "tmpfile", (uintptr_t)&tmpfile },
        { "tmpnam", (uintptr_t)&tmpnam },


        // stdlib
        { "abort", (uintptr_t)&abort_soloader },
        { "atof", (uintptr_t)&atof },
        { "atoi", (uintptr_t)&atoi },
        { "atol", (uintptr_t)&atol },
        { "atoll", (uintptr_t)&atoll },
        { "bsearch", (uintptr_t)&bsearch },
        { "exit", (uintptr_t)&exit_soloader },
        { "lrand48", (uintptr_t)&lrand48 },
        { "prctl", (uintptr_t)&ret0 },
        { "sleep", (uintptr_t)&sleep },
        { "srand48", (uintptr_t)&srand48 },
        { "strtod", (uintptr_t)&strtod },
        { "strtof", (uintptr_t)&strtof },
        { "strtoimax", (uintptr_t)&strtoimax },
        { "strtol", (uintptr_t)&strtol },
        { "strtold", (uintptr_t)&strtold },
        { "strtoll", (uintptr_t)&strtoll },
        { "strtoul", (uintptr_t)&strtoul },
        { "strtoull", (uintptr_t)&strtoull },
        { "strtoumax", (uintptr_t)&strtoumax },
        { "usleep", (uintptr_t)&usleep },

        #ifdef USE_SCELIBC_IO
            { "qsort", (uintptr_t)&sceLibcBridge_qsort },
            { "rand", (uintptr_t)&sceLibcBridge_rand },
            { "srand", (uintptr_t)&sceLibcBridge_srand },
        #else
            { "qsort", (uintptr_t)&qsort },
            { "rand", (uintptr_t)&rand },
            { "srand", (uintptr_t)&srand },
        #endif


        // Env
        { "getenv", (uintptr_t)&getenv_soloader },
        { "setenv", (uintptr_t)&setenv_soloader },


        // Jmp
        { "setjmp", (uintptr_t)&setjmp }, // TODO: May have different struct size?
        { "longjmp", (uintptr_t)&longjmp }, // TODO: May have different struct size?


        // Signals
        { "bsd_signal", (uintptr_t)&signal },
        { "raise", (uintptr_t)&raise },
        { "sigaction", (uintptr_t)&sigaction },


        // Locale
        { "freelocale", (uintptr_t)&freelocale },
        { "localeconv", (uintptr_t)&localeconv },
        { "newlocale", (uintptr_t)&newlocale },
        { "setlocale", (uintptr_t)&setlocale },
        { "uselocale", (uintptr_t)&uselocale },


        // zlib
        { "adler32", (uintptr_t)&adler32 },
        { "compress", (uintptr_t)&compress },
        { "compressBound", (uintptr_t)&compressBound },
        { "crc32", (uintptr_t)&crc32 },
        { "deflate", (uintptr_t)&deflate },
        { "deflateEnd", (uintptr_t)&deflateEnd },
        { "deflateInit2_", (uintptr_t)&deflateInit2_ },
        { "deflateInit_", (uintptr_t)&deflateInit_ },
        { "deflateReset", (uintptr_t)&deflateReset },
        { "gzclose", (uintptr_t)&gzclose },
        { "gzgets", (uintptr_t)&gzgets },
        { "gzopen", (uintptr_t)&gzopen },
        { "inflate", (uintptr_t)&inflate },
        { "inflateEnd", (uintptr_t)&inflateEnd },
        { "inflateInit2_", (uintptr_t)&inflateInit2_ },
        { "inflateInit_", (uintptr_t)&inflateInit_ },
        { "inflateReset", (uintptr_t)&inflateReset },
        { "inflateReset2", (uintptr_t)&inflateReset2 },
        { "uncompress", (uintptr_t)&uncompress },
};

void *dlsym_soloader(void * handle, const char * symbol) {
    for (int i = 0; i < sizeof(default_dynlib) / sizeof(default_dynlib[0]); i++) {
        if (strcmp(symbol, default_dynlib[i].symbol) == 0) {
            return &default_dynlib[i].func;
        }
    }

    l_error("dlsym: Unknown symbol \"%s\".", symbol);
    return NULL;
}

void resolve_imports(so_module* mod) {
    __sF_fake[0] = *stdin;
    __sF_fake[1] = *stdout;
    __sF_fake[2] = *stderr;

    extern void memfile_install(so_default_dynlib *tbl, int n);
    memfile_install(default_dynlib, sizeof(default_dynlib) / sizeof(default_dynlib[0]));
#ifdef PERF_LOG
    extern void perf_install(so_default_dynlib *tbl, int n);
    perf_install(default_dynlib, sizeof(default_dynlib) / sizeof(default_dynlib[0]));
#endif
    so_resolve(mod, default_dynlib, sizeof(default_dynlib), 0);
}