/*
 * Guard for sceGxmSetUniformDataF (linked with -Wl,--wrap).
 *
 * On Vita3K some vitaGL draws end up calling it with a NULL uniform buffer or
 * parameter, and the emulator then crashes. Log who called it (return address,
 * resolvable with addr2line against build/so_loader) and skip the bad call.
 */
#include <psp2/gxm.h>
#include <psp2/kernel/clib.h>
#include <stdint.h>

int __real_sceGxmSetUniformDataF(void *uniformBuffer, const SceGxmProgramParameter *parameter,
                                 unsigned int componentOffset, unsigned int componentCount,
                                 const float *sourceData);

int __wrap_sceGxmSetUniformDataF(void *uniformBuffer, const SceGxmProgramParameter *parameter,
                                 unsigned int componentOffset, unsigned int componentCount,
                                 const float *sourceData) {
    if (!uniformBuffer || !parameter || !sourceData) {
        static uintptr_t logged[32];
        static int n_logged = 0;
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        int seen = 0;
        for (int i = 0; i < n_logged; i++)
            if (logged[i] == caller) { seen = 1; break; }
        if (!seen && n_logged < 32) {
            logged[n_logged++] = caller;
            sceClibPrintf("[gxm_guard] sceGxmSetUniformDataF(buf=%p, param=%p, data=%p) from 0x%08X\n",
                          uniformBuffer, parameter, sourceData, caller);
        }
        return 0x805B0004; // SCE_GXM_ERROR_INVALID_POINTER, same as the real call
    }
    return __real_sceGxmSetUniformDataF(uniformBuffer, parameter, componentOffset, componentCount, sourceData);
}

/*
 * Scene tracking (also via -Wl,--wrap).
 *
 * vitaGL's scene_reset() ignores sceGxmBeginScene failures and then reserves
 * default uniform buffers for its clear/scissor draws. Reserving outside a
 * scene makes SceGxm jump to a NULL callback on real hardware (He-Man crash:
 * glClear -> scene_reset -> update_scissor_test). Refuse reservations outside
 * a scene and log what happened to ux0:data/heman/gxm_log.txt.
 */
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <stdarg.h>

static volatile int gxm_in_scene = 0;
static int gxm_log_lines = 0;
static unsigned gxm_begins = 0, gxm_ends = 0, gxm_draws = 0, gxm_draw_errors = 0;

static void gxm_file_log(const char *fmt, ...) {
    if (gxm_log_lines >= 200)
        return;
    gxm_log_lines++;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
    sceClibPrintf("%s", line);
    SceUID fd = sceIoOpen(DATA_PATH "gxm_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
}

int __real_sceGxmBeginScene(SceGxmContext *context, unsigned int flags, const SceGxmRenderTarget *renderTarget,
                            const SceGxmValidRegion *validRegion, SceGxmSyncObject *vertexSyncObject,
                            SceGxmSyncObject *fragmentSyncObject, const SceGxmColorSurface *colorSurface,
                            const SceGxmDepthStencilSurface *depthStencil);
int __real_sceGxmEndScene(SceGxmContext *context, const SceGxmNotification *vertexNotification,
                          const SceGxmNotification *fragmentNotification);
int __real_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *context, void **uniformBuffer);
int __real_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext *context, void **uniformBuffer);

