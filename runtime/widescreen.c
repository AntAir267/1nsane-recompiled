/* Widescreen (Enhanced option, design point 1): "widescreen = on" (match the screen) or an
   aspect ratio such as 16:9.

   The game builds every camera with one function (0x42a350: matrix, -1, -1, 1, 1, near, far,
   horizontal field of view in degrees, aspect = y scale / x scale, always 4:3). Each camera
   gets a wider horizontal view with the same vertical one, so the game itself draws and culls
   the wider world (no pop-in at the sides). The renderer widens those cameras' viewports to
   match: a full-screen view spans the whole screen, a small one (checkpoint arrow) widens
   around its centre; a menu's background (the flat textured tiles each frame starts with)
   stretches across the screen; everything else (HUD, text, panels, 2D pictures) keeps its 4:3
   shape in the middle. Off by default: the game's memory then never changes. */
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "widescreen.h"
#include "drawdist.h"
#include "funcs.h"

static int ws_on;
static float ws_want;            /* requested aspect, 0 = follow the screen */
float ws_aspect = 4.0f / 3.0f;   /* current target aspect (width / height) */
float ws_world_aspect;           /* y/x scale the widened camera's projection has */
static int ws_log, allowed;

static float rdf(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }

/* hook at 0x42a350's entry: [esp] return, [esp+4] matrix, +8 x, +12 y, +16 w, +20 h,
   +24 near, +28 far, +32 fov, +36 aspect */
static int perspective_hook(CPU *c)
{
    uint32_t a = c->esp;
    float x = rdf(a + 8), y = rdf(a + 12), w = rdf(a + 16), h = rdf(a + 20);
    float fov = rdf(a + 32), asp = rdf(a + 36);
    if (ws_log)
        LOG(LOG_ALWAYS, "perspective(near %g, far %g, fov %g, aspect %g) at frame %lu", rdf(a + 24), rdf(a + 28),
            fov, asp, frame_count);
    (void)x; (void)y; (void)w; (void)h;
    drawdist_camera(a);                     /* a longer view's far plane (drawdist.c) */
    if (!ws_on || ws_aspect <= 4.0f / 3.0f + 0.001f)
        return 0;
    /* every 4:3 camera (menus' and races'): the renderer widens its viewport to match */
    if (fabsf(asp - 4.0f / 3.0f) > 0.001f)
        return 0;
    float k = ws_aspect / (4.0f / 3.0f);
    float half = fov * (float)M_PI / 360.0f;
    float fov2 = 2.0f * atanf(tanf(half) * k) * 360.0f / (2.0f * (float)M_PI);
    wrf(a + 32, fov2);
    wrf(a + 36, asp * k);
    ws_world_aspect = asp * k;
    return 0;
}

static void parse(void)
{
    const char *v = option_str("widescreen", "off");
    ws_want = 0;
    if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcmp(v, "0")) {
        ws_on = 0;
    } else if (!strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcmp(v, "1")) {
        ws_on = 1;
    } else {
        float a = 0, b = 0;
        if (sscanf(v, "%f:%f", &a, &b) == 2 && a > 0 && b > 0) ws_want = a / b;
        else if (sscanf(v, "%f", &a) == 1 && a > 1.0f) ws_want = a;
        ws_on = ws_want > 0;
        if (!ws_on)
            LOG(LOG_ALWAYS, "options: widescreen \"%s\" is not on, off or an aspect like 16:9", v);
    }
    ws_aspect = ws_on && ws_want > 0 ? ws_want : 4.0f / 3.0f;
}

/* Where the renderer is in a race frame, for the HUD placement (vk.c): inside the HUD routines
   pieces go to the screen edge they sit nearest; other flat drawings of a race frame (the name
   labels over the cars) follow the widened view; menus keep their layout. */
int ws_race_depth, ws_hud_depth, ws_menu_depth;
#define WRAP(name, counter, fn) \
    static int name(CPU *c) { counter++; fn##_orig(c); counter--; return 1; }
WRAP(race_frame_hook, ws_race_depth, F_004661c0)
WRAP(race_dashboard_hook, ws_hud_depth, F_00462c10)
WRAP(race_hud_hook, ws_hud_depth, F_00463bd0)
WRAP(menu_draw_hook, ws_menu_depth, F_004759b0)
WRAP(menu_anim_hook, ws_menu_depth, F_00474cc0)

void widescreen_init(void)
{
    ws_log = getenv("INSANE_WS_LOG") != NULL;
    allowed = !deterministic || getenv("INSANE_ENHANCED");
    parse();
    if (ws_on && !allowed) {
        LOG(LOG_ALWAYS, "widescreen is off in --test runs (it changes the game's camera)");
        ws_on = 0;
    }
    if (allowed || ws_log)
        hook_perspective = perspective_hook;   /* also when off: the menu can turn it on */
    if (allowed) {
        hook_race_frame = race_frame_hook;
        hook_race_dashboard = race_dashboard_hook;
        hook_race_hud = race_hud_hook;
        hook_menu_draw = menu_draw_hook;
        hook_menu_anim = menu_anim_hook;
    }
}

/* the Enhanced page changed the option */
void widescreen_options(void)
{
    if (allowed) parse();
}

int widescreen_on(void) { return ws_on; }

/* The renderer tells us the shape of the picture area (window or screen). */
void widescreen_screen(int w, int h)
{
    if (!ws_on || ws_want > 0 || w <= 0 || h <= 0)
        return;
    float a = (float)w / (float)h;
    ws_aspect = a > 4.0f / 3.0f ? a : 4.0f / 3.0f;
}
