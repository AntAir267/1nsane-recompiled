/* The Enhanced options in the game's own menus (design point 1).

   The menus are scripts (data\menu.cfg and menu_ingame.cfg, in the language's Text.idf) that the
   game reads through its archive opener (0x44c110). A hook there hands it patched copies kept
   in the user folder (cache\):
   - Options gets an ENHANCED OPTIONS line, leading to a page built like VIDEO OPTIONS, and the
     pause menu in a race gets one with the settings that apply at once;
   - VIDEO OPTIONS gets a COLOUR DEPTH line and a line saying what Vulkan draws at, and its
     RESOLUTION line offers NATIVE (the screen's own resolution) and the game's display mode
     sizes. INTERFACE names the Vulkan renderer (d3d.c's device names).
   The game's menu variables (MV_*: a static table at 0x4c9038 that 0x47cff0 links into each
   menu it loads, 36-byte entries {link, name, value, type, min, max, next, prev, list}) get ours
   linked in front: type 0x100 (a function returning the text to show) with our functions
   behind thunks for the text and for Right/Left. Values live in options.txt; a change is saved
   at once and applied straight away (options_changed), or at the next start where noted.

   Off in --test runs unless INSANE_ENHANCED is set: those compare the game as shipped. */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include "runtime.h"
#include "options.h"
#include "funcs.h"
#include "vk.h"
#include "widescreen.h"
#include "drawdist.h"
#include "font.h"
#include "opponents.h"
#include "shadows.h"
#include "smoke.h"
#include "headlights.h"

/* game addresses (GOG build) */
#define MV_TABLE        0x4c9038u       /* first entry of the menu variable table */
#define MV_ENTRY        36u
#define MV_COUNT        165             /* entries, ML_MAPS to MV_MNU_WATCHWHAT */
#define VID_DEVICE      0x1c54b8cu      /* current render device record */
#define VID_MODE        0x1c54b90u      /* current display mode record: +0x68 width, +0x6c height, +0x70 bpp */
#define VID_MODE_NEXT   0x47a420u       /* the game's own Right/Left on RESOLUTION */
#define VID_MODE_PREV   0x47a480u
#define IDF_COUNT       0x1b84b90u      /* mounted directory entries */
#define WHEEL_TO_2_FN   0x4c8eccu       /* the menu action MA_WHEEL_TO_2's function (table entry +0xc) */
#define MENU_WHEEL      0x1c54eb8u      /* the main menu wheel's stop: 0 SINGLE PLAYER ... 3 EXIT */
#define MO_WHEEL_SLOT   0x4c8cecu       /* the menu object MO_WHEEL's function (table entry +0xc) */
#define MO_WHEEL_FN     0x471450u

int host_fullscreen(void);
void win_options(void);
int registry_get_str(const char *key, const char *name, char *out, int n);
void registry_set_str(const char *key, const char *name, const char *s);
#define GAME_KEY "HKLM\\Software\\Codemasters\\Insane"

static int enabled;

/* ------------------------------------------------------------------ settings */
typedef struct {
    const char *var, *key;              /* menu variable, options.txt key */
    const char *values[8];              /* options.txt values in menu order; values[0] is the default */
    const char *shown[8];               /* what the menu shows for each */
} Setting;

static Setting settings[] = {
    {"MV_ENH_WIDESCREEN", "widescreen", {"off", "on", "16:9", "16:10", "21:9"},
     {"OFF", "MATCH SCREEN", "16:9", "16:10", "21:9"}},
    {"MV_ENH_DISPLAY", "display", {"window", "fullscreen"}, {"WINDOW", "FULL SCREEN"}},
    {"MV_ENH_VSYNC", "vsync", {"on", "off"}, {"ON", "OFF"}},
    {"MV_ENH_AA", "antialiasing", {"off", "4x", "fsr", "dlss", "taa"}, {"OFF", "4X", "FSR", "DLSS", "TAA"}},
    /* (DLSS: NVIDIA RTX only; TAA: the port's own temporal upscaler, lighter than FSR 2) */
    {"MV_ENH_TEXTURES", "textures", {"original", "smooth"}, {"ORIGINAL", "SMOOTH"}},
    {"MV_ENH_RUMBLE", "rumble", {"on", "off"}, {"ON", "OFF"}},
    {"MV_ENH_VIDEOS", "videos", {"on", "off"}, {"ON", "OFF"}},
    {"MV_ENH_MODS", "mods", {"on", "off"}, {"ON", "OFF"}},
    {"MV_ENH_LANGUAGE", NULL, {"00", "01", "02", "03", "04"},
     {"ENGLISH", "FRANCAIS", "DEUTSCH", "ESPANOL", "ITALIANO"}},
    {"MV_ENH_DEPTH", NULL, {"16", "32"}, {"16-BIT", "32-BIT"}},
    {"MV_VID_MODE", NULL, {0}, {0}},    /* RESOLUTION: the game's own entry, taken over */
    {"MV_ENH_INFO", NULL, {0}, {0}},    /* what Vulkan draws at (a line of small text) */
    /* button prompts and control hints (prompts.c) */
    {"MV_ENH_PROMPTS", "prompts", {"auto", "keyboard", "xbox"}, {"AUTO", "KEYBOARD", "XBOX"}},
    {"MV_ENH_HINTS", "control_hints", {"useful", "always", "off"}, {"WHEN USEFUL", "ALWAYS", "OFF"}},
    /* the CHEATS page: the game's own cheat codes (Data\codes.cfg, typed in the menus) as switches */
    {"MV_ENH_C_CARS", "cheat_cars", {"on", "off"}, {"ON", "OFF"}},
    {"MV_ENH_C_MAPS", "cheat_maps", {"on", "off"}, {"ON", "OFF"}},
    {"MV_ENH_C_CHAMPS", "cheat_championships", {"on", "off"}, {"ON", "OFF"}},
    {"MV_ENH_C_GENERATOR", "cheat_generator", {"on", "off"}, {"ON", "OFF"}},
    {"MV_ENH_C_WIN", "cheat_ctrl_w_wins", {"off", "on"}, {"OFF", "ON"}},
    {"MV_ENH_C_WHEELS", "cheat_wheels", {"normal", "gokart", "bigfoot"}, {"NORMAL", "GO-KART", "BIGFOOT"}},
    {"MV_ENH_C_HEADS", "cheat_big_heads", {"off", "on"}, {"OFF", "ON"}},
    {"MV_ENH_C_BOXERS", "cheat_boxers", {"off", "on"}, {"OFF", "ON"}},
    {"MV_ENH_C_GRIP", "cheat_grip", {"normal", "greasy", "glued"}, {"NORMAL", "GREASY", "GLUED"}},
    {"MV_ENH_C_GRAVITY", "cheat_gravity", {"normal", "moon"}, {"NORMAL", "MOON"}},
    {"MV_ENH_C_SIZE", "cheat_size", {"normal", "double", "half", "quarter", "tenth", "tiny"},
     {"NORMAL", "DOUBLE", "HALF", "QUARTER", "TENTH", "TINY"}},
    /* draw distance and distance fade (drawdist.c) */
    {"MV_ENH_DRAWDIST", "draw_distance", {"original", "1.5x", "2x", "3x", "4x", "auto"},
     {"ORIGINAL", "1.5X", "2X", "3X", "4X", "AUTO"}},
    {"MV_ENH_DISTFADE", "distance_fade", {"off", "on"}, {"OFF", "ON"}},
    /* more AI opponents (opponents.c): how far the race setup's OPPONENT NUMBER goes */
    {"MV_ENH_OPPONENTS", "max_opponents", {"8", "15"}, {"8", "15"}},
    /* ray-traced sun shadows (shadows.c) */
    {"MV_ENH_SHADOWS", "shadows", {"original", "raytraced"}, {"ORIGINAL", "RAY TRACED"}},
    /* ray-traced ambient occlusion (shadows.c) */
    {"MV_ENH_AO", "ambient_occlusion", {"off", "light", "on"}, {"OFF", "LIGHT", "FULL"}},   /* "on": FULL */
    /* ray-traced reflections (shadows.c) */
    {"MV_ENH_REFL", "reflections", {"original", "light", "raytraced"}, {"ORIGINAL", "LIGHT", "FULL"}},   /* "raytraced": FULL */
    /* temporal anti-aliasing (taa.c, vk.c): the scene drawn at 1 / 1, 1.5, 1.7, 2 of the screen's size (FSR 2's modes),
       or AUTO: the size that keeps the GPU inside the frame's time (vk.c "automatic render scale") */
    {"MV_ENH_RSCALE", "render_scale", {"native", "quality", "balanced", "performance", "auto"},
     {"NATIVE", "QUALITY", "BALANCED", "PERFORMANCE", "AUTO"}},
    /* car classes (classes.c): as shipped, or the reorganized table; at the next start */
    {"MV_ENH_CLASSES", "car_classes", {"original", "reorganized"}, {"ORIGINAL", "REORGANIZED"}},
    /* volumetric smoke and dust (smoke.c): the exhausts' smoke, the wheels' dust and smoke, splashes */
    {"MV_ENH_SMOKE", "smoke", {"original", "volumetric"}, {"ORIGINAL", "VOLUMETRIC"}},
    /* colour precision (vk.c): the 3D world's blending in 16 bits a channel, dithered into the 8-bit picture */
    {"MV_ENH_PRECISION", "colour_precision", {"original", "high"}, {"ORIGINAL", "HIGH"}},
    /* a frame rate limit (main.c): "screen" is 3 under the screen's refresh (variable refresh: G-Sync,
       FreeSync), so frames stay where the screen follows them */
    {"MV_ENH_FRAMELIMIT", "frame_limit", {"off", "screen", "30", "60", "90", "120", "144"},
     {"OFF", "SCREEN", "30 FPS", "60 FPS", "90 FPS", "120 FPS", "144 FPS"}},
    /* headlights (headlights.c): the cars' beams on night maps, lighting what is ahead per pixel, with shadows */
    {"MV_ENH_HEADLIGHTS", "headlights", {"off", "on", "volumetric"}, {"OFF", "ON", "VOLUMETRIC"}},
    {"MV_ENH_FONT", "font", {"original", "1nsanefont", "mirabel"}, {"ORIGINAL", "1NSANEFONT", "MIRABEL"}},  /* font.c */
    {"MV_ENH_INFO2", NULL, {0}, {0}},   /* the info line's second: the GPU */
};
static uint32_t texts[40];              /* program-memory buffers for the shown text */

