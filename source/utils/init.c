/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <reimpl/controls.h>

#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000

extern so_module so_mod;

// so_util reserves a 64KB patch arena right below the load address
#define SO_PATCH_ARENA 0x10000

/**
 * Find a free address range big enough for the whole .so (plus so_util's patch
 * arena below it) by letting the kernel pick one, then releasing it again.
 * Returns 0 on failure.
 */
static uintptr_t find_free_load_address(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;

    Elf32_Ehdr ehdr;
    uintptr_t span = 0;
    if (sceIoRead(fd, &ehdr, sizeof(ehdr)) == sizeof(ehdr)) {
        for (int i = 0; i < ehdr.e_phnum; i++) {
            Elf32_Phdr phdr;
            sceIoLseek(fd, ehdr.e_phoff + i * sizeof(Elf32_Phdr), SCE_SEEK_SET);
            if (sceIoRead(fd, &phdr, sizeof(phdr)) != sizeof(phdr))
                break;
            if (phdr.p_type == PT_LOAD && phdr.p_vaddr + phdr.p_memsz > span)
                span = phdr.p_vaddr + phdr.p_memsz;
        }
    }
    sceIoClose(fd);
    if (span == 0)
        return 0;

    // Some slack for segment alignment, rounded to 1MB
    SceSize size = (SO_PATCH_ARENA + span + 0x10000 + 0xFFFFF) & ~0xFFFFF;
    SceUID blk = sceKernelAllocMemBlock("so_probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (blk < 0) {
        l_error("Could not find 0x%X bytes of free memory for the .so (0x%08X)", size, blk);
        return 0;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(blk, &base);
    sceKernelFreeMemBlock(blk);
    return (uintptr_t)base + SO_PATCH_ARENA;
}

void game_log_file(const char *tag, const char *text);

void soloader_init_all() {
    // First line of ux0:data/heman/game_log.txt: which build is running
    game_log_file("loader", "He-Man loader build " __DATE__ " " __TIME__);
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    int ret = so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS);
    if (ret < 0) {
        l_warn("Could not load SO at 0x%08X (0x%08X), looking for a free address...", LOAD_ADDRESS, ret);
        uintptr_t addr = find_free_load_address(SO_PATH);
        if (addr)
            ret = so_file_load(&so_mod, SO_PATH, addr);
    }
    if (ret < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s (0x%08X).", SO_PATH, ret);
    }
    l_success("SO loaded at 0x%08X.", so_mod.load_addr);

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

#ifndef NDK_PORT
    controls_init();
    l_success("Controls initialized.");
#endif
}
