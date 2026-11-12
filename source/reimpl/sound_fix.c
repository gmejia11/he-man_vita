/*
 * sound_fix.c -- close a use-after-free race in the game's music streaming.
 *
 * OpenALStreamingThread::streamLoop(OpenALSound *) keeps raw pointers to the
 * sound and its OpenALStreamingInfo (sound+32) and refills buffers until the
 * source stops. Nothing stops the main thread from destroying that sound in
 * the meantime (e.g. when a level is unloaded): the thread then copies the
 * freed info's file name, std::string throws length_error, nobody catches it
 * and the game aborts (crash dump: streamLoop -> loadStreamingBuffer ->
 * getFileURL -> __cxa_throw -> std::terminate). Slow file I/O on the Vita
 * makes the window much wider than on Android.
 *
 * Hook OpenALSound's destructors: if the streaming thread is still using the
 * sound's source, stop the source (loadNextStreamingBufferForSound() returns 0
 * for a stopped source, ending the loop) and wait until the thread has removed
 * it from OpenALStreamingThread::streamingSources before tearing down.
 */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>
#include <stdint.h>

extern so_module so_mod;

typedef void (*info_dtor_t)(void *info);
typedef void (*op_delete_t)(void *p);
typedef void (*al_source_stop_t)(unsigned int source);

static info_dtor_t streaming_info_dtor;
static op_delete_t op_delete;
static al_source_stop_t al_source_stop;
static unsigned int **streaming_sources;   // std::vector<unsigned int>: begin, end, cap

#define WAIT_TIMEOUT_US (2 * 1000 * 1000)

static int source_streamed(unsigned int source) {
    unsigned int *b = streaming_sources[0], *e = streaming_sources[1];
    for (unsigned int *p = b; p && p < e; p++)
        if (*p == source) return 1;
    return 0;
}

static void *sound_dtor_hook(void *sound) {
    unsigned int source = *(unsigned int *)sound;   // OpenALSound::getSourceId()

    if (source && source_streamed(source)) {
        al_source_stop(source);
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        while (source_streamed(source) && sceKernelGetProcessTimeWide() - t0 < WAIT_TIMEOUT_US)
            sceKernelDelayThread(1000);
        if (source_streamed(source))
            sceClibPrintf("[sound_fix] streaming thread still owns source %u after 2 s\n", source);
    }

    // Original body of OpenALSound::~OpenALSound()
    uint32_t *s = (uint32_t *)sound;
    s[2] = s[1];
    streaming_info_dtor((uint8_t *)sound + 32);
    if (s[1]) op_delete((void *)s[1]);
    return sound;
}

void sound_fix_patch(void) {
    uintptr_t d1 = (uintptr_t)so_symbol(&so_mod, "_ZN11OpenALSoundD1Ev");
    uintptr_t d2 = (uintptr_t)so_symbol(&so_mod, "_ZN11OpenALSoundD2Ev");
    streaming_info_dtor = (info_dtor_t)so_symbol(&so_mod, "_ZN19OpenALStreamingInfoD1Ev");
    op_delete = (op_delete_t)so_symbol(&so_mod, "_ZdlPv");
    al_source_stop = (al_source_stop_t)so_symbol(&so_mod, "alSourceStop");
    streaming_sources = (unsigned int **)so_symbol(&so_mod, "_ZN21OpenALStreamingThread16streamingSourcesE");
    if (!d1 || !d2 || !streaming_info_dtor || !op_delete || !al_source_stop || !streaming_sources) {
        sceClibPrintf("[sound_fix] symbols missing, not installed\n");
        return;
    }
    hook_addr(d1, (uintptr_t)&sound_dtor_hook);
    hook_addr(d2, (uintptr_t)&sound_dtor_hook);
}