enum { S_WIDESCREEN, S_DISPLAY, S_VSYNC, S_AA, S_TEXTURES, S_RUMBLE, S_VIDEOS, S_MODS, S_LANGUAGE, S_DEPTH,
       S_RESOLUTION, S_INFO, S_PROMPTS, S_HINTS, S_C_CARS, S_C_MAPS, S_C_CHAMPS, S_C_GENERATOR, S_C_WIN, S_C_WHEELS, S_C_HEADS,
       S_C_BOXERS, S_C_GRIP, S_C_GRAVITY, S_C_SIZE, S_DRAWDIST, S_DISTFADE, S_OPPONENTS, S_SHADOWS, S_AO, S_REFL,
       S_RSCALE, S_CLASSES, S_SMOKE, S_PRECISION, S_FRAMELIMIT, S_HEADLIGHTS, S_FONT, S_INFO2, NSETTINGS };

/* ------------------------------------------------------------------ cheats */
/* What each code sets (the handler at 0x475c01 compares the typed text with codes.cfg's list
   and the size words). Flags are never cleared by the game, values only by a game start
   (0x46cd00 resets the wheel scale), so they are written at start-up, after that reset, and when
   the page changes one. 0x46e690 recomputes what is unlocked, as the championsareus code does. */
#define CH_ALL_CARS     0x1c54a30u      /* ineedmorecars */
#define CH_ALL_MAPS     0x1c54a34u      /* ineedmoremaps */
#define CH_ALL_CHAMPS   0x1c54a3cu      /* championsareus */
#define CH_GENERATOR    0x1c54a44u      /* terraformer; also MV_GEN_ENABLED at 0x1c54d40 */
#define CH_GEN_ENABLED  0x1c54d40u
#define CH_CTRL_W       0x1c54a50u      /* imacheater: Ctrl+W wins the race */
#define CH_WHEELS       0x4cf95cu       /* gokartz 0.7, bigfootz 1.5 */
#define CH_HEADS        0x4c59a4u       /* bigheadz 2 */
#define CH_BOXERS_A     0x4c59a8u       /* boxerz 2, 2 */
#define CH_BOXERS_B     0x4c59acu
#define CH_GRIP         0x4c59a0u       /* greasy 0.5, glued 2 */
#define CH_GRAVITY      0x4c599cu       /* manonthemoon 0.5 */
#define CH_SIZE         0x4cf958u       /* normalsize, doublesize, halfsize, quartersize, tenthsize, veryverysmall */
#define UNLOCKS_UPDATE  0x46e690u

static int current(int i);
static void wf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }

static void cheats_apply(CPU *c, int update_unlocks)
{
    static const float wheels[] = {1.0f, 0.7f, 1.5f}, grip[] = {1.0f, 0.5f, 2.0f};
    static const float size[] = {1.0f, 2.0f, 0.5f, 0.25f, 0.1f, 0.01f};
    uint32_t before = R32(CH_ALL_CARS) | R32(CH_ALL_MAPS) << 1 | R32(CH_ALL_CHAMPS) << 2 | R32(CH_GENERATOR) << 3;
    W32(CH_ALL_CARS, current(S_C_CARS) == 0);
    W32(CH_ALL_MAPS, current(S_C_MAPS) == 0);
    W32(CH_ALL_CHAMPS, current(S_C_CHAMPS) == 0);
    W32(CH_GENERATOR, current(S_C_GENERATOR) == 0);
    W32(CH_GEN_ENABLED, current(S_C_GENERATOR) == 0);
    W32(CH_CTRL_W, current(S_C_WIN) == 1);
    wf(CH_WHEELS, wheels[current(S_C_WHEELS)]);
    wf(CH_HEADS, current(S_C_HEADS) ? 2.0f : 1.0f);
    wf(CH_BOXERS_A, current(S_C_BOXERS) ? 2.0f : 1.0f);
    wf(CH_BOXERS_B, current(S_C_BOXERS) ? 2.0f : 1.0f);
    wf(CH_GRIP, grip[current(S_C_GRIP)]);
    wf(CH_GRAVITY, current(S_C_GRAVITY) ? 0.5f : 1.0f);
    wf(CH_SIZE, size[current(S_C_SIZE)]);
    uint32_t after = R32(CH_ALL_CARS) | R32(CH_ALL_MAPS) << 1 | R32(CH_ALL_CHAMPS) << 2 | R32(CH_GENERATOR) << 3;
    if (update_unlocks && after != before)
        guest_call(c, UNLOCKS_UPDATE, 0);
}

/* 0x46cd00: a game starts (resets the wheel scale): our values again afterwards */
static int hook_game_start_fn(CPU *c)
{
    F_0046cd00_orig(c);
    cheats_apply(c, 0);
    return 1;
}

static int nvalues(const Setting *s)
{
    int n = 0;
    while (n < 8 && s->values[n]) n++;
    return n;
}

static int current(int i)
{
    const Setting *s = &settings[i];
    if (i == S_LANGUAGE) {
        char v[8] = "00";
        registry_get_str(GAME_KEY, "LangID", v, sizeof v);
        for (int k = 0; k < nvalues(s); k++)
            if (!strcmp(v, s->values[k])) return k;
        return 0;
    }
    if (i == S_DEPTH) {
        uint32_t m = R32(VID_MODE);
        return m && R32(m + 0x70) == 32;
    }
    if (i == S_DISPLAY)
        return host_fullscreen();
    const char *v = option_str(s->key, s->values[0]);
    for (int k = 0; k < nvalues(s); k++)
        if (!strcasecmp(v, s->values[k])) return k;
    if (i == S_WIDESCREEN && strchr(v, ':')) return 2;
    if (i == S_AO && !strcasecmp(v, "full")) return 2;
    if (i == S_REFL && !strcasecmp(v, "full")) return 2;
    return 0;
}

