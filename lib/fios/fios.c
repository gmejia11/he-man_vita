/*
 * fios.c -- use FIOS2 for optimized I/O
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <string.h>

#include "fios.h"

#define MAX_PATH_LENGTH 256
#define RAMCACHEBLOCKSIZE (128 * 1024)
#define RAMCACHEBLOCKNUM 64

static int64_t g_OpStorage[SCE_FIOS_OP_STORAGE_SIZE(64, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_ChunkStorage[SCE_FIOS_CHUNK_STORAGE_SIZE(1024) / sizeof(int64_t) + 1];
static int64_t g_FHStorage[SCE_FIOS_FH_STORAGE_SIZE(1024, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_DHStorage[SCE_FIOS_DH_STORAGE_SIZE(32, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];

static SceFiosRamCacheContext g_RamCacheContext = SCE_FIOS_RAM_CACHE_CONTEXT_INITIALIZER;
static char *g_RamCacheWorkBuffer;

// Guard area after the RAM cache work buffer. FIOS appears to write past
// blocks*blockSize (He-Man: the heap block right after it was zeroed once
// ~8 MB had been read, corrupting vitaGL/SceGxm objects). Keep slack behind
// the buffer and expose it so the loader can check it.
#define RAMCACHE_GUARD_SIZE (256 * 1024)
#define RAMCACHE_GUARD_BYTE 0x5A
char *fios_ramcache_buffer = NULL;
unsigned fios_ramcache_size = 0;
unsigned fios_ramcache_guard_size = RAMCACHE_GUARD_SIZE;

int fios_init(const char * path) {
    int res;

    SceFiosParams params = SCE_FIOS_PARAMS_INITIALIZER;
    params.opStorage.pPtr = g_OpStorage;
    params.opStorage.length = sizeof(g_OpStorage);
    params.chunkStorage.pPtr = g_ChunkStorage;
    params.chunkStorage.length = sizeof(g_ChunkStorage);
    params.fhStorage.pPtr = g_FHStorage;
    params.fhStorage.length = sizeof(g_FHStorage);
    params.dhStorage.pPtr = g_DHStorage;
    params.dhStorage.length = sizeof(g_DHStorage);
    params.pathMax = MAX_PATH_LENGTH;

    params.threadAffinity[SCE_FIOS_IO_THREAD] = 0x20000;
    params.threadAffinity[SCE_FIOS_CALLBACK_THREAD] = 0;
    params.threadAffinity[SCE_FIOS_DECOMPRESSOR_THREAD] = 0;

    params.threadPriority[SCE_FIOS_IO_THREAD] = 64;
    params.threadPriority[SCE_FIOS_CALLBACK_THREAD] = 191;
    params.threadPriority[SCE_FIOS_DECOMPRESSOR_THREAD] = 191;

    res = sceFiosInitialize(&params);
    if (res < 0)
        return res;

    g_RamCacheWorkBuffer = memalign(8, RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE + RAMCACHE_GUARD_SIZE);
    if (!g_RamCacheWorkBuffer)
        return -1;
    memset(g_RamCacheWorkBuffer + RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE, RAMCACHE_GUARD_BYTE, RAMCACHE_GUARD_SIZE);
    fios_ramcache_buffer = g_RamCacheWorkBuffer;
    fios_ramcache_size = RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE;

    g_RamCacheContext.pPath = path;
    g_RamCacheContext.pWorkBuffer = g_RamCacheWorkBuffer;
    g_RamCacheContext.workBufferSize = RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE;
    g_RamCacheContext.blockSize = RAMCACHEBLOCKSIZE;
    res = sceFiosIOFilterAdd(0, sceFiosIOFilterCache, &g_RamCacheContext);
    if (res < 0)
        return res;

    return 0;
}

void fios_terminate(void) {
    sceFiosTerminate();
    free(g_RamCacheWorkBuffer);
}
