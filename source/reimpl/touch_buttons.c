/*
 * touch_buttons.c -- map PS Vita buttons/sticks to He-Man's on-screen touch
 * controls by injecting virtual fingers into FalsoNDK's touch sampling.
 *
 * The game only understands touches (its HemanGamePad: a drag-to-move "dpad"
 * zone on the left half, taps/swipes on the touchPad, and HUD buttons), so a
 * pressed button becomes a finger at a configurable screen position.
 *
 * Bindings live in ux0:data/heman/controls.txt (created with defaults when
 * missing). Coordinates are screen pixels (960x544).
 *
 *   <BUTTON> tap   x y            finger held at (x,y) while the button is held
 *   <BUTTON> swipe x1 y1 x2 y2    quick swipe from (x1,y1) to (x2,y2) per press
 *   STICK    stick cx cy radius   left analog drags a finger around (cx,cy)
 *   DPAD     stick cx cy radius   d-pad drags the same way
 *   <BUTTON> ihtp                 "I Have The Power" (calls the game directly)
 *
 * BUTTON: CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 START SELECT
 */
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { B_TAP, B_SWIPE, B_STICK, B_IHTP };

typedef struct {
    uint32_t mask;      // SCE_CTRL_* (0 for STICK)
    int is_dpad;        // stick binding driven by the d-pad
    int type;
    float x1, y1, x2, y2, r;
    // runtime
    int active, frame;
    float cur_x, cur_y;
    int prev_pressed;
} Binding;

#define MAX_BINDINGS 16
static Binding binds[MAX_BINDINGS];
static int n_binds = 0;
static int loaded = 0;

static const char *DEFAULT_CONTROLS =
    "# He-Man PS Vita controls -> on-screen touch (screen is 960x544)\n"
    "# <BUTTON> tap x y | <BUTTON> swipe x1 y1 x2 y2 | STICK/DPAD stick cx cy radius\n"
    "# <BUTTON> ihtp = I Have The Power\n"
    "# Buttons: CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 START SELECT\n"
    "STICK    stick 180 380 110\n"
    "DPAD     stick 180 380 110\n"
    "SQUARE   tap   720 360\n"
    "CROSS    swipe 720 420 720 220\n"
    "CIRCLE   swipe 640 360 900 360\n"
    "TRIANGLE tap   910 50\n"
    "R1       ihtp\n"
    "L1       swipe 900 360 640 360\n"
    "START    tap   45 45\n";

