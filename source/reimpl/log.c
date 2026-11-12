/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include "reimpl/log.h"
#include "utils/logger.h"
#include <psp2/kernel/clib.h>
#include <stdlib.h>

// Also append the game's Android log to ux0:data/heman/game_log.txt so it can
// be read after running on real hardware (no console there).
#include <psp2/io/fcntl.h>
#include <string.h>
void game_log_file(const char *tag, const char *text) {
#ifndef DEBUG_SOLOADER
    // Release: no file I/O (an open/write/close per log line stalls frames)
    (void)tag; (void)text;
    return;
#endif
    static int lines = 0, truncated = 0;
    if (lines >= 4000)
        return;
    int flags = SCE_O_WRONLY | SCE_O_CREAT | (truncated ? SCE_O_APPEND : SCE_O_TRUNC);
    truncated = 1;
    SceUID fd = sceIoOpen(DATA_PATH "game_log.txt", flags, 0666);
    if (fd < 0)
        return;
    char line[1100];
    int n = snprintf(line, sizeof(line), "[%s] %s%s", tag ? tag : "?", text,
                     (text[0] && text[strlen(text) - 1] == '\n') ? "" : "\n");
    if (n > 0)
        sceIoWrite(fd, line, n > (int)sizeof(line) - 1 ? (int)sizeof(line) - 1 : n);
    sceIoClose(fd);
    lines++;
}

#define print_common \
    game_log_file(tag, text); \
    switch (prio) { \
        case ANDROID_LOG_INFO: \
            l_info("[ALOG][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_WARN: \
            l_warn("[ALOG][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_ERROR: \
        case ANDROID_LOG_FATAL: \
            l_error("[ALOG][%s] %s", tag, text); \
            break; \
        case ANDROID_LOG_UNKNOWN: \
        case ANDROID_LOG_DEFAULT: \
        case ANDROID_LOG_VERBOSE: \
        case ANDROID_LOG_DEBUG: \
        case ANDROID_LOG_SILENT: \
        default: \
            l_debug("[ALOG][%s] %s", tag, text); \
            break; \
    }

int __android_log_write(int prio, const char* tag, const char* text) {
    print_common
    return 0;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list list;
    char text[1024];

    va_start(list, fmt);
    vsnprintf(text, sizeof(text), fmt, list); // sceClibVsnprintf has no %f support
    va_end(list);

    print_common

    return 0;
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    char text[1024];

    vsnprintf(text, sizeof(text), fmt, ap);

    print_common

    return 0;
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    if (fmt) {
        va_list list;
        char text[1024];

        va_start(list, fmt);
        vsnprintf(text, sizeof(text), fmt, list); // sceClibVsnprintf has no %f support
        va_end(list);

        l_fatal("[ALOG][ASSERT] %s", text);
    } else {
        if (cond) {
            l_fatal("[ALOG][ASSERT] Assertion failed: %s", cond);
        } else {
            l_fatal("[ALOG][ASSERT] Unspecified assertion failed");
        }
    }

    abort();
}