/* ------------------------------------------------------------------ RESOLUTION and COLOUR DEPTH */
extern const uint32_t (*const ddraw_mode_sizes)[2];  /* ddraw.c: the display mode sizes it offers */
extern const int ddraw_nmode_sizes;

static int mode_matches(uint32_t w, uint32_t h, uint32_t bpp)
{
    uint32_t m = R32(VID_MODE);
    return m && R32(m + 0x68) == w && R32(m + 0x6c) == h && (!bpp || R32(m + 0x70) == bpp);
}

/* Step the game's own mode selection (its RESOLUTION handler, so everything it updates stays
   consistent) until it shows w x h x bpp; returns 0 if the game has no such mode. */
static int game_mode_select(CPU *c, uint32_t w, uint32_t h, uint32_t bpp)
{
    for (int k = 0; k < 64 && !mode_matches(w, h, bpp); k++)
        guest_call(c, VID_MODE_NEXT, 0);
    return mode_matches(w, h, bpp);
}

/* 0 = NATIVE, 1.. = ddraw_mode_sizes[k - 1] */
static int resolution_index(void)
{
    if (!strcasecmp(option_str("resolution", "original"), "native"))
        return 0;
    uint32_t m = R32(VID_MODE);
    for (int k = 0; m && k < ddraw_nmode_sizes; k++)
        if (R32(m + 0x68) == ddraw_mode_sizes[k][0] && R32(m + 0x6c) == ddraw_mode_sizes[k][1])
            return k + 1;
    return 1;
}

static void resolution_step(CPU *c, int dir)
{
    uint32_t m = R32(VID_MODE);
    uint32_t bpp = m ? R32(m + 0x70) : 16;
    int n = ddraw_nmode_sizes + 1, i = resolution_index();
    for (int tries = 0; tries < n; tries++) {
        i = (i + dir + n) % n;
        if (i == 0) {
            option_set("resolution", "native");
            break;
        }
        if (game_mode_select(c, ddraw_mode_sizes[i - 1][0], ddraw_mode_sizes[i - 1][1], bpp) ||
            game_mode_select(c, ddraw_mode_sizes[i - 1][0], ddraw_mode_sizes[i - 1][1], 0)) {
            option_set("resolution", "original");
            break;
        }
    }
}

static void depth_step(CPU *c)
{
    uint32_t m = R32(VID_MODE);
    if (!m) return;
    uint32_t w = R32(m + 0x68), h = R32(m + 0x6c), bpp = R32(m + 0x70) == 32 ? 16 : 32;
    game_mode_select(c, w, h, bpp);
}

/* ------------------------------------------------------------------ the menu's calls */
static void show(int i, const char *text)
{
    char up[128];
    int k = 0;
    for (; text[k] && k < 127; k++)
        up[k] = i == S_INFO || i == S_INFO2 ? text[k] : (char)toupper((unsigned char)text[k]);
    up[k] = 0;
    gstrncpy(texts[i], up, 128);
}

static uint32_t setting_text(int i)
{
    if (i == S_INFO || i == S_INFO2) {
        /* "Vulkan: <size>[, DLSS from <size>], <GPU>": the sizes on the first line, the GPU the second */
        char t[128];
        vk_render_info(t, sizeof t);
        char *gpu = strrchr(t, ',');
        if (gpu)
            *gpu = 0;
        const char *first = strncmp(t, "Vulkan: ", 8) ? t : t + 8;
        show(i, i == S_INFO2 ? (gpu ? gpu + 2 : "") : first);
    } else if (i == S_RESOLUTION) {
        int r = resolution_index();
        char t[48];
        if (r == 0) {
            snprintf(t, sizeof t, "NATIVE");        /* the size is on the info line below */
        } else {
            snprintf(t, sizeof t, "%uX%u", (unsigned)ddraw_mode_sizes[r - 1][0], (unsigned)ddraw_mode_sizes[r - 1][1]);
        }
        show(i, t);
    } else if ((i == S_SHADOWS || i == S_AO || i == S_REFL || i == S_HEADLIGHTS) && !shadows_available()) {
        show(i, "UNAVAILABLE");                      /* no ray tracing on this GPU */
    } else if (i == S_SMOKE && !smoke_available()) {
        show(i, "UNAVAILABLE");                      /* the software renderer */
    } else if (i == S_PRECISION && !vk_hp_available()) {
        show(i, "UNAVAILABLE");                      /* the software renderer, or no 16-bit colour to blend */
    } else if (i == S_AA && current(i) == 2 && !vk_taa_available()) {
        show(i, "FSR UNAVAILABLE");                  /* FSR 2 not built in, or the GPU lacks what it needs */
    } else if (i == S_AA && current(i) == 3 && !vk_dlss_available()) {
        show(i, "DLSS UNAVAILABLE");                 /* DLSS not built in, or not an NVIDIA RTX GPU */
    } else if (i == S_AA && current(i) == 4 && !vk_own_taa_available()) {
        show(i, "TAA UNAVAILABLE");                  /* the software renderer, or no scene images */
    } else if (i == S_RSCALE && current(S_AA) < 2) {
        show(i, vk_dlss_available() ? "WITH TAA/FSR/DLSS" : "WITH TAA OR FSR");
    } else if (i == S_WIDESCREEN && current(i) == 2 && strcmp(option_str("widescreen", ""), "16:9")) {
        show(i, option_str("widescreen", ""));        /* some other aspect from options.txt */
    } else if (i == S_FRAMELIMIT && current(i) == 1) {
        char t[48];
        snprintf(t, sizeof t, "%d FPS (SCREEN)", frame_limit_screen());
        show(i, t);
    } else {
        show(i, settings[i].shown[current(i)]);
    }
    return texts[i];
}

static void setting_step(CPU *c, int i, int dir)
{
    Setting *s = &settings[i];
    if (i == S_INFO || i == S_INFO2) {
        return;
    } else if (i == S_RESOLUTION) {
        resolution_step(c, dir);
    } else if (i == S_DEPTH) {
        depth_step(c);
        return;                                 /* the game saves its own mode */
    } else if ((i == S_SHADOWS || i == S_AO || i == S_REFL || i == S_HEADLIGHTS) && !shadows_available()) {
        return;
    } else if (i == S_RSCALE && current(S_AA) != 2 && current(S_AA) != 3) {
        return;
    } else if (i == S_SMOKE && !smoke_available()) {
        return;
    } else if (i == S_PRECISION && !vk_hp_available()) {
        return;
    } else {
        int n = nvalues(s), k = (current(i) + dir + n) % n;
        for (int t = 0; t < 3 && i == S_AA; t++)              /* (no FSR, DLSS or TAA here: past them) */
            if ((k == 2 && !vk_taa_available()) || (k == 3 && !vk_dlss_available()) ||
                (k == 4 && !vk_own_taa_available()))
                k = (k + dir + n) % n;
        if (i == S_LANGUAGE) {
            registry_set_str(GAME_KEY, "LangID", s->values[k]);
            settings_save();
            return;
        }
        option_set(s->key, s->values[k]);
        if (i >= S_C_CARS)
            cheats_apply(c, 1);
    }
    options_save();
    options_changed();
}