static uint32_t button_mask(const char *name) {
    static const struct { const char *n; uint32_t m; } t[] = {
        { "CROSS", SCE_CTRL_CROSS }, { "CIRCLE", SCE_CTRL_CIRCLE },
        { "SQUARE", SCE_CTRL_SQUARE }, { "TRIANGLE", SCE_CTRL_TRIANGLE },
        { "L1", SCE_CTRL_L1 }, { "R1", SCE_CTRL_R1 },
        { "L2", SCE_CTRL_L2 }, { "R2", SCE_CTRL_R2 },
        { "START", SCE_CTRL_START }, { "SELECT", SCE_CTRL_SELECT },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (strcmp(name, t[i].n) == 0) return t[i].m;
    return 0;
}

static void parse_line(char *line) {
    char name[16], type[16];
    float a = 0, b = 0, c = 0, d = 0;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    int n = sscanf(line, "%15s %15s %f %f %f %f", name, type, &a, &b, &c, &d);
    if (n < 2 || n_binds >= MAX_BINDINGS) return;
    Binding *bd = &binds[n_binds];
    memset(bd, 0, sizeof(*bd));
    if (strcmp(type, "ihtp") == 0) {
        bd->type = B_IHTP;
    } else if (n < 4) {
        return;
    } else if (strcmp(type, "tap") == 0) {
        bd->type = B_TAP; bd->x1 = a; bd->y1 = b;
    } else if (strcmp(type, "swipe") == 0 && n >= 6) {
        bd->type = B_SWIPE; bd->x1 = a; bd->y1 = b; bd->x2 = c; bd->y2 = d;
    } else if (strcmp(type, "stick") == 0 && n >= 5) {
        bd->type = B_STICK; bd->x1 = a; bd->y1 = b; bd->r = c;
    } else {
        return;
    }
    if (strcmp(name, "STICK") == 0) {
        if (bd->type != B_STICK) return;
    } else if (strcmp(name, "DPAD") == 0) {
        if (bd->type != B_STICK) return;
        bd->is_dpad = 1;
    } else {
        bd->mask = button_mask(name);
        if (!bd->mask || bd->type == B_STICK) return;
    }
    n_binds++;
}

static void load_controls(void) {
    loaded = 1;
    char buf[2048];
    int len = -1;
    SceUID fd = sceIoOpen(DATA_PATH "controls.txt", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        len = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
    }
    if (len <= 0) {
        len = strlen(DEFAULT_CONTROLS);
        memcpy(buf, DEFAULT_CONTROLS, len);
        fd = sceIoOpen(DATA_PATH "controls.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fd >= 0) { sceIoWrite(fd, DEFAULT_CONTROLS, len); sceIoClose(fd); }
    }
    buf[len] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        parse_line(line);

    // controls.txt files written by older builds have no ihtp binding (R1 was
    // a tap that missed the button): give R1 "I Have The Power" instead
    int has_ihtp = 0;
    for (int i = 0; i < n_binds; i++)
        if (binds[i].type == B_IHTP) has_ihtp = 1;
    if (!has_ihtp) {
        int w = 0;
        for (int i = 0; i < n_binds; i++)
            if (binds[i].mask != SCE_CTRL_R1) binds[w++] = binds[i];
        n_binds = w;
        if (n_binds < MAX_BINDINGS) {
            memset(&binds[n_binds], 0, sizeof(binds[0]));
            binds[n_binds].type = B_IHTP;
            binds[n_binds].mask = SCE_CTRL_R1;
            n_binds++;
        }
    }
    sceClibPrintf("[controls] %d bindings loaded\n", n_binds);
}

// Press point reported with direct movement: the STICK binding's centre
static float binds_stick_cx(void) {
    for (int i = 0; i < n_binds; i++)
        if (binds[i].type == B_STICK && !binds[i].is_dpad) return binds[i].x1;
    return 180;
}

static float binds_stick_cy(void) {
    for (int i = 0; i < n_binds; i++)
        if (binds[i].type == B_STICK && !binds[i].is_dpad) return binds[i].y1;
    return 380;
}

static void add_report(SceTouchData *t, int id, float x, float y) {
    if (t->reportNum >= SCE_TOUCH_MAX_REPORT) return;
    if (x < 0) x = 0; if (x > 959) x = 959;
    if (y < 0) y = 0; if (y > 543) y = 543;
    SceTouchReport *r = &t->report[t->reportNum++];
    memset(r, 0, sizeof(*r));
    r->id = (SceUInt8)id;      // ids 200+ never collide with real fingers (0..127)
    r->force = 128;
    r->x = (SceInt16)(x * 2.0f);   // panel space is 1920x1088
    r->y = (SceInt16)(y * 2.0f);
}

/*
 * Direct movement input.
 *
 * Faking a drag on the on-screen WDirectionPad is fragile: GameInputManager
 * only routes a finger to the pad if its DOWN lands on the pad widget (closest
 * widget centre under the point, in UI space that depends on the asset bundle
 * scale), and it tracks at most 3 fingers by pointer id. So during gameplay we
 * skip the widget and feed the stick straight into GameInputData's movement
 * slot, exactly like MovementWDirectionPadHandler does:
 *
 *   InputData (72 bytes, GameInputData::getInput(0) = movement)
 *     +0  Point2 direction  (cursor - centre) / radius, y down, |v| <= 1
 *     +8  Point2 position   press point
 *     +20 int    state      0 idle, 1 pressed, 2 double tap, 3 held, 4 released
 *   InputData::tick() turns 1/2 into 3 and 4 into 0.
 *
 * GameInputManager::tick(float) is just getGameInputData()->tick(dt); the hook
 * writes the stick first and then does the same. The game is softfp, so dt
 * arrives as raw bits in r1 and is passed through untouched.
 */
extern so_module so_mod;

typedef void (*gid_tick_t)(void *gid, uint32_t dt_bits);
typedef void *(*gid_get_input_t)(void *gid, int which);
typedef int (*gim_locked_t)(void *gim);

static gid_tick_t gid_tick;
static gid_get_input_t gid_get_input;
static gim_locked_t gim_is_input_locked;

#define DIRECT_MOVE_DEADZONE 0.25f

// Set by the game thread while gameplay ticks; read by the touch thread to
// stop the virtual stick/d-pad fingers from fighting the direct input
static volatile SceUInt32 direct_move_last_tick = 0;
static int direct_move_active = 0;
static float direct_move_last_x = 0, direct_move_last_y = 0;

static int direct_move_live(void) {
    SceUInt32 last = direct_move_last_tick;
    return last != 0 && (sceKernelGetProcessTimeLow() - last) < 250000;
}

// Physical movement vector: d-pad wins over the left stick
static int read_move_vector(const SceCtrlData *pad, float *ox, float *oy) {
    float x = 0, y = 0;
    if (pad->buttons & SCE_CTRL_LEFT) x -= 1;
    if (pad->buttons & SCE_CTRL_RIGHT) x += 1;
    if (pad->buttons & SCE_CTRL_UP) y -= 1;
    if (pad->buttons & SCE_CTRL_DOWN) y += 1;
    if (x != 0 || y != 0) {
        float m = sqrtf(x * x + y * y);
        *ox = x / m; *oy = y / m;
        return 1;
    }

    x = (pad->lx - 128) / 128.0f;
    y = (pad->ly - 128) / 128.0f;
    float m = sqrtf(x * x + y * y);
    if (m < DIRECT_MOVE_DEADZONE) return 0;
    float k = (m - DIRECT_MOVE_DEADZONE) / (1.0f - DIRECT_MOVE_DEADZONE);
    if (k > 1) k = 1;
    *ox = x / m * k; *oy = y / m * k;
    return 1;
}

/*
 * "I Have The Power", done the way DefaultGamePadButtonListener::onReleased()
 * does it for the on-screen IHTP button:
 *   if (!GameSystems::get()->getGameScoring()->isIHTPBusy()) {
 *       player = CachedGameObjects::get()->getPlayer(0);
 *       pc = CPlayerCharacter::extractPlayerComponent(player.get());
 *       if (pc && pc->useConsumable(IHTP = 1)) {     // spends a charge
 *           GameDataManager::getInstance()->addedScriptIHTP(false);
 *           pc->changeToIHaveThePowerMode();
 *       }
 *   }
 * Runs on the game thread (from the GameInputManager::tick hook).
 */
typedef struct { void *px; void *pn; } SharedPtr;   // boost::shared_ptr
static void *(*gs_get)(void);
static void *(*gs_get_scoring)(void *);
static int (*scoring_ihtp_busy)(void *);
static void *(*cgo_get)(void);
static void (*cgo_get_player)(SharedPtr *ret, void *cgo, int idx);
static void *(*pc_extract)(void *go);
static int (*pc_use_consumable)(void *pc, int type);
static void *(*gdm_get)(void);
static void (*gdm_added_script_ihtp)(void *gdm, int b);
static void (*pc_change_to_ihtp)(void *pc);
static void (*sp_release)(void *pn);
static int ihtp_ready = 0;
static uint32_t ihtp_prev_buttons = 0;

static void do_ihtp(void) {
    void *scoring = gs_get_scoring(gs_get());
    if (!scoring || scoring_ihtp_busy(scoring)) return;
    SharedPtr player = { 0, 0 };
    cgo_get_player(&player, cgo_get(), 0);
    if (player.px) {
        void *pc = pc_extract(player.px);
        if (pc && (uint8_t)pc_use_consumable(pc, 1)) {
            gdm_added_script_ihtp(gdm_get(), 0);
            pc_change_to_ihtp(pc);
        }
    }
    if (player.pn) sp_release(player.pn);
}

static void ihtp_poll(void *gim, uint32_t buttons) {
    if (!ihtp_ready || !loaded) return;
    uint32_t pressed = buttons & ~ihtp_prev_buttons;
    ihtp_prev_buttons = buttons;
    if (!pressed || gim_is_input_locked(gim)) return;
    for (int i = 0; i < n_binds; i++)
        if (binds[i].type == B_IHTP && (pressed & binds[i].mask)) {
            do_ihtp();
            break;
        }
}

static void game_input_manager_tick_hook(void *gim, uint32_t dt_bits) {
    void *gid = *(void **)((uint8_t *)gim + 24);   // GameInputManager::getGameInputData()
    if (!gid) return;

    direct_move_last_tick = sceKernelGetProcessTimeLow() | 1;

    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);   // Ext2: the L/R shoulders report as L1/R1

    ihtp_poll(gim, pad.buttons);

    float x = 0, y = 0;
    int held = read_move_vector(&pad, &x, &y);

    if (gim_is_input_locked(gim)) {
        // cutscene / hidden gamepad: the touch handler ignores input too
        direct_move_active = 0;
    } else if (held || direct_move_active) {
        uint8_t *in = gid_get_input(gid, 0);
        float *dir = (float *)in, *pos = (float *)(in + 8);
        int *state = (int *)(in + 20);

        if (held) {
            if (!direct_move_active && *state != 2) {
                *state = 1;
                pos[0] = binds_stick_cx();
                pos[1] = binds_stick_cy();
            }
            dir[0] = x; dir[1] = y;
            direct_move_last_x = x; direct_move_last_y = y;
            direct_move_active = 1;
        } else {
            if (*state != 2) *state = 4;
            dir[0] = direct_move_last_x; dir[1] = direct_move_last_y;
            direct_move_active = 0;
        }
    }

    gid_tick(gid, dt_bits);
}

void touch_buttons_patch(void) {
    uintptr_t tick = (uintptr_t)so_symbol(&so_mod, "_ZN16GameInputManager4tickEf");
    gid_tick = (gid_tick_t)so_symbol(&so_mod, "_ZN13GameInputData4tickEf");
    gid_get_input = (gid_get_input_t)so_symbol(&so_mod, "_ZN13GameInputData8getInputENS_5INPUTE");
    gim_is_input_locked = (gim_locked_t)so_symbol(&so_mod, "_ZN16GameInputManager13isInputLockedEv");
    if (!tick || !gid_tick || !gid_get_input || !gim_is_input_locked) {
        sceClibPrintf("[controls] direct movement hook NOT installed (missing symbols)\n");
        return;
    }
    hook_addr(tick, (uintptr_t)&game_input_manager_tick_hook);
    sceClibPrintf("[controls] direct movement hook installed\n");

#define SYM(var, name) (*(void **)&(var) = so_symbol(&so_mod, name))
    SYM(gs_get, "_ZN11GameSystems3getEv");
    SYM(gs_get_scoring, "_ZN11GameSystems14getGameScoringEv");
    SYM(scoring_ihtp_busy, "_ZN11GameScoring10isIHTPBusyEv");
    SYM(cgo_get, "_ZN17CachedGameObjects3getEv");
    SYM(cgo_get_player, "_ZN17CachedGameObjects9getPlayerEi");
    SYM(pc_extract, "_ZN16CPlayerCharacter22extractPlayerComponentEPN4helo12GoGameObjectE");
    SYM(pc_use_consumable, "_ZN16CPlayerCharacter13useConsumableEN15GameDataManager14ConsumableTypeE");
    SYM(gdm_get, "_ZN9SingletonI15GameDataManagerE11getInstanceEv");
    SYM(gdm_added_script_ihtp, "_ZN15GameDataManager15addedScriptIHTPEh");
    SYM(pc_change_to_ihtp, "_ZN16CPlayerCharacter25changeToIHaveThePowerModeEv");
    SYM(sp_release, "_ZN5boost6detail15sp_counted_base7releaseEv");
#undef SYM
    ihtp_ready = gs_get && gs_get_scoring && scoring_ihtp_busy && cgo_get && cgo_get_player &&
                 pc_extract && pc_use_consumable && gdm_get && gdm_added_script_ihtp &&
                 pc_change_to_ihtp && sp_release;
    sceClibPrintf("[controls] IHTP button %s\n", ihtp_ready ? "ready" : "NOT available (missing symbols)");
}

void fndk_inject_touch(SceTouchData *t) {
    if (!loaded)
        load_controls();

    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);   // Ext2: the L/R shoulders report as L1/R1

    float sx = (pad.lx - 128) / 128.0f, sy = (pad.ly - 128) / 128.0f;
    if (fabsf(sx) < 0.25f && fabsf(sy) < 0.25f) sx = sy = 0;
    float dx = 0, dy = 0;
    if (pad.buttons & SCE_CTRL_LEFT) dx -= 1;
    if (pad.buttons & SCE_CTRL_RIGHT) dx += 1;
    if (pad.buttons & SCE_CTRL_UP) dy -= 1;
    if (pad.buttons & SCE_CTRL_DOWN) dy += 1;

    // In gameplay movement goes straight into GameInputData (see above)
    if (direct_move_live()) sx = sy = dx = dy = 0;

    for (int i = 0; i < n_binds; i++) {
        Binding *b = &binds[i];
        int id = 200 + i;
        if (b->type == B_STICK) {
            float vx = b->is_dpad ? dx : sx, vy = b->is_dpad ? dy : sy;
            if (vx == 0 && vy == 0) { b->active = 0; continue; }
            float tx = b->x1 + vx * b->r, ty = b->y1 + vy * b->r;
            if (!b->active) {
                // finger lands on the pad centre first...
                b->active = 1;
                b->frame = 0;
                b->cur_x = b->x1;
                b->cur_y = b->y1;
            } else {
                // ...then slides towards the stick direction (WDirectionPad is
                // drag based and wants a real drag, not a jump)
                b->cur_x += (tx - b->cur_x) * 0.35f;
                b->cur_y += (ty - b->cur_y) * 0.35f;
                b->frame++;
            }
            // keep MOVE events flowing while held: alternate a 1 px wiggle
            float wiggle = (b->frame & 1) ? 1.0f : 0.0f;
            add_report(t, id, b->cur_x + wiggle, b->cur_y);
        } else if (b->type == B_TAP) {
            if (pad.buttons & b->mask)
                add_report(t, id, b->x1, b->y1);
        } else if (b->type == B_IHTP) {
            continue;   // handled on the game thread (ihtp_poll)
        } else { // swipe: fixed 6-frame gesture started on each new press
            int pressed = (pad.buttons & b->mask) != 0;
            if (pressed && !b->prev_pressed && !b->active) { b->active = 1; b->frame = 0; }
            b->prev_pressed = pressed;
            if (b->active) {
                float k = b->frame / 4.0f;
                if (k > 1) k = 1;
                add_report(t, id, b->x1 + (b->x2 - b->x1) * k, b->y1 + (b->y2 - b->y1) * k);
                if (++b->frame > 5) b->active = 0;   // lifted on the next poll
            }
        }
    }
}
