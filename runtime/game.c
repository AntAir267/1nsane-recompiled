/* game.c: 1NSANE's hooks into the AntiStatic runtime core (third_party/antistatic,
   runtime/win32/main.c): its start-up, its per-frame work and its exit, in the order 1NSANE's own
   main.c (before the move, master 13812ba) ran them. */
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "font.h"
#include "widescreen.h"
#include "mods.h"
#include "padgame.h"
#include "pad.h"
#include "vk.h"
#include "drawdist.h"

void ddraw_init(void);
void vk_stop(void);
void drawdist_init(void);
void shadows_init(void);
void headlights_init(void);
void fancars_init(void);
void smoke_init(void);
void taa_init(void);
void opponents_init(void);
void enhanced_init(void);
void opponents_menus_init(void);
void classes_menus_init(void);
void camera_init(void);

/* The display layer, between pad_init and dsound_init as before. The fan content (mods_init) ran
   before settings_load there and the controllers' game layer (padgame_init) before ddraw_init;
   both only need files_init and pad_init, which the core has run by now. */
void port_graphics_init(void)
{
    mods_init();
    padgame_init();
    ddraw_init();
}

/* after dsound_init and dinput_init, before the program starts: the game's own set-up */
void game_init(void)
{
    /* the game's language (00: English), as its installer leaves it; 1NSANE's settings_load
       put it in when registry.txt had none */
    if (!settings_has("HKLM\\Software\\Codemasters\\Insane", "LangID"))
        settings_put("HKLM\\Software\\Codemasters\\Insane", "LangID", 1, "00", 3);
    ifc_init();
    video_init();
    widescreen_init();
    drawdist_init();
    shadows_init();                     /* after drawdist_init: chains to its hooks */
    headlights_init();
    font_init();
    fancars_init();                     /* after shadows_init: chains to its car shadow hook */
    smoke_init();                       /* after shadows_init, before taa_init: between their views */
    taa_init();                         /* after shadows_init and smoke_init: chains to their hooks */
    opponents_init();
    enhanced_init();
    opponents_menus_init();             /* after the Enhanced options: adds to their file and menu hooks */
    classes_menus_init();               /* CAR CLASSES REORGANIZED: after opponents.c's, before the prompts' */
    prompts_init();                     /* after the Enhanced options: adds to their file and menu hooks */
    camera_init();                      /* Enhanced START CAMERA */
}

/* options.txt: the comment written above each of 1NSANE's keys the first time it is saved (the
   toolkit has display, rumble and frame_limit's) */
const char *port_option_help(const char *key)
{
    static const char *const help[][2] = {
        {"resolution", "resolution: original (the game's display mode, scaled up) or native (the screen's own)"},
        {"widescreen", "widescreen: off (4:3 as shipped), on (match the screen), or an aspect such as 16:9"},
        {"vsync", "vsync: on (frames wait for the screen) or off"},
        {"antialiasing", "antialiasing: off, 4x (drawn at twice the size each way, then scaled down), fsr (AMD's FSR 2), "
                         "dlss (NVIDIA RTX) or taa (the port's own temporal upscaler: lighter than FSR 2, softer)"},
        {"textures", "textures: original or smooth (trilinear and anisotropic filtering on the 3D world)"},
        {"videos", "videos: on or off (the intro videos)"},
        {"mods", "mods: on or off (the fan cars and maps listed in mods.txt; at the next start)"},
        {"render_scale", "render_scale: native, quality, balanced, performance or auto (with FSR or DLSS the 3D world is drawn "
                         "smaller and upscaled; auto picks the size that keeps the frame rate: frame_limit's, else the screen's)"},
        {"headlights", "headlights: off, on (on night maps the cars' beams light the road ahead, with shadows; needs ray "
                       "tracing) or volumetric (the beams seen in the air too, and in smoke and dust)"},
        {"font", "font: original (the game's picture of its letters, stretched), 1nsanefont (drawn sharp from 1nsaneFont, made "
                 "to match the game's letters) or mirabel (drawn sharp from Mirabel Gothish, which 1nsaneFont is made from)"},
        {NULL, NULL}};
    for (int h = 0; help[h][0]; h++)
        if (!strcasecmp(help[h][0], key))
            return help[h][1];
    return NULL;
}

/* each presented frame */
void game_frame(unsigned long frame)
{
    (void)frame;
    font_frame();                       /* (font.c: marks the game's picture of letters) */
}

