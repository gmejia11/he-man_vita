/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#ifdef __cplusplus
extern "C"
{
#endif
	extern so_module so_mod;
#ifdef __cplusplus
};
#endif

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/sys.h"
#include <stdbool.h>

void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

void kuser_patch(void) {
	// Android's kuser helpers live at fixed addresses (0xFFFF0FA0 / 0xFFFF0FC0)
	// that don't exist on Vita. Allocate an executable page anywhere and redirect
	// the .so's references to equivalent offsets inside it.
	// Pass a valid (zeroed) opt: Vita3K's kubridge HLE dereferences it.
	// attr = 0 lets the kernel choose the address.
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	SceUID blk = kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt);
	if (blk < 0)
		fatal_error("Error could not allocate atomic block (0x%08X).", blk);

	void *base_ptr = NULL;
	sceKernelGetMemBlockBase(blk, &base_ptr);
	uintptr_t base = (uintptr_t)base_ptr;
	kuKernelMemProtect(base_ptr, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	const uint32_t barrier_addr = base + 0xFA0;
	const uint32_t cmpxchg_addr = base + 0xFC0;
	hook_addr(barrier_addr, (uintptr_t)__kuser_memory_barrier);
	hook_addr(cmpxchg_addr, (uintptr_t)__atomic_cmpxchg);

	for (uint32_t addr = so_mod.text_base; addr < so_mod.text_base + so_mod.text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching 0x%x -> __kuser_cmpxchg", a);
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &cmpxchg_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching 0x%x -> __kuser_memory_barrier", a);
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &barrier_addr, sizeof(uint32_t));
		}
	}
}

// The game picks its asset bundle in DeviceInfo::initAssetBundleInfo(480, 720, 960)
// by screen width: >= 960 selects screen-xlarge at 2x (a 480x272 virtual screen,
// too short for a UI laid out for >= 320 px), 720..959 selects screen-large at
// 1.5x (640x362). Raise the xlarge threshold to 961 so the Vita's 960 px width
// gets screen-large: patch "movw r3, #0" (low half of 960.0f) to "movw r3, #0x4000"
// so the third argument becomes 961.0f.
static void asset_bundle_patch(void) {
	// Both callers: the startup path and GSAppDelegate::onCreate
	const uintptr_t sites[] = { 0x441302, 0x6a82fc };
	const uint8_t expected[4] = { 0x40, 0xf2, 0x00, 0x03 }; // movw r3, #0
	const uint8_t patched[4]  = { 0x44, 0xf2, 0x00, 0x03 }; // movw r3, #0x4000
	for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
		uintptr_t addr = so_mod.text_base + sites[i];
		if (memcmp((void *)addr, expected, sizeof(expected)) == 0) {
			kuKernelCpuUnrestrictedMemcpy((void *)addr, patched, sizeof(patched));
			l_success("Asset bundle patch applied at 0x%x (screen-large).", sites[i]);
		} else {
			l_warn("Asset bundle patch skipped at 0x%x: unexpected code.", sites[i]);
		}
	}
}

// The engine's global scratch buffers helo::StringBuffer(2048) and
// helo::WStringBuffer(2048) are created in a static initializer (so+0x434610).
// Their append*() methods don't bounds-check, and around the intro the game
// writes ~14.7 KB into the 8 KB WStringBuffer, trampling the game's static
// std::strings, FalsoJNI's tables and vitaGL/SceGxm objects that follow it in
// the heap (on Android it presumably lands in unused memory). Allocate both
// with 8192 entries instead: patch "mov.w r1, #2048" -> "mov.w r1, #8192".
static void scratch_buffer_patch(void) {
	const uintptr_t sites[] = { 0x434616, 0x434636 };
	const uint8_t expected[4] = { 0x4f, 0xf4, 0x00, 0x61 }; // mov.w r1, #2048
	const uint8_t patched[4]  = { 0x4f, 0xf4, 0x00, 0x51 }; // mov.w r1, #8192
	for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
		uintptr_t addr = so_mod.text_base + sites[i];
		if (memcmp((void *)addr, expected, sizeof(expected)) == 0) {
			kuKernelCpuUnrestrictedMemcpy((void *)addr, patched, sizeof(patched));
			l_success("Scratch buffer patch applied at 0x%x (8192 entries).", sites[i]);
		} else {
			l_warn("Scratch buffer patch skipped at 0x%x: unexpected code.", sites[i]);
		}
	}
}

