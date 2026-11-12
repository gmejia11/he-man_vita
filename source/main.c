#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/dialog.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <kubridge.h>
#include <stdio.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#ifndef NDK_PORT
#include "reimpl/controls.h"
#else
#include <falso_ndk/FalsoNDK.h>
#endif

int _newlib_heap_size_user = 192 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

#ifdef HEAP_WATCH
void heapwatch_begin(void);
void heapwatch_after_gl_init(void);
#endif


int main() {
    soloader_init_all();

    // Pure NDK games often don't export JNI_OnLoad
    int (*JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    if (JNI_OnLoad)
        JNI_OnLoad(&jvm);

#ifdef HEAP_WATCH
    heapwatch_begin();
#endif
    gl_init();
#ifdef HEAP_WATCH
    heapwatch_after_gl_init();
#endif



#ifndef NDK_PORT
    // ... do some initialization

    while (1) {
        // ... render call
        gl_swap();
    }
#else
    // Build a fake ANativeActivity that the game's onCreate will receive
    ANativeActivity *activity = calloc(1, sizeof(ANativeActivity));
    activity->callbacks = calloc(1, sizeof(ANativeActivityCallbacks));
    activity->env = &jni; // from FalsoJNI
    activity->vm = &jvm;  // from FalsoJNI
    activity->clazz = (jclass)0x42424242;
    activity->internalDataPath = DATA_PATH "assets/";
    activity->externalDataPath = DATA_PATH "assets/";
    activity->sdkVersion = 14;
    activity->instance = NULL;

    // Drive the activity lifecycle
    int (*ANativeActivity_onCreate)(ANativeActivity *, void *, size_t) =
        (void *)so_symbol(&so_mod, "ANativeActivity_onCreate");
    if (!ANativeActivity_onCreate)
        fatal_error("Error: ANativeActivity_onCreate not found in %s.", SO_PATH);
    ANativeActivity_onCreate(activity, NULL, 0);

    // The game only fills the callbacks it cares about
    ANativeActivityCallbacks *cb = activity->callbacks;
    if (cb->onStart) cb->onStart(activity);
    if (cb->onResume) cb->onResume(activity);

    // Wire up input and the native window
    AInputQueue *aInputQueue = AInputQueue_create();
    if (cb->onInputQueueCreated) cb->onInputQueueCreated(activity, aInputQueue);

    ANativeWindow *aNativeWindow = ANativeWindow_create();
    if (cb->onNativeWindowCreated) cb->onNativeWindowCreated(activity, aNativeWindow);

    if (cb->onWindowFocusChanged) cb->onWindowFocusChanged(activity, 1);
#endif

    sceKernelExitDeleteThread(0);
}

#ifndef NDK_PORT
void controls_handler_key(int32_t keycode, ControlsAction action) {
    // Call into the .so here
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    // Call into the .so here
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    // Call into the .so here
}
#endif