/* one set of thunk targets per setting (a thunk stands for one runtime function) */
#define SLOT(n) \
    METHOD(get_##n, 0) { return setting_text(n); } \
    METHOD(next_##n, 0) { setting_step(c, n, 1); return 0; } \
    METHOD(prev_##n, 0) { setting_step(c, n, -1); return 0; }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11)
SLOT(12) SLOT(13) SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22)
SLOT(23) SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31) SLOT(32) SLOT(33) SLOT(34)
SLOT(35) SLOT(36) SLOT(37) SLOT(38)
static void (*const slot_fns[][3])(CPU *) = {
    {get_0, next_0, prev_0}, {get_1, next_1, prev_1}, {get_2, next_2, prev_2}, {get_3, next_3, prev_3},
    {get_4, next_4, prev_4}, {get_5, next_5, prev_5}, {get_6, next_6, prev_6}, {get_7, next_7, prev_7},
    {get_8, next_8, prev_8}, {get_9, next_9, prev_9}, {get_10, next_10, prev_10}, {get_11, next_11, prev_11},
    {get_12, next_12, prev_12}, {get_13, next_13, prev_13}, {get_14, next_14, prev_14}, {get_15, next_15, prev_15},
    {get_16, next_16, prev_16}, {get_17, next_17, prev_17}, {get_18, next_18, prev_18}, {get_19, next_19, prev_19},
    {get_20, next_20, prev_20}, {get_21, next_21, prev_21}, {get_22, next_22, prev_22}, {get_23, next_23, prev_23},
    {get_24, next_24, prev_24}, {get_25, next_25, prev_25}, {get_26, next_26, prev_26},
    {get_27, next_27, prev_27}, {get_28, next_28, prev_28}, {get_29, next_29, prev_29}, {get_30, next_30, prev_30},
    {get_31, next_31, prev_31}, {get_32, next_32, prev_32}, {get_33, next_33, prev_33}, {get_34, next_34, prev_34},
    {get_35, next_35, prev_35}, {get_36, next_36, prev_36}, {get_37, next_37, prev_37},
    {get_38, next_38, prev_38}};
_Static_assert(sizeof slot_fns / sizeof slot_fns[0] == NSETTINGS, "one slot per setting");

/* ------------------------------------------------------------------ the game's lines with our steps
   VIDEO OPTIONS' (and the race's DISPLAY page's) SHADOWS, ENV. MAPPING (shown as REFLECTIONS) and
   SMOKE go on past the game's own steps into ours: RAY TRACED after HIGH; RAY TRACED (LIGHT) and RAY
   TRACED FULL; VOLUMETRIC after ON. The game's entries (type 0x101: a function returning a pointer to the shown
   index, and its Right and Left) are taken over: the game's own while ours is ORIGINAL, else our
   step, shown after the game's words (menu_text.cfg's arrays, extended: patch_text). Under ours the
   game's setting stays at the step before them. Without ray tracing (or smoke's renderer) only the
   game's. */
typedef struct {
    const char *var;
    int setting;
    uint32_t get, next, prev;           /* the game's */
    int host, after;                    /* the game's shown index ours follow, and the one after it */
    int words;                          /* the game's words (ours shown from this index) */
} Merge;
static const Merge merges[] = {
    {"MV_VID_SHADOWS", S_SHADOWS, 0x47cc80u, 0x47c950u, 0x47c9a0u, 4, 0, 5},    /* AUTO OFF LOW MEDIUM HIGH */
    {"MV_VID_ENVMAP", S_REFL, 0x47ccb0u, 0x47c9f0u, 0x47ca30u, 4, 0, 5},
    {"MV_VID_SMOKES", S_SMOKE, 0x47cce0u, 0x47caf0u, 0x47cb30u, 1, 2, 3},       /* OFF ON AUTO */
};
#define NMERGES 3
static uint32_t merge_shown;            /* NMERGES ints in program memory */

static int words_ok(void);

static int merge_avail(int m)
{
    return words_ok() && (merges[m].setting == S_SMOKE ? smoke_available() : shadows_available());
}

static uint32_t merge_get(CPU *c, int m)
{
    int k = current(merges[m].setting);
    if (!merge_avail(m) || k == 0)
        return guest_call(c, merges[m].get, 0);
    W32(merge_shown + 4 * m, (uint32_t)(merges[m].words + k - 1));
    return merge_shown + 4 * m;
}

/* the game's line to its shown index `want` (its own Right or Left, as many times as it takes: ours
   may have been chosen on another page with the game's anywhere) */
static void game_to(CPU *c, const Merge *g, int want, int dir)
{
    for (int t = 0; t < 8 && (int)R32(guest_call(c, g->get, 0)) != want; t++)
        guest_call(c, dir > 0 ? g->next : g->prev, 0);
}

static void merge_step(CPU *c, int m, int dir)
{
    const Merge *g = &merges[m];
    const Setting *s = &settings[g->setting];
    int n = nvalues(s) - 1, k = current(g->setting);
    if (!merge_avail(m)) {
        guest_call(c, dir > 0 ? g->next : g->prev, 0);
        return;
    }
    int at = (int)R32(guest_call(c, g->get, 0));
    if (dir > 0) {
        if (k > 0 && k < n)
            k++;
        else if (k == n) {
            k = 0;
            game_to(c, g, g->after, 1);         /* on past ours: the game's step after the one before them */
        } else if (at == g->host)
            k = 1;
        else {
            guest_call(c, g->next, 0);
            return;
        }
    } else {
        if (k > 1)
            k--;
        else if (k == 1) {
            k = 0;
            game_to(c, g, g->host, -1);         /* back from ours: the game's step before them */
        } else if (at == g->after) {
            game_to(c, g, g->host, -1);         /* back into ours, the game's step before them under them */
            k = n;
        } else {
            guest_call(c, g->prev, 0);
            return;
        }
    }
    option_set(s->key, s->values[k]);
    options_save();
    options_changed();
}

#define MERGE(n) \
    METHOD(mget_##n, 0) { return merge_get(c, n); } \
    METHOD(mnext_##n, 0) { merge_step(c, n, 1); return 0; } \
    METHOD(mprev_##n, 0) { merge_step(c, n, -1); return 0; }
MERGE(0) MERGE(1) MERGE(2)
static void (*const merge_fns[][3])(CPU *) = {{mget_0, mnext_0, mprev_0}, {mget_1, mnext_1, mprev_1},
                                              {mget_2, mnext_2, mprev_2}};
_Static_assert(sizeof merge_fns / sizeof merge_fns[0] == NMERGES && sizeof merges / sizeof merges[0] == NMERGES,
               "one set of thunks per merged line");

/* ------------------------------------------------------------------ menu variables */
static uint32_t our_first, our_last;    /* our entries, linked in front of the game's */

static void vars_build(void)
{
    uint32_t zero = galloc(4);
    W32(zero, 0);
    merge_shown = galloc(4 * NMERGES);
    for (int m = 0; m < NMERGES; m++)
        for (uint32_t e = MV_TABLE; e < MV_TABLE + MV_COUNT * MV_ENTRY; e += MV_ENTRY)
            if (!strcmp((const char *)gptr(R32(e + 4)), merges[m].var) && R32(e + 8) == merges[m].get) {
                W32(e + 8, thunk_addr(merge_fns[m][0], merges[m].var));
                W32(e + 24, thunk_addr(merge_fns[m][1], merges[m].var));
                W32(e + 28, thunk_addr(merge_fns[m][2], merges[m].var));
            }
    uint32_t prev = 0;
    for (int i = 0; i < NSETTINGS; i++) {
        texts[i] = galloc(128);
        W8(texts[i], 0);
        if (i == S_RESOLUTION) {
            /* the game's RESOLUTION entry itself: same name, our text and steps */
            for (uint32_t e = MV_TABLE; e < MV_TABLE + MV_COUNT * MV_ENTRY; e += MV_ENTRY)
                if (!strcmp((const char *)gptr(R32(e + 4)), "MV_VID_MODE")) {
                    W32(e + 8, thunk_addr(slot_fns[i][0], "enhanced resolution text"));
                    W32(e + 24, thunk_addr(slot_fns[i][1], "enhanced resolution next"));
                    W32(e + 28, thunk_addr(slot_fns[i][2], "enhanced resolution prev"));
                }
            continue;
        }
        uint32_t e = galloc(MV_ENTRY), name = galloc(32);
        gstrncpy(name, settings[i].var, 32);
        W32(e + 0, 0);
        W32(e + 4, name);
        W32(e + 8, thunk_addr(slot_fns[i][0], settings[i].var));
        W32(e + 12, 0x100);                     /* value: a function returning the text */
        W32(e + 16, zero);
        W32(e + 20, zero);
        W32(e + 24, thunk_addr(slot_fns[i][1], settings[i].var));
        W32(e + 28, thunk_addr(slot_fns[i][2], settings[i].var));
        W32(e + 32, 0);
        if (prev) W32(prev, e); else our_first = e;
        prev = e;
    }
    our_last = prev;
}

/* 0x47cff0(menu): links the static table and stores its head at menu+0x10 */
static int hook_vars(CPU *c)
{
    uint32_t menu = R32(c->esp + 4);
    F_0047cff0_orig(c);
    if (!our_first) vars_build();
    W32(our_last, R32(menu + 0x10));
    W32(menu + 0x10, our_first);
    return 1;
}

/* ------------------------------------------------------------------ the patched menu script */
static const char page[] =
    "\n;;;;;;;;;; 1NSANE native: Enhanced options\n"
    "block menu enhancedmenu\n{\n"
    "\tpos -0 -76\n\ttab 50  50  100  50\n\tmaxlines 14\n"
    "\tmp \"RUMBLE\" /select MV_ENH_RUMBLE\n"
    "\tmp \"BUTTON PROMPTS\" /select MV_ENH_PROMPTS\n"
    "\tmp \"CONTROL HINTS\" /select MV_ENH_HINTS\n"
    "\tmp \"FONT\" /select MV_ENH_FONT\n"
    "\tmp \"INTRO VIDEOS\" /select MV_ENH_VIDEOS\n"
    "\tmp \"FAN CARS & MAPS *\" /select MV_ENH_MODS\n"
    "\tmp \"CAR CLASSES *\" /select MV_ENH_CLASSES\n"
    "\tmp \"LANGUAGE *\" /select MV_ENH_LANGUAGE\n"
    "\tmp \"MAX OPPONENTS\" /select MV_ENH_OPPONENTS\n"
    "\tmp \"CHEATS\" [ENTER: enhancedmenu:out enhcheatsmenu:in enhcheatsmenu:run]\n"
    "\tmp \"BACK\" [ENTER: MA_BACK]\n"
    "\tblock state in\n\t\t{\n\t\tpos 100 -76\n\t\t}\n"
    "\tblock state out\n\t\t{\n\t\tpos 150 -76\n\t\t}\n"
    "\tblock rect menucsik\n\t{\n\t\t\tpos -100 -83\n\t\t\tpri -10\n\t\t\tsize 200 138\n"
    "\t\t\talpha 0 255 255 0\n\t\t\tcolor 0 5 25 42 \n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos 100 -83\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos 150 -83\n\t\t\t}\n\t}\n"
    "\tblock text enhancedoptions\n\t{\n\t\t\tjustify right\n\t\t\tpos 90 -92\n\t\t\tfont text 3\n"
    "\t\t\ttext \"ENHANCED OPTIONS\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos 150 -92\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos 150 -92\n\t\t\t}\n\t}\n"
    "\tblock text setting\n\t{\n\t\t\tpos -83 -76\n\t\t\tfont text 4\n"
    "\t\t\ttext \"$MV_ENH_RUMBLE\"\n\t\t\ttext \"$MV_ENH_PROMPTS\"\n\t\t\ttext \"$MV_ENH_HINTS\"\n"
    "\t\t\ttext \"$MV_ENH_FONT\"\n\t\t\ttext \"$MV_ENH_VIDEOS\"\n\t\t\ttext \"$MV_ENH_MODS\"\n\t\t\ttext \"$MV_ENH_CLASSES\"\n"
    "\t\t\ttext \"$MV_ENH_LANGUAGE\"\n\t\t\ttext \"$MV_ENH_OPPONENTS\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos -200 -76\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos -200 -76\n\t\t\t}\n\t}\n"
    /* the note left of CHEATS and BACK (the settings' column is empty there) */
    "\tblock text enhancednote\n\t{\n\t\t\tpos -83 32\n\t\t\tfont text 11\n"
    "\t\t\ttext \"* the next time the game starts\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos -200 32\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos -200 32\n\t\t\t}\n\t}\n"
    "}\n\n"
    /* MORE VIDEO OPTIONS (from VIDEO OPTIONS): the screen, and what the 3D world looks like past the
       game's own lines */
    "block menu enhgfxmenu\n{\n"
    "\tpos -0 -70\n\ttab 50  50  100  50\n\tmaxlines 14\n"
    "\tmp \"DISPLAY\" /select MV_ENH_DISPLAY\n"
    "\tmp \"WIDESCREEN\" /select MV_ENH_WIDESCREEN\n"
    "\tmp \"COLOUR DEPTH\" /select MV_ENH_DEPTH\n"
    "\tmp \"VSYNC\" /select MV_ENH_VSYNC\n"
    "\tmp \"FRAME LIMIT\" /select MV_ENH_FRAMELIMIT\n"
    "\tmp \"ANTI-ALIASING\" /select MV_ENH_AA\n"
    "\tmp \"RENDER SCALE\" /select MV_ENH_RSCALE\n"
    "\tmp \"TEXTURES\" /select MV_ENH_TEXTURES\n"
    "\tmp \"AMBIENT OCCLUSION\" /select MV_ENH_AO\n"
    "\tmp \"DISTANCE FADE\" /select MV_ENH_DISTFADE\n"
    "\tmp \"COLOUR PRECISION\" /select MV_ENH_PRECISION\n"
    "\tmp \"HEADLIGHTS\" /select MV_ENH_HEADLIGHTS\n"
    "\tmp \"BACK\" [ENTER: MA_BACK]\n"
    "\tblock state in\n\t\t{\n\t\tpos 100 -70\n\t\t}\n"
    "\tblock state out\n\t\t{\n\t\tpos 150 -70\n\t\t}\n"
    "\tblock rect menucsik\n\t{\n\t\t\tpos -100 -77\n\t\t\tpri -10\n\t\t\tsize 200 163\n"
    "\t\t\talpha 0 255 255 0\n\t\t\tcolor 0 5 25 42 \n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos 100 -77\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos 150 -77\n\t\t\t}\n\t}\n"
    "\tblock text gfxtitle\n\t{\n\t\t\tjustify right\n\t\t\tpos 90 -92\n\t\t\tfont text 3\n"
    "\t\t\ttext \"MORE VIDEO OPTIONS\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos 150 -92\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos 150 -92\n\t\t\t}\n\t}\n"
    "\tblock text setting\n\t{\n\t\t\tpos -83 -70\n\t\t\tfont text 4\n"
    "\t\t\ttext \"$MV_ENH_DISPLAY\"\n\t\t\ttext \"$MV_ENH_WIDESCREEN\"\n\t\t\ttext \"$MV_ENH_DEPTH\"\n"
    "\t\t\ttext \"$MV_ENH_VSYNC\"\n"
    "\t\t\ttext \"$MV_ENH_FRAMELIMIT\"\n\t\t\ttext \"$MV_ENH_AA\"\n\t\t\ttext \"$MV_ENH_RSCALE\"\n"
    "\t\t\ttext \"$MV_ENH_TEXTURES\"\n\t\t\ttext \"$MV_ENH_AO\"\n\t\t\ttext \"$MV_ENH_DISTFADE\"\n"
    "\t\t\ttext \"$MV_ENH_PRECISION\"\n\t\t\ttext \"$MV_ENH_HEADLIGHTS\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos -200 -70\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos -200 -70\n\t\t\t}\n\t}\n"
    "}\n\n"
    "block menu enhcheatsmenu\n{\n"
    "\tpos -0 -70\n\ttab 50  50  100  50\n\tmaxlines 14\n"
    "\tmp \"ALL CARS\" /select MV_ENH_C_CARS\n"
    "\tmp \"ALL MAPS\" /select MV_ENH_C_MAPS\n"
    "\tmp \"ALL CHAMPIONSHIPS\" /select MV_ENH_C_CHAMPS\n"
    "\tmp \"TRACK GENERATOR\" /select MV_ENH_C_GENERATOR\n"
    "\tmp \"CTRL+W WINS\" /select MV_ENH_C_WIN\n"
    "\tmp \"WHEELS\" /select MV_ENH_C_WHEELS\n"
    "\tmp \"BIG HEADS\" /select MV_ENH_C_HEADS\n"
    "\tmp \"BOXERS\" /select MV_ENH_C_BOXERS\n"
    "\tmp \"GRIP\" /select MV_ENH_C_GRIP\n"
    "\tmp \"GRAVITY\" /select MV_ENH_C_GRAVITY\n"
    "\tmp \"WORLD SIZE\" /select MV_ENH_C_SIZE\n"
    "\tmp \"BACK\" [ENTER: MA_BACK]\n"
    "\tblock state in\n\t\t{\n\t\tpos 100 -70\n\t\t}\n"
    "\tblock state out\n\t\t{\n\t\tpos 150 -70\n\t\t}\n"
    "\tblock rect menucsik\n\t{\n\t\t\tpos -100 -77\n\t\t\tpri -10\n\t\t\tsize 200 150\n"
    "\t\t\talpha 0 255 255 0\n\t\t\tcolor 0 5 25 42 \n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos 100 -77\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos 150 -77\n\t\t\t}\n\t}\n"
    "\tblock text cheatstitle\n\t{\n\t\t\tjustify right\n\t\t\tpos 90 -92\n\t\t\tfont text 3\n"
    "\t\t\ttext \"CHEATS\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos 150 -92\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos 150 -92\n\t\t\t}\n\t}\n"
    "\tblock text setting\n\t{\n\t\t\tpos -83 -70\n\t\t\tfont text 4\n"
    "\t\t\ttext \"$MV_ENH_C_CARS\"\n\t\t\ttext \"$MV_ENH_C_MAPS\"\n\t\t\ttext \"$MV_ENH_C_CHAMPS\"\n"
    "\t\t\ttext \"$MV_ENH_C_GENERATOR\"\n\t\t\ttext \"$MV_ENH_C_WIN\"\n\t\t\ttext \"$MV_ENH_C_WHEELS\"\n"
    "\t\t\ttext \"$MV_ENH_C_HEADS\"\n\t\t\ttext \"$MV_ENH_C_BOXERS\"\n\t\t\ttext \"$MV_ENH_C_GRIP\"\n"
    "\t\t\ttext \"$MV_ENH_C_GRAVITY\"\n\t\t\ttext \"$MV_ENH_C_SIZE\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos -200 -70\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos -200 -70\n\t\t\t}\n\t}\n"
    "}\n\n";

typedef struct { char *p; size_t n, cap; } Buf;
static void put(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

/* the start of the line containing `what` inside the top-level block starting `block` */
static const char *block_end(const char *text, const char *block);
static const char *find_in_block(const char *text, const char *block, const char *what)
{
    const char *b = strstr(text, block), *e = b ? block_end(text, block) : NULL;
    const char *w = b ? strstr(b, what) : NULL;
    if (!w || !e || w > e) return NULL;
    while (w > text && w[-1] != '\n') w--;
    return w;
}

/* the closing brace of a top-level block (the last '}' before the next unindented line) */
static const char *block_end(const char *text, const char *block)
{
    const char *b = strstr(text, block);
    if (!b) return NULL;
    const char *p = strchr(b, '\n');
    while (p && p[1] && (p[1] == '\t' || p[1] == ' ' || p[1] == '{' || p[1] == '}' || p[1] == '\r' ||
                         p[1] == '\n' || p[1] == ';'))
        p = strchr(p + 1, '\n');
    if (!p) return NULL;
    while (p > b && *p != '}') p--;
    return p > b ? p : NULL;
}

/* the line after the one containing `at` */
static const char *next_line(const char *at)
{
    const char *n = at ? strchr(at, '\n') : NULL;
    return n ? n + 1 : NULL;
}

/* the "end" line closing the script */
static const char *script_end(const char *in)
{
    const char *end = NULL;
    for (const char *p = strstr(in, "\nend"); p; p = strstr(p + 1, "\nend"))
        if (p[4] == '\r' || p[4] == '\n' || p[4] == 0) end = p + 1;
    return end;
}

typedef struct { const char *at, *text; } Edit;     /* insert text before `at` */

static int edit_cmp(const void *a, const void *b)
{
    const Edit *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}

static char *apply_edits(const char *in, Edit *e, int n, const char *what)
{
    for (int k = 0; k < n; k++)
        if (!e[k].at) {
            LOG(LOG_ALWAYS, "enhanced: %s is not as expected (edit %d); Enhanced options left out of it", what, k);
            return NULL;
        }
    qsort(e, (size_t)n, sizeof *e, edit_cmp);
    Buf b = {0};
    const char *p = in;
    for (int k = 0; k < n; k++) {
        put(&b, p, (size_t)(e[k].at - p));
        put(&b, e[k].text, strlen(e[k].text));
        p = e[k].at;
    }
    put(&b, p, strlen(p));
    return b.p;
}

static const char video_info[] =
    /* what Vulkan draws at and on, left of MORE VIDEO OPTIONS and BACK (the settings' column is empty
       there) */
    "\tblock text renderinfo\n\t{\n\t\t\tpos -83 64\n\t\t\tfont text 11\n"
    "\t\t\ttext \"$MV_ENH_INFO\"\n\t\t\ttext \"$MV_ENH_INFO2\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos -200 64\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos -200 64\n\t\t\t}\n\t}\n"
    "\tblock text videonote\n\t{\n\t\t\tpos -83 -91\n\t\t\tfont text 11\n"
    "\t\t\ttext \"* at the next race\"\n"
    "\t\tblock state in\n\t\t\t{\n\t\t\tpos -200 -91\n\t\t\t}\n"
    "\t\tblock state out\n\t\t\t{\n\t\t\tpos -200 -91\n\t\t\t}\n\t}\n";

/* `in` with each `from` between `lo` and `hi` (offsets into it; hi < 0: to its end) made `to`; frees in */
static char *subst(char *in, const char *from, const char *to, size_t lo, long hi)
{
    if (!in)
        return NULL;
    size_t end = hi < 0 ? strlen(in) : (size_t)hi, nf = strlen(from);
    Buf b = {0};
    put(&b, in, lo);
    const char *p = in + lo;
    for (const char *q; (q = strstr(p, from)) && (size_t)(q - in) + nf <= end; p = q + nf) {
        put(&b, p, (size_t)(q - p));
        put(&b, to, strlen(to));
    }
    put(&b, p, strlen(p));
    free(in);
    return b.p;
}

/* the same, within a top-level block */
static char *subst_block(char *in, const char *block, const char *from, const char *to)
{
    const char *b = in ? strstr(in, block) : NULL, *e = b ? block_end(in, block) : NULL;
    if (!b || !e)
        return in;
    return subst(in, from, to, (size_t)(b - in), e - in);
}

/* the game's lines with our steps (merges): their words extended (patch_text), REFLECTIONS for ENV.
   MAPPING */
static char *merged_words(char *s)
{
    if (!words_ok())
        return s;
    s = subst(s, "\"$MV_VID_SHADOWS:shadow\"", "\"$MV_VID_SHADOWS:shadowrt\"", 0, -1);
    s = subst(s, "\"$MV_VID_ENVMAP:shadow\"", "\"$MV_VID_ENVMAP:reflrt\"", 0, -1);
    s = subst(s, "\"$MV_VID_SMOKES:onoff\"", "\"$MV_VID_SMOKES:smokevol\"", 0, -1);
    return subst(s, "mp \"ENV. MAPPING\" /select MV_VID_ENVMAP", "mp \"REFLECTIONS\" /select MV_VID_ENVMAP", 0, -1);
}

static char *patch_menu(const char *in)
{
    const char *vis_mode = find_in_block(in, "block menu videoopmenu", "/select MV_VID_VISIBILITYRANGE");
    const char *vis_text = find_in_block(in, "block menu videoopmenu", "\"$MV_VID_VISIBILITYRANGE");
    Edit e[] = {
        {find_in_block(in, "block menu optionsmenu", "[ENTER: MA_BACK]"),
         "\tmp \"ENHANCED OPTIONS\" [ENTER: optionsmenu:out mainscr:out fomenucsik:out al10scr:in "
         "enhancedmenu:in enhancedmenu:run]\r\n"},
        {next_line(vis_mode), "\tmp \"DRAW DISTANCE *\" /select MV_ENH_DRAWDIST\r\n"},
        {next_line(vis_text), "\t\t\ttext \"$MV_ENH_DRAWDIST\"\r\n"},
        {find_in_block(in, "block menu videoopmenu", "[ENTER: MA_BACK]"),
         "\tmp \"MORE VIDEO OPTIONS\" [ENTER: videoopmenu:out enhgfxmenu:in enhgfxmenu:run]\r\n"},
        {block_end(in, "block menu videoopmenu"), video_info},
        {script_end(in), page},
    };
    char *out = apply_edits(in, e, 6, "the menu script");
    /* VIDEO OPTIONS, a line longer: up by 10 (the menu, its panel, its settings' column), the panel
       taller */
    static const char *const up[][2] = {{"pos -0 -70", "pos -0 -80"}, {"pos 100 -70", "pos 100 -80"},
                                        {"pos 150 -70", "pos 150 -80"}, {"pos -100 -77", "pos -100 -87"},
                                        {"size 200 150", "size 200 170"}, {"pos 100 -77", "pos 100 -87"},
                                        {"pos 150 -77", "pos 150 -87"}, {"pos -83 -70", "pos -83 -80"},
                                        {"pos -200 -70", "pos -200 -80"}};
    for (int k = 0; k < (int)(sizeof up / sizeof up[0]); k++)
        out = subst_block(out, "block menu videoopmenu", up[k][0], up[k][1]);
    return merged_words(out);
}

/* The pause menu during a race, laid out as the main menu's (Ant, 2026-10-19: "the in-game options
   menu seems to be buggy with what video options work and how they're structured"): VIDEO OPTIONS
   (the game's lines, the merged ones going on into ours) ends in MORE VIDEO OPTIONS, the main
   menu's page of that name with what can change mid-race (all of it but COLOUR DEPTH), in its
   order; ENHANCED OPTIONS keeps the rest that can (rumble, prompts, hints, font). DRAW DISTANCE and the
   others marked * apply at the next race and stay on the main menu's pages. */
#define INGAME_PAGE(name, lines, texts, n)                                                          \
    "block menu " name "\n{\n"                                                                      \
    "\tpos 5 -70\n" lines "\tmp \"BACK\" [ENTER: MA_BACK]\n"                                        \
    "\tblock state in\n\t{\n\t\tpos 75 -70\n\t}\n"                                                  \
    "\tblock state out\n\t{\n\t\tpos 80 -70\n\t}\n"                                                 \
    "\tblock rect\n\t{\n\t\tpos -100 -77\n\t\tsize  200 " n "\n\t\tpri -30\n\t\tcolor 0 0 0 0\n"     \
    "\t\talpha 0 255 255 0\n\t\talpha 255\n"                                                        \
    "\t\tblock state in\n\t\t{\n\t\t\tpos 75 -77\n\t\t\talpha 0\n\t\t}\n"                            \
    "\t\tblock state out\n\t\t{\n\t\t\tpos 80 -77\n\t\t\talpha 0\n\t\t}\n"                           \
    "\tblock text\n\t{\n\t\tpos -80 -70\n" texts                                                    \
    "\t\tblock state in\n\t\t{\n\t\t\tpos -135 -70\n\t\t}\n"                                        \
    "\t\tblock state out\n\t\t{\n\t\t\tpos -140 -70\n\t\t}\n\t}\n"                                   \
    "}\n}\n\n"

static const char page_ingame[] =
    "\n;;;;;;;;;; 1NSANE native: Enhanced options\n"
    INGAME_PAGE("enhvideomenu",
                "\tmp \"DISPLAY\" /select MV_ENH_DISPLAY\n"
                "\tmp \"WIDESCREEN\" /select MV_ENH_WIDESCREEN\n"
                "\tmp \"VSYNC\" /select MV_ENH_VSYNC\n"
                "\tmp \"FRAME LIMIT\" /select MV_ENH_FRAMELIMIT\n"
                "\tmp \"ANTI-ALIASING\" /select MV_ENH_AA\n"
                "\tmp \"RENDER SCALE\" /select MV_ENH_RSCALE\n"
                "\tmp \"TEXTURES\" /select MV_ENH_TEXTURES\n"
                "\tmp \"AMBIENT OCCLUSION\" /select MV_ENH_AO\n"
                "\tmp \"DISTANCE FADE\" /select MV_ENH_DISTFADE\n"
                "\tmp \"COLOUR PRECISION\" /select MV_ENH_PRECISION\n"
                "\tmp \"HEADLIGHTS\" /select MV_ENH_HEADLIGHTS\n",
                "\t\ttext \"$MV_ENH_DISPLAY\"\n\t\ttext \"$MV_ENH_WIDESCREEN\"\n\t\ttext \"$MV_ENH_VSYNC\"\n"
                "\t\ttext \"$MV_ENH_FRAMELIMIT\"\n\t\ttext \"$MV_ENH_AA\"\n\t\ttext \"$MV_ENH_RSCALE\"\n"
                "\t\ttext \"$MV_ENH_TEXTURES\"\n\t\ttext \"$MV_ENH_AO\"\n\t\ttext \"$MV_ENH_DISTFADE\"\n"
                "\t\ttext \"$MV_ENH_PRECISION\"\n\t\ttext \"$MV_ENH_HEADLIGHTS\"\n",
                "141")
    INGAME_PAGE("enhancedmenu",
                "\tmp \"RUMBLE\" /select MV_ENH_RUMBLE\n"
                "\tmp \"BUTTON PROMPTS\" /select MV_ENH_PROMPTS\n"
                "\tmp \"CONTROL HINTS\" /select MV_ENH_HINTS\n"
                "\tmp \"FONT\" /select MV_ENH_FONT\n",
                "\t\ttext \"$MV_ENH_RUMBLE\"\n\t\ttext \"$MV_ENH_PROMPTS\"\n\t\ttext \"$MV_ENH_HINTS\"\n"
                "\t\ttext \"$MV_ENH_FONT\"\n",
                "57");

static char *patch_ingame(const char *in)
{
    const char *menu = strstr(in, "block menu mainmenu");
    Edit e[] = {
        {next_line(find_in_block(in, "block menu mainmenu", "displaymenu:run")),
         "\tmp \"ENHANCED OPTIONS\" [ENTER: mainmenu:out enhancedmenu:run]\r\n"},
        {next_line(next_line(menu)), "\tmaxlines 12\r\n"},
        {find_in_block(in, "block menu videomenu", "mp \"BACK\""),
         "\tmp \"MORE VIDEO OPTIONS\" [ENTER: videomenu:out enhvideomenu:run]\r\n"},
        {script_end(in), page_ingame},
    };
    char *out = apply_edits(in, e, 4, "the in-race menu script");
    out = subst_block(out, "block menu videomenu", "size  200 117", "size  200 129");    /* a line more */
    return merged_words(out);
}

/* menu_text.cfg's arrays (the language's words for the menus' values): for the merged lines, the
   game's words with ours after them */
static char *array_more(const char *in, const char *name, const char *ours, const char *extra)
{
    char key[40];
    snprintf(key, sizeof key, "\narray %s ", name);
    const char *p = strstr(in, key);
    if (!p)
        return NULL;
    p += strlen(key);
    size_t n = strcspn(p, "\r\n;");
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
        n--;
    size_t len = strlen(ours) + n + strlen(extra) + 16;
    char *s = malloc(len);
    snprintf(s, len, "array %s %.*s %s\r\n", ours, (int)n, p, extra);
    return s;
}

static char *patch_text(const char *in);
static char *idf_read(const char *want, uint32_t *size);

/* the merged lines' words can be had (the language's menu_text.cfg as expected): else the menus keep the
   game's words and the merged lines the game's steps */
static int words_ok(void)
{
    static int ok = -1;
    if (ok < 0) {
        uint32_t size = 0;
        char *in = idf_read("data\\menu_text.cfg", &size), *out = in ? patch_text(in) : NULL;
        ok = out != NULL;
        free(in);
        free(out);
    }
    return ok;
}

static char *patch_text(const char *in)
{
    char *a = array_more(in, "shadow", "shadowrt", "\"RAY TRACED\""),
         *b = array_more(in, "shadow", "reflrt", "\"RAY TRACED\" \"RAY TRACED FULL\""),   /* LIGHT, FULL */
         *v = array_more(in, "onoff", "smokevol", "\"VOLUMETRIC\"");
    const char *at = strstr(in, "\narray shadow ");
    char *out = NULL;
    if (a && b && v && at) {
        Edit e[] = {{next_line(at + 1), a}, {next_line(at + 1), b}, {next_line(at + 1), v}};
        out = apply_edits(in, e, 3, "the menus' words");
    } else
        LOG(LOG_ALWAYS, "enhanced: menu_text.cfg is not as expected; the merged lines show the game's words only");
    free(a);
    free(b);
    free(v);
    return out;
}

/* Read an archive member the way the game would find it: its directory entries are
   {name[52], archive, offset, size} (64 bytes), the archive names 100 bytes each. */
static char *idf_read(const char *want, uint32_t *size)
{
    uint32_t n = R32(IDF_COUNT);
    for (uint32_t i = 0; i < n && i < limit_idf_entries; i++) {
        uint32_t e = reloc_idf_entries + 64 * i;
        if (strcasecmp((const char *)gptr(e), want)) continue;
        const char *arch = (const char *)gptr(reloc_idf_archives + 100 * R32(e + 0x34));
        char host[1200];
        if (!path_to_host(arch, host, sizeof host, 0)) return NULL;
        FILE *f = fopen(host, "rb");
        if (!f) return NULL;
        uint32_t off = R32(e + 0x38), sz = R32(e + 0x3c);
        char *buf = malloc(sz + 1);
        int ok = fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, sz, f) == sz;
        fclose(f);
        if (!ok) { free(buf); return NULL; }
        buf[sz] = 0;
        *size = sz;
        return buf;
    }
    return NULL;
}

static struct {
    const char *file;                   /* the script, as the game names it */
    char *(*patch)(const char *in);
    uint32_t name;                      /* "cache\<file>" in program memory, once written */
    int tried;
} scripts[] = {{"menu.cfg", patch_menu, 0, 0}, {"menu_ingame.cfg", patch_ingame, 0, 0},
               {"menu_text.cfg", patch_text, 0, 0}};

/* 0x44c110(name, &size): the game's file opener (archive members first, then loose files) */
static int hook_open(CPU *c)
{
    uint32_t a = R32(c->esp + 4);
    if (!a) return 0;
    const char *name = (const char *)gptr(a);
    const char *base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    for (int k = 0; k < (int)(sizeof scripts / sizeof scripts[0]); k++) {
        if (strcasecmp(base, scripts[k].file)) continue;
        if (!scripts[k].tried) {
            scripts[k].tried = 1;
            uint32_t size = 0;
            char *orig = idf_read(name, &size), *out = orig ? scripts[k].patch(orig) : NULL;
            char path[1300];
            snprintf(path, sizeof path, "%s/cache", user_dir);
            mkdir(path, 0755);
            snprintf(path, sizeof path, "%s/cache/%s", user_dir, scripts[k].file);
            FILE *f = out ? fopen(path, "wb") : NULL;
            if (f) {
                int ok = fwrite(out, 1, strlen(out), f) == strlen(out);
                if (fclose(f) == 0 && ok) {
                    scripts[k].name = galloc(64);
                    char win[64];
                    snprintf(win, sizeof win, "cache\\%s", scripts[k].file);
                    gstrncpy(scripts[k].name, win, 64);
                    LOG(LOG_FILE, "enhanced: %s patched into %s", name, path);
                }
            }
            free(orig);
            free(out);
        }
        if (scripts[k].name)
            W32(c->esp + 4, scripts[k].name);
        return 0;
    }
    return 0;
}

/* MA_WHEEL_TO_2 (0x47cd50): the main menu's wheel turned to MULTI PLAYER. The game's own turns
   it there only with network connection types to offer (0x1ca6ba0, DirectPlay's count, none
   here): else it skips that stop, from SINGLE PLAYER's to OPTIONS' and back, while the highlight
   does not, so the wheel was a stop off past MULTI PLAYER (Ant: "the menu wheel doesn't spin
   with the options properly"). Ours turns it there always, as the game does with one. */
METHOD(wheel_to_multi, 0)
{
    W32(MENU_WHEEL, 1);
    return 0;
}

/* MO_WHEEL (0x471450(block, holder, op), op 1 each frame): the main menu's wheel at its menu
   pose (the block's +0x320: +0x8c its turn, radians, +0x94 its roll) plus its spin to the stop
   chosen. On a 4:3 screen its left side is off the screen's edge and the stop's name is the one
   on its right, by the menu; widescreen widens its camera round the middle, so all of it shows,
   with the next stop's name the biggest (Ant: "use rotation to change which text is closest to
   the user, maybe center it on the bottom"). With the view widened it is rolled WHEEL_TURN on,
   so the chosen stop's name sits at its bottom, for that call only (the menu's own pose is left
   as it was). */
#ifndef WHEEL_TURN_DEG
#define WHEEL_TURN_DEG -45.0f
#endif
#define WHEEL_TURN  (WHEEL_TURN_DEG * (float)M_PI / 180.0f)
METHOD(wheel_draw, 0)
{
    uint32_t args[4];
    for (int k = 0; k < 4; k++)
        args[k] = R32(c->esp + 4 + 4 * (uint32_t)k);
    uint32_t pose = args[2] == 1 && args[0] && ws_aspect > 4.0f / 3.0f + 0.001f ? R32(args[0] + 0x320) : 0;
    if (!pose)
        return guest_callv(c, MO_WHEEL_FN, 4, args);
    uint32_t keep = R32(pose + 0x94), u;
    float roll;
    memcpy(&roll, &keep, 4);
    roll += WHEEL_TURN;
    memcpy(&u, &roll, 4);
    W32(pose + 0x94, u);
    uint32_t r = guest_callv(c, MO_WHEEL_FN, 4, args);
    W32(pose + 0x94, keep);
    return r;
}

void enhanced_init(void)
{
    enabled = !deterministic || getenv("INSANE_ENHANCED");
    if (!enabled) return;
    if (R32(WHEEL_TO_2_FN) == 0x47cd50)
        W32(WHEEL_TO_2_FN, thunk_addr(wheel_to_multi, "MA_WHEEL_TO_2"));
    if (R32(MO_WHEEL_SLOT) == MO_WHEEL_FN)
        W32(MO_WHEEL_SLOT, thunk_addr(wheel_draw, "MO_WHEEL"));
    hook_idf_open = hook_open;
    hook_menu_vars = hook_vars;
    hook_game_start = hook_game_start_fn;
    cheats_apply(NULL, 0);              /* unlocks and cheats from the start */
}

/* apply changed options in every part that has one */
void options_changed(void)
{
    vk_options();
    widescreen_options();
    win_options();
    drawdist_options();
    opponents_options();
    shadows_options();
    smoke_options();
    headlights_options();
    frame_limit_options();
    font_options();
}