// Diagnostic replacement for helo::widget::UISystem::getWidgetWithName(const Handle&)
// (so+0x4bf550). Same linear search over the UISystem's widget vector
// (begin/end at +52/+56, widget name id at widget+88, handle id at handle+4);
// on a miss, log the wanted name and every widget name present.
void game_log_file(const char *tag, const char *text);
typedef const char *(*handle_get_cstring_t)(int *handle);

static void *getWidgetWithName_hook(void *uisys, const int *handle) {
	void **begin = *(void ***)((uint8_t *)uisys + 52);
	void **end = *(void ***)((uint8_t *)uisys + 56);
	int id = handle[1];
	for (void **w = begin; w < end; w++)
		if (*w && *(int *)((uint8_t *)*w + 88) == id)
			return *w;

	static int misses = 0;
	if (misses++ < 8) {
		handle_get_cstring_t get_cstring = (handle_get_cstring_t)(so_mod.text_base + 0x435121);
		int wanted[2] = { 0, id };
		char msg[512];
		snprintf(msg, sizeof(msg), "getWidgetWithName('%s' id=%d) NOT FOUND in UISystem %p with %d widgets:",
		         get_cstring(wanted), id, uisys, (int)(end - begin));
		game_log_file("ui", msg);
		int n = 0;
		for (void **w = begin; w < end && n < 64; w++, n++) {
			int wid = *w ? *(int *)((uint8_t *)*w + 88) : -1;
			int h[2] = { 0, wid };
			snprintf(msg, sizeof(msg), "  widget[%d] %p id=%d name='%s'", n, *w, wid,
			         wid >= 0 ? get_cstring(h) : "?");
			game_log_file("ui", msg);
		}
	}
	return NULL;
}

// Frame pacing. android_main runs a tick when
//   (gettimeofday_ms() - last_ms) > app->frame_interval_ms   (app+40)
// and HeloApp::setTimer(60) sets frame_interval_ms = 1000/60 = 16.667. With a
// whole-millisecond clock that is only true after 17 ms, so the game ran at a
// fixed 58.8 fps that never lines up with the 60 Hz display (a dropped/doubled
// frame about every second). Use floor(1000/fps) - 0.5 (15.5 ms: ticks every
// 16 ms) and let vitaGL's vsync hold it at exactly 60. app+44 (the interval in
// seconds) is left at the real 1/fps.
static void HeloApp_setTimer_hook(void *app, int fps) {
	float interval = 1000.0f / (float)(fps > 0 ? fps : 60);
	*(float *)((uint8_t *)app + 44) = interval * 0.001f;
	if (interval > 2.0f)
		interval = (float)(int)interval - 0.5f;
	*(float *)((uint8_t *)app + 40) = interval;
}

static void frame_pacing_patch(void) {
	uintptr_t set_timer = (uintptr_t)so_symbol(&so_mod, "_ZN7HeloApp8setTimerEi");
	if (set_timer)
		hook_addr(set_timer, (uintptr_t)&HeloApp_setTimer_hook);
	else
		l_warn("HeloApp::setTimer not found, frame pacing patch skipped.");
}

void touch_buttons_patch(void);
void sound_fix_patch(void);
void memfile_patch_helo(void);

void so_patch(void) {
	kuser_patch();
	touch_buttons_patch();
	sound_fix_patch();
	memfile_patch_helo();
	frame_pacing_patch();
	hook_addr(so_mod.text_base + 0x4bf550 + 1, (uintptr_t)&getWidgetWithName_hook);
	scratch_buffer_patch();
	asset_bundle_patch();
	// Sample hook with symbol name
	// hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN6glitch2os7Printer5printEPKcz"), (uintptr_t)&hookedFunction);
	// Or with offset
	// hook_addr((uintptr_t)so_mod.text_base + 0xdeadbabe, (uintptr_t)&hookedFunction);
	// If you use SO_CONTINUE, define a so_hook before the function and assign to it
	// function_hook = hook_addr(...);
}