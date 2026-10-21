/* START CAMERA: which camera a race starts with (design point 5).

   The game's cameras (0x44ddc0(camera, player) sets one; it prints its name, Text.idf's
   cameraname, at the screen's bottom): F1 1 "driver camera" (the driver's eyes, hands on the
   wheel), F2 13 "onboard 3", F3 11 "onboard 1", F4 12 "onboard 2" (the car's own mounted cameras,
   from its data), F5 21 "helicopter 1" and F6 22 "helicopter 2" (chase cameras: cam_heli_dist /
   height and ..2 in the registry), F7 41 "external view" and F8 42 "3rd person view" (turned with
   the head turn/rise controls). Shift+F1-F4 are the TV cameras 31-34; F9 (and the controller's
   Y) steps through 1, 13, 21, 22, 11, 12, 41, 42.

   As shipped a race starts on 22, the start line's view, and at the countdown's first beep
   (0x4552ea; 0x46b273/0x46b7ea/0x46b806 in network races) switches to DefaultCamera (0x4c7f34,
   registry DefaultCamera, 21 in a new install). F1-F8 set DefaultCamera to what they chose;
   F9 / Y do not. So races already start on the last camera chosen by F-key, but not one reached
   by stepping through them.

   START CAMERA (options.txt start_camera): ORIGINAL leaves all that alone; LAST USED starts on
   the camera the player last switched to in a race, by F-key or by stepping (start_camera_last
   in options.txt, so it carries over to the next session); DRIVER .. 3RD PERSON (f1 .. f8, the
   game's names for F1-F8) always start on that one. Only the switch at the countdown is changed (race mode 1: not replays, which have
   their own cameras, nor the attract-mode demo), and DefaultCamera is never written, so ORIGINAL
   stays as shipped. Free Roam has no countdown, so the game never makes that switch and stays on
   22 (Ant: "the camera doesn't save in free roam"): there the chosen camera replaces 22 where the
   race sets it up (0x45db76). The line is on DISPLAY OPTIONS, in the menus and in the pause menu (prompts.c
   adds it to the scripts). Off in --test runs unless INSANE_ENHANCED, like the Enhanced options. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "funcs.h"
#include "image.h"
#include "options.h"

#define CURRENT_CAMERA  0x4c7f80u       /* the camera in use */
#define LOCAL_PLAYER    0x1c54d7cu      /* the player the view follows */
#define GAME_MODE       0x4c7f20u       /* MV_GAMEMODE: 0 Free Roam, 1 Off-Road Race, 2 Pathfinder, 3 Jamboree, ... */
#define FREE_ROAM       0
#define SETUP_SITE      0x45db76u       /* the race set up on the start line's camera (22) */
static const uint32_t start_sites[] = {0x4552eau, 0x46b273u, 0x46b7eau, 0x46b806u};  /* to DefaultCamera at the countdown */
static const uint32_t chosen_sites[] = {0x45eeb2u, 0x45ef20u};                      /* F1-F8 / Shift+F1-F4, F9 / Y */

/* (shown: the game's own names for them, Text.idf's cameraname as it prints them at the screen's
   bottom on a switch, "driver camera" .. "external view", "3rd person view", cut to what the value
   column holds, 12 letters; Ant knew F7 by its "external view", not by our first names, ORBIT 1 and
   the like) */
static const struct { const char *value, *shown; int camera; } choices[] = {
    {"original", "ORIGINAL", 0}, {"last", "LAST USED", -1},
    {"f1", "DRIVER", 1}, {"f2", "ONBOARD 3", 13}, {"f3", "ONBOARD 1", 11}, {"f4", "ONBOARD 2", 12},
    {"f5", "HELICOPTER 1", 21}, {"f6", "HELICOPTER 2", 22}, {"f7", "EXTERNAL", 41}, {"f8", "3RD PERSON", 42},
};
#define NCHOICES (int)(sizeof choices / sizeof *choices)

static int choice(void)
{
    const char *v = option_str("start_camera", "original");
    for (int k = 0; k < NCHOICES; k++)
        if (!strcasecmp(v, choices[k].value))
            return k;
    return 0;
}

static int returns_to(CPU *c, const uint32_t *sites, size_t n)
{
    uint32_t ret = R32(c->esp);
    for (size_t k = 0; k < n; k++)
        if (ret == sites[k])
            return 1;
    return 0;
}

/* 0x44ddc0(camera, player) */
static int camera_hook(CPU *c)
{
    static int log = -1;
    if (log < 0)
        log = getenv("INSANE_CAM_LOG") != NULL;
    int cam = (int)R32(c->esp + 4), player = (int)R32(c->esp + 8);
    if (R32(GAME_RACE_MODE) == 1 && player == (int)R32(LOCAL_PLAYER) && returns_to(c, chosen_sites, 2) && cam > 0) {
        if (atoi(option_str("start_camera_last", "0")) != cam) {
            char v[16];
            snprintf(v, sizeof v, "%d", cam);
            option_set("start_camera_last", v);
            options_save();
        }
    }
    int k = choice();
    const uint32_t setup = SETUP_SITE;
    if (R32(GAME_RACE_MODE) == 1 && k && (returns_to(c, start_sites, 4) ||
                                          (R32(GAME_MODE) == FREE_ROAM && returns_to(c, &setup, 1)))) {
        int want = choices[k].camera < 0 ? atoi(option_str("start_camera_last", "0")) : choices[k].camera;
        if (want > 0 && want != cam) {
            if (log)
                LOG(LOG_ALWAYS, "camera: the race starts on %d (%s) instead of %d", want, choices[k].shown, cam);
            /* already on it (F6 is the start line's camera): no call, which for the chase cameras
               would also swap their custom and default distances, as pressing the key again does */
            if (want == (int)R32(CURRENT_CAMERA)) {
                c->ret = R32(c->esp);
                c->esp += 4;
                return 1;
            }
            W32(c->esp + 4, (uint32_t)want);
        }
    }
    if (log)
        LOG(LOG_ALWAYS, "camera %lu: set %d player %d from %08x (current %d, race mode %u state %u)", frame_count,
            (int)R32(c->esp + 4), player, R32(c->esp), (int)R32(CURRENT_CAMERA), R32(GAME_RACE_MODE), R32(GAME_RACE_STATE));
    if (log)
        LOG(LOG_ALWAYS, "camera: game mode %u", R32(GAME_MODE));
    return 0;
}

/* ------------------------------------------------------------------ the menu line */
static uint32_t text_buf;

METHOD(cam_text, 0)
{
    gstrncpy(text_buf, choices[choice()].shown, 32);
    return text_buf;
}

static void cam_step(int dir)
{
    int k = (choice() + dir + NCHOICES) % NCHOICES;
    option_set("start_camera", choices[k].value);
    options_save();
}
METHOD(cam_next, 0) { cam_step(1); return 0; }
METHOD(cam_prev, 0) { cam_step(-1); return 0; }

static uint32_t var_entry;
static int (*prev_vars)(CPU *c);

/* 0x47cff0(menu): MV_CAM_START in front of the variables linked into it */
static int vars_hook(CPU *c)
{
    uint32_t menu = R32(c->esp + 4);
    if (prev_vars)
        prev_vars(c);
    else
        F_0047cff0_orig(c);
    if (!var_entry) {
        uint32_t zero = galloc(4), name = galloc(32);
        W32(zero, 0);
        gstrncpy(name, "MV_CAM_START", 32);
        text_buf = galloc(32);
        W8(text_buf, 0);
        var_entry = galloc(36);
        const uint32_t v[9] = {0, name, thunk_addr(cam_text, "start camera text"), 0x100, zero, zero,
                               thunk_addr(cam_next, "start camera next"), thunk_addr(cam_prev, "start camera prev"), 0};
        for (int k = 0; k < 9; k++)
            W32(var_entry + 4 * k, v[k]);
    }
    W32(var_entry, R32(menu + 0x10));
    W32(menu + 0x10, var_entry);
    return 1;
}

void camera_init(void)
{
    if (deterministic && !getenv("INSANE_ENHANCED"))
        return;                     /* the game as shipped: --test runs compare it */
    hook_camera_set = camera_hook;
    prev_vars = hook_menu_vars;
    hook_menu_vars = vars_hook;
}