/* the controllers' frame (pad.c's pad_frame, from input_frame): the game's controls from them */
void port_pad_frame(unsigned long frame)
{
    padgame_frame(frame);
}

/* The joystick 1NSANE's game sees (its "XBOX CONTROLLER" layout and saved Data\N.ctl layouts
   number them so; 1NSANE's pad.c before the move): axes X/Y the left stick, Z the left trigger,
   Rx the right trigger (each 0 at rest .. 32767 pulled), Ry/Rz the right stick; the D-pad as POV 0;
   buttons in SDL's order without the four D-pad ones: 0 A, 1 B, 2 X, 3 Y, 4 Back, 5 Guide, 6 Start,
   7 left stick, 8 right stick, 9 LB, 10 RB, 11 Misc, 12-15 paddles. The toolkit's state comes
   merged and with the sticks' dead zone applied. Any button going down skips an intro video. */
int port_pad_send(int joy, const PadState *st)
{
    static uint8_t held[INPUT_MAX_JOYS][PAD_BUTTONS];
    input_joy_axis(joy, INPUT_AXIS_X, st->axis[PAD_LX]);
    input_joy_axis(joy, INPUT_AXIS_Y, st->axis[PAD_LY]);
    input_joy_axis(joy, INPUT_AXIS_RY, st->axis[PAD_RX]);
    input_joy_axis(joy, INPUT_AXIS_RZ, st->axis[PAD_RY]);
    input_joy_axis(joy, INPUT_AXIS_Z, st->axis[PAD_LT]);
    input_joy_axis(joy, INPUT_AXIS_RX, st->axis[PAD_RT]);
    int pressed = 0;
    for (int b = 0; b < PAD_BUTTONS; b++) {
        if (joy >= 0 && joy < INPUT_MAX_JOYS) {
            pressed |= st->button[b] && !held[joy][b];
            held[joy][b] = st->button[b];
        }
        if (b < PAD_DUP)
            input_joy_button(joy, b, st->button[b]);
        else if (b > PAD_DRIGHT && b - 4 < 16)
            input_joy_button(joy, b - 4, st->button[b]);
    }
    int up = st->button[PAD_DUP], dn = st->button[PAD_DDOWN], l = st->button[PAD_DLEFT], r = st->button[PAD_DRIGHT];
    int v = INPUT_POV_CENTERED;
    if (up && r) v = 4500; else if (r && dn) v = 13500;
    else if (dn && l) v = 22500; else if (l && up) v = 31500;
    else if (up) v = 0; else if (r) v = 9000; else if (dn) v = 18000; else if (l) v = 27000;
    input_joy_pov(joy, 0, v);
    if (pressed)
        video_skip_press();             /* (any button skips an intro video: video.c) */
    return 1;
}

/* at exit, before the core's ifc_exit (ifc21.c: the force-feedback motors off) and
   settings_save: the render thread done first, then the controllers' game layer */
void port_at_exit(void)
{
    vk_stop();
    padgame_exit();
}

/* Fan cars and maps (mods.c) where the game folder has none: an archive or a fan car's loose
   files ("Data/Vehicles/x.idf", "Data/Vehicles/x/..."), extracted once into the cache */
int port_file_extra(const char *rel, char *out, size_t n)
{
    return mods_file(rel, out, n);
}

/* ... and their names in the game's Data\Vehicles and Data\Maps listings (after the game's own,
   the first spelling winning; their sizes then through port_file_extra) */
int port_find_extra(const char *rel, char (*names)[64], int max)
{
    if (!mods_active)
        return 0;
    int kind = !strcasecmp(rel, "Data/Vehicles") ? 0 : !strcasecmp(rel, "Data/Maps") ? 1 : -1;
    return kind < 0 ? 0 : mods_list(kind, names, max);
}

/* GetAsyncKeyState: Escape reads as down once after a tap or a controller button that skips an
   intro video (video.c's latch: the game looks once per video frame, a tap may fall between) */
int port_async_key(int vk)
{
    if (vk == 0x1b && video_skip_taken())   /* VK_ESCAPE */
        return 0x8000;
    return -1;
}

/* a surface about to go, its pixels still there: drawdist.c keeps copies for its far tiles */
void port_d3d7_surface_gone(const Surface *s)
{
    drawdist_surface_gone(s);
}