int __wrap_sceGxmBeginScene(SceGxmContext *context, unsigned int flags, const SceGxmRenderTarget *renderTarget,
                            const SceGxmValidRegion *validRegion, SceGxmSyncObject *vertexSyncObject,
                            SceGxmSyncObject *fragmentSyncObject, const SceGxmColorSurface *colorSurface,
                            const SceGxmDepthStencilSurface *depthStencil) {
    int ret = __real_sceGxmBeginScene(context, flags, renderTarget, validRegion, vertexSyncObject,
                                      fragmentSyncObject, colorSurface, depthStencil);
    if (gxm_in_scene)
        gxm_file_log("[gxm] BeginScene while a scene is already open (begins=%u ends=%u) caller=0x%08X\n",
                     gxm_begins, gxm_ends, (unsigned)(uintptr_t)__builtin_return_address(0));
    if (ret == 0) {
        gxm_in_scene = 1;
        gxm_begins++;
        // Only the first scenes: the old every-120-scenes line opened and
        // appended to gxm_log.txt on the render thread, a ~15 ms hitch every 2 s
        if (gxm_begins <= 3)
            gxm_file_log("[gxm] scenes begun=%u ended=%u draws=%u draw_errors=%u (rt=%p)\n",
                         gxm_begins, gxm_ends, gxm_draws, gxm_draw_errors, renderTarget);
    } else
        gxm_file_log("[gxm] sceGxmBeginScene failed: 0x%08X (in_scene=%d, rt=%p, color=%p, depth=%p) caller=0x%08X\n",
                     ret, gxm_in_scene, renderTarget, colorSurface, depthStencil,
                     (unsigned)(uintptr_t)__builtin_return_address(0));
    return ret;
}

int __wrap_sceGxmEndScene(SceGxmContext *context, const SceGxmNotification *vertexNotification,
                          const SceGxmNotification *fragmentNotification) {
    int ret = __real_sceGxmEndScene(context, vertexNotification, fragmentNotification);
    if (ret != 0)
        gxm_file_log("[gxm] sceGxmEndScene failed: 0x%08X (in_scene=%d) caller=0x%08X\n",
                     ret, gxm_in_scene, (unsigned)(uintptr_t)__builtin_return_address(0));
    gxm_in_scene = 0;
    gxm_ends++;
    return ret;
}

int __real_sceGxmDraw(SceGxmContext *context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType,
                      const void *indexData, unsigned int indexCount);
int __wrap_sceGxmDraw(SceGxmContext *context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType,
                      const void *indexData, unsigned int indexCount) {
    gxm_draws++;
    if (!gxm_in_scene)
        gxm_file_log("[gxm] sceGxmDraw outside a scene (count=%u) caller=0x%08X\n",
                     indexCount, (unsigned)(uintptr_t)__builtin_return_address(0));
    int ret = __real_sceGxmDraw(context, primType, indexType, indexData, indexCount);
    if (ret != 0) {
        gxm_draw_errors++;
        if (gxm_draw_errors <= 20)
            gxm_file_log("[gxm] sceGxmDraw failed: 0x%08X (prim=%d count=%u) caller=0x%08X\n",
                         ret, primType, indexCount, (unsigned)(uintptr_t)__builtin_return_address(0));
    }
    return ret;
}

int __real_sceGxmDisplayQueueAddEntry(SceGxmSyncObject *oldBuffer, SceGxmSyncObject *newBuffer, const void *callbackData);
int __wrap_sceGxmDisplayQueueAddEntry(SceGxmSyncObject *oldBuffer, SceGxmSyncObject *newBuffer, const void *callbackData) {
    int ret = __real_sceGxmDisplayQueueAddEntry(oldBuffer, newBuffer, callbackData);
    if (ret != 0)
        gxm_file_log("[gxm] sceGxmDisplayQueueAddEntry failed: 0x%08X\n", ret);
    return ret;
}

#define SCE_GXM_ERROR_NOT_WITHIN_SCENE_ 0x805B0007

int __wrap_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *context, void **uniformBuffer) {
    if (!gxm_in_scene) {
        gxm_file_log("[gxm] ReserveVertexDefaultUniformBuffer outside a scene, refused. caller=0x%08X\n",
                     (unsigned)(uintptr_t)__builtin_return_address(0));
        if (uniformBuffer) *uniformBuffer = NULL;
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE_;
    }
    return __real_sceGxmReserveVertexDefaultUniformBuffer(context, uniformBuffer);
}

int __wrap_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext *context, void **uniformBuffer) {
    if (!gxm_in_scene) {
        gxm_file_log("[gxm] ReserveFragmentDefaultUniformBuffer outside a scene, refused. caller=0x%08X\n",
                     (unsigned)(uintptr_t)__builtin_return_address(0));
        if (uniformBuffer) *uniformBuffer = NULL;
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE_;
    }
    return __real_sceGxmReserveFragmentDefaultUniformBuffer(context, uniformBuffer);
}
