/* Ray-traced sun shadows (Enhanced option "shadows": original | raytraced; DESIGN.md decision 4,
   "Ray-traced sun shadows").

   The game lights its world with one directional light, the sun (0x44c510: suncolor, ambient,
   direction from the map's .scn), and draws its own shadows: each car's is painted by a software
   rasterizer into a texture every race frame (0x465b60 -> 0x41a6b0 -> 0x416090) and laid on the
   ground with the other objects' shadow shapes (0x41a670 -> 0x419500 cars, 0x419eb0 objects),
   black at alpha 3/4 of the object light map there (a 256 x 256 byte per cell map the load-time
   bake leaves, *0x4c3070: 0 in the hills' shadows, 255 in full sun) times a distance fade. The
   terrain's colour map has the sun baked in (slope shading, the hills' shadows), and objects take
   the object light map's value at their place as the sun's strength (0x429980).

   RAY TRACED: in the race's main view (0x465950 from the race frame) the renderer finds, for
   every pixel, the part of its colour the sun gives (vertex-lit draws: their lighting without the
   sun's diffuse and specular; the terrain: what the game's own shadow takes there, from the object
   light map) and takes it away where a ray towards the sun meets something: the frame's objects
   and cars as drawn (cut-outs such as leaves, lattices and fences by their textures' alpha), and
   for objects and cars the terrain too (a mesh of the map's heights; the hills' shadows on the
   terrain itself are in its colour map already). The game's own shadows are left out then (their
   painting and drawing skipped), so nothing is doubled and the CPU is spared the painting.
   ORIGINAL changes nothing. Off in --test runs unless INSANE_ENHANCED is set. INSANE_SH_LOG=1 logs
   the set-ups.

   AMBIENT OCCLUSION (Enhanced option "ambient_occlusion": off | light | on, which is FULL; "full"
   too; DESIGN.md decision 4, "Ray-traced ambient occlusion") works on the same views, structures
   and attachments: short rays from every surface over its hemisphere darken the light that does
   not come from the sun where things meet (under and round the cars, at the feet of trees, posts
   and buildings, in corners and under overhangs), whether the shadows are ray traced or the
   game's own (then its shadow shapes and the cars' painted shadows stay). LIGHT does the same
   with fewer, shorter rays into a smaller image and a cheaper applying. OFF changes nothing.
   INSANE_AO_RADIUS (game units), INSANE_AO_STRENGTH, _TERRAIN, _FALLOFF, _FAR (game units) and
   _FOLIAGE override the tuning below (FULL's).

   REFLECTIONS (Enhanced option "reflections": original | light | raytraced, which is FULL; "full"
   too; DESIGN.md decision 4, "Ray-traced reflections"): the game adds a reflection to cars and
   some objects with a second draw of their meshes (0x40a770: the map's env texture, SRCALPHA /
   ONE, z bias 1, its strength the model's ENV value times the object light map and a distance
   fade in the vertices' alpha, texture coordinates from the normal and the camera), and the
   water's colour pass shows the sky texture by the view direction. FULL and LIGHT draw those
   without the texture's colour and again after the view with what a ray from the surface along
   the mirror direction meets in place of the texture (vk.c refl_gather, shaders/refl.glsl): the
   same draws' strength, colours, fog and blending, the strength rising towards grazing angles
   (Fresnel); rays that meet nothing show the sky dome (0x41c7a0, hook sky_draw: its draws are
   gathered for them) and the sun's glint. LIGHT takes fewer rays and shows the game's own look
   where they cannot follow what is reflected. ORIGINAL changes nothing. INSANE_REFL_DETAIL and
   _LEN override the tuning below, vk.c's INSANE_REFL_FRESNEL, _DARKEN, _GAIN and _SPREAD the rest
   of it. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "funcs.h"
#include "gfx.h"
#include "vk.h"
#include "shadows.h"
#include "headlights.h"
#include "drawdist.h"
#include "widescreen.h"

/* ------------------------------------------------------------------ game addresses (GOG build) */
#define CAM_X           0xa4ca90u       /* the camera, 16.16 game units */
#define CAM_Y           0xa4ca94u
#define CAM_Z           0xa4ca98u
#define HEIGHTS         0x924400u       /* float[256 * 256], z * 256 + x: .map byte * 16 */
#define OBJ_LIGHT       0x4c3070u       /* -> the object light map: byte[256 * 256], z * 256 + x */
#define LIGHT_DIR       0x1bf2290u      /* the sun's D3DLIGHT7 direction (floats: -(toward the sun) / 65536) */
#define RACE_FAR        20000.0f        /* the race camera's far plane (game units) */

static int allowed, want, want_ao, want_refl, logon;   /* want_ao, want_refl: 0 off, 1 LIGHT, 2 FULL */
static int view_on;                     /* inside a view with ray tracing (shadows, ambient occlusion, reflections) */
static int view_shadows;                /* ... with the sun shadows ray traced */
/* ambient occlusion's tuning: its rays' length (game units: 1 m is about 66.7) and how much a
   fully closed-in pixel's ambient light is darkened on objects and cars, and on the terrain */
static float ao_radius = 150.0f, ao_strength = 2.0f, ao_terrain = 2.0f, ao_falloff = 2.0f;
static float ao_far = 13333.0f;         /* ... out to 200 m, fading from 120 m (and no further than the shadows) */
/* LIGHT: a smaller image (about 267 rows: a pixel per 6 x 6 at 1600 rows), 2 rays, shorter (1.9 m),
   out to 120 m */
static float ao_light_radius = 125.0f, ao_light_far = 8000.0f, ao_light_rows = 267.0f;
static int ao_light_rays = 2;
static float ao_foliage = 0.35f;        /* ... and how much of it foliage takes, */
static float ao_parts = 0.4f;           /* ... and solid see-through parts (lattices, fences); */
static float ao_max = 0.7f;             /* ... the most it takes of a pixel's ambient light (bounce light stays) */
/* the water: how much of its colour is the sun's (per unit of the object light map, as the
   terrain's 3/4), how much ambient occlusion it takes, and how much of its sun part (its own
   and the ground's seen through it) a shadow on it takes: the water scatters light into a
   shadow, so less than on land */
static float water_dark = 0.5f, water_ao = 0.6f, water_shadow = 0.6f;
/* reflections: how far their strength rises from the game's towards 1 at grazing angles (Schlick's
   term times this), how much of the colour below the rise takes away; their rays' length where
   the view has no sky dome (game units: 400 m; else to the dome); what the terrain's detail pass
   leaves of its colour map in them */
static float refl_fresnel = 0.5f, refl_darken = 0.5f, refl_len = 26667.0f, terrain_detail = 0.85f;
static float refl_gain = 0.55f;         /* ... and how bright what they meet is shown: the game's env map,
                                           its stand-in, is about half as bright as the world */
static int (*prev_water)(CPU *);
static int (*prev_world)(CPU *), (*prev_terrain)(CPU *), (*prev_object_shadow)(CPU *), (*prev_terrain_buffers)(CPU *);

/* the race's views are ray traced (the shadows, ambient occlusion or reflections asked for, and the
   GPU can) */
static int active(void)
{
    return allowed && (want || want_ao || want_refl || headlights_on()) && vk_rt_available();
}

/* ... with the sun shadows ray traced (the game's own left out) */
static int shadows_rt(void)
{
    return allowed && want && vk_rt_available();
}

/* ------------------------------------------------------------------ the map */
/* the map's heights as a mesh for the rays: 257 x 257 points (the far edge repeats the near one),
   map units, the cells split along (0,0)-(1,1) as the game's (0x401000) */
static int terrain_mesh(void)
{
    enum { N = 257 };
    float *v = malloc((size_t)N * N * 12);
    uint32_t *ix = malloc((size_t)256 * 256 * 6 * 4);
    if (!v || !ix)
        fatal("shadows: out of memory");
    for (int z = 0; z < N; z++)
        for (int x = 0; x < N; x++) {
            float *p = v + 3 * (z * N + x);
            p[0] = (float)x * 256.0f;
            p[1] = RF32(HEIGHTS + 4 * (uint32_t)((z & 255) << 8 | (x & 255)));
            p[2] = (float)z * 256.0f;
        }
    uint32_t *o = ix;
    for (uint32_t z = 0; z < 256; z++)
        for (uint32_t x = 0; x < 256; x++) {
            uint32_t a = z * N + x, b = a + 1, d = a + N, e = d + 1;
            *o++ = a; *o++ = d; *o++ = e;
            *o++ = a; *o++ = e; *o++ = b;
        }
    int ok = vk_shadow_terrain(v, N * N, ix, 256 * 256 * 6);
    free(v);
    free(ix);
    return ok;
}

static uint32_t map_sum;                /* what the map's mesh and light map were made from */
static int map_ok;

/* a quick fingerprint of the heights, the object light map and the sun (a new map, or the same) */
static uint32_t map_fingerprint(void)
{
    uint32_t h = 2166136261u, om = R32(OBJ_LIGHT);
    for (uint32_t i = 0; i < 65536; i += 61)
        h = (h ^ R32(HEIGHTS + 4 * i)) * 16777619u;
    for (uint32_t i = 0; om && i < 65536; i += 251)
        h = (h ^ R8(om + i)) * 16777619u;
    for (uint32_t i = 0; i < 3; i++)
        h = (h ^ R32(LIGHT_DIR + 4 * i)) * 16777619u;
    return h ^ om;
}

static void map_setup(void)
{
    uint32_t sum = map_fingerprint();
    if (map_ok && sum == map_sum)
        return;
    map_sum = sum;
    uint32_t om = R32(OBJ_LIGHT);
    int a = om && vk_shadow_lightmap((const uint8_t *)gptr(om)), b = terrain_mesh();
    map_ok = a && b;
    if (logon) {
        float d[3] = {RF32(LIGHT_DIR), RF32(LIGHT_DIR + 4), RF32(LIGHT_DIR + 8)};
        LOG(LOG_ALWAYS, "shadows: map set up (sun from %.3f %.3f %.3f, %.0f degrees up; light map %s, terrain mesh %s)",
            -d[0], -d[1], -d[2], asin(fmin(fmax(-d[1], -1.0), 1.0)) * 180 / M_PI, a ? "read" : "missing",
            b ? "made" : "not made");
    }
}

/* ------------------------------------------------------------------ the view */
static void view_setup(VkShadowView *s)
{
    memset(s, 0, sizeof *s);
    s->cam[0] = (int32_t)R32(CAM_X) / 65536.0;
    s->cam[1] = (int32_t)R32(CAM_Y) / 65536.0;
    s->cam[2] = (int32_t)R32(CAM_Z) / 65536.0;
    double l = 0, d[3];
    for (int i = 0; i < 3; i++) {
        d[i] = -(double)RF32(LIGHT_DIR + 4 * (uint32_t)i);
        l += d[i] * d[i];
    }
    l = sqrt(l);
    for (int i = 0; i < 3; i++)
        s->sun[i] = l > 0 ? (float)(d[i] / l) : (float)(i == 1);
    for (int i = 0; i < 3; i++)       /* (the sun's D3DLIGHT7: its diffuse colour 60 bytes before the direction) */
        s->sun_color[i] = fminf(fmaxf(RF32(LIGHT_DIR - 60 + 4 * (uint32_t)i), 0.0f), 1.0f);
    /* out to three quarters of the view's far plane (it moves out with the draw distance), at most
       about 400 m (objects are drawn to about 480 m) */
    float far = RACE_FAR * drawdist_factor();
    s->fade_end = fminf(far * 0.75f, 26000.0f);
    s->fade_start = s->fade_end * 0.7f;
    s->ray_len = 16384.0f;
    s->offset = 0.002f;
    /* the terrain as dark as the game's own shadow shapes make it there: black at alpha
       (L - L / 4) * 255 >> 8 (0x419500), L the object light map */
    s->terrain_dark = 0.75f * 255.0f / 256.0f;
    s->terrain = map_ok;
    s->shadows = want;
    s->ao = want_ao;
    int light = want_ao == 1;
    s->ao_rays = light ? ao_light_rays : 3;
    s->ao_rows = light ? ao_light_rows : 400.0f;
    s->ao_radius = light ? ao_light_radius : ao_radius;
    s->ao_strength = ao_strength;
    s->ao_terrain = ao_terrain;
    s->ao_falloff = ao_falloff;
    s->ao_fade_end = fminf(light ? ao_light_far : ao_far, s->fade_end);
    s->ao_fade_start = s->ao_fade_end * 0.6f;
    s->ao_foliage = ao_foliage;
    s->ao_parts = ao_parts;
    s->ao_max = ao_max;
    s->water_dark = water_dark;
    s->water_ao = water_ao;
    s->water_shadow = water_shadow;
    s->refl = want_refl;
    s->refl_fresnel = refl_fresnel;
    s->refl_darken = refl_darken;
    s->refl_len = refl_len;
    s->terrain_detail = terrain_detail;
    s->refl_gain = refl_gain;
    s->nbeams = headlights_beams(s->beams, VK_BEAMS, &s->beam_cap, &s->beam_haze, s->beam_tint);
}

/* A view of the world with the shadows: f draws it (or prev, the hook it chains to, if that does) */
static void view(CPU *c, int (*prev)(CPU *), void (*f)(CPU *))
{
    map_setup();
    VkShadowView s;
    view_setup(&s);
    vk_shadow_begin(&s);
    view_on = 1;
    view_shadows = s.shadows;
    int min = vk_tnl_min;
    vk_tnl_min = 3;                     /* small objects' transform on the GPU too: they cast */
    if (!(prev && prev(c)))
        f(c);
    vk_tnl_min = min;
    view_on = view_shadows = 0;
    vk_shadow_end();
}

/* 0x465950(range, ...): the world round a camera: the race's main view (from 0x465b60) and the
   in-car view's mirrors (0x41cb20) */
/* INSANE_DEBUG_CAM=x,z[,metres[,frame[,dx,dz]]] (map cells 0..255): the race view drawn from there
   (the camera's place moved for the view's drawing and put back after it; its turn stays the
   game's), from that frame on, moving dx, dz cells a frame: for looking at a place the scripted
   races do not reach (tests only) */
static int dbg_cam_on = -1;
static double dbg_cam[3], dbg_cam_step[2];
static unsigned long dbg_cam_from;

static int world_hook(CPU *c)
{
    if (!active())
        return prev_world ? prev_world(c) : 0;
    if (dbg_cam_on < 0) {
        const char *e = getenv("INSANE_DEBUG_CAM");
        double m = 4, f = 0;
        dbg_cam_on = e && sscanf(e, "%lf,%lf,%lf,%lf,%lf,%lf", &dbg_cam[0], &dbg_cam[2], &m, &f, &dbg_cam_step[0],
                                 &dbg_cam_step[1]) >= 2;
        dbg_cam[1] = m * 66.67;
        dbg_cam_from = (unsigned long)f;
    }
    uint32_t saved[3];
    int moved = dbg_cam_on && frame_count >= dbg_cam_from;
    if (moved) {
        double k = (double)(frame_count - dbg_cam_from), px = dbg_cam[0] + k * dbg_cam_step[0],
               pz = dbg_cam[2] + k * dbg_cam_step[1];
        int cx = (int)px & 255, cz = (int)pz & 255;
        double y = RF32(HEIGHTS + 4 * (uint32_t)(cz << 8 | cx)) + dbg_cam[1];
        for (int i = 0; i < 3; i++)
            saved[i] = R32(CAM_X + 4 * (uint32_t)i);
        W32(CAM_X, (uint32_t)(int32_t)lround(px * 256.0 * 65536.0));
        W32(CAM_Y, (uint32_t)(int32_t)lround(y * 65536.0));
        W32(CAM_Z, (uint32_t)(int32_t)lround(pz * 256.0 * 65536.0));
    }
    view(c, prev_world, F_00465950_orig);
    if (moved)
        for (int i = 0; i < 3; i++)
            W32(CAM_X + 4 * (uint32_t)i, saved[i]);
    return 1;
}

/* 0x463000: the rear-view mirror over the race view (Video Options MIRROR), drawn after it */
static int mirror_hook(CPU *c)
{
    if (!active() || ws_race_depth <= 0)
        return 0;
    view(c, NULL, F_00463000_orig);
    return 1;
}

/* 0x402a90(range): the terrain (and drawdist.c's far terrain after it) */
static int terrain_hook(CPU *c)
{
    if (!view_on)
        return prev_terrain ? prev_terrain(c) : 0;
    vk_shadow_class(VK_SH_TERRAIN);
    if (!(prev_terrain && prev_terrain(c)))
        F_00402a90_orig(c);
    vk_shadow_class(VK_SH_OBJECT);
    return 1;
}

/* 0x419280: the frame's see-through model parts, sorted by depth, drawn without depth writes:
   lattice towers and gates, fences, car glass. They cast shadows by their textures' alpha. */
static int see_through_hook(CPU *c)
{
    if (!view_on)
        return 0;
    vk_shadow_class(VK_SH_PARTS);
    F_00419280_orig(c);
    vk_shadow_class(VK_SH_OBJECT);
    return 1;
}

/* 0x406370: the water, two passes over the map's water cells (its colour, blended over the ground
   below, then its waves added); both draw no depth */
static int water_hook(CPU *c)
{
    if (!view_on)
        return prev_water ? prev_water(c) : 0;
    vk_shadow_class(VK_SH_WATER);
    if (!(prev_water && prev_water(c)))
        F_00406370_orig(c);
    vk_shadow_class(VK_SH_OBJECT);
    return 1;
}

/* 0x41c7a0: the sky dome round the camera (and the sun after it), first in a view: what ray-traced
   reflections that meet nothing show */
static int sky_hook(CPU *c)
{
    if (!view_on)
        return 0;
    vk_shadow_class(VK_SH_SKY);
    F_0041c7a0_orig(c);
    vk_shadow_class(VK_SH_OBJECT);
    return 1;
}

/* 0x41a670(object, ...): an object's (or a car's) own shadow shape; not with ray-traced ones */
static int object_shadow_hook(CPU *c)
{
    if (view_shadows) {
        c->ret = R32(c->esp);           /* return at once (cdecl) */
        c->esp += 4;
        return 1;
    }
    return prev_object_shadow ? prev_object_shadow(c) : 0;
}

/* 0x41a6b0(record, ...): a car's shadow painted into its texture, each race frame; with
   ray-traced shadows none (record + 4 = 0, as the routine leaves it when there is no shadow:
   0x419500 then draws nothing) */
static int car_shadow_hook(CPU *c)
{
    if (!shadows_rt())
        return 0;
    uint32_t rec = R32(c->esp + 4);
    if (rec)
        W32(rec + 4, 0);
    c->ret = R32(c->esp);
    c->esp += 4;
    return 1;
}

/* 0x401580: the terrain's vertex buffers made (a race or the menus start): the map may be new */
static int terrain_buffers_hook(CPU *c)
{
    map_ok = 0;
    return prev_terrain_buffers ? prev_terrain_buffers(c) : 0;
}

/* ------------------------------------------------------------------ options */
static void parse(void)
{
    const char *v = option_str("shadows", "original");
    want = !strcasecmp(v, "raytraced");
    if (!want && strcasecmp(v, "original"))
        LOG(LOG_ALWAYS, "options: shadows \"%s\" is not original or raytraced", v);
    const char *a = option_str("ambient_occlusion", "off");
    want_ao = !strcasecmp(a, "on") || !strcasecmp(a, "full") ? 2 : !strcasecmp(a, "light") ? 1 : 0;
    if (!want_ao && strcasecmp(a, "off"))
        LOG(LOG_ALWAYS, "options: ambient_occlusion \"%s\" is not off, light or on (full)", a);
    const char *r = option_str("reflections", "original");
    want_refl = !strcasecmp(r, "raytraced") || !strcasecmp(r, "full") ? 2 : !strcasecmp(r, "light") ? 1 : 0;
    if (!want_refl && strcasecmp(r, "original"))
        LOG(LOG_ALWAYS, "options: reflections \"%s\" is not original, light or raytraced (full)", r);
}

void shadows_init(void)
{
    logon = getenv("INSANE_SH_LOG") != NULL;
    allowed = !deterministic || getenv("INSANE_ENHANCED");
    if (!allowed)
        return;
    const char *e;
    if ((e = getenv("INSANE_AO_RADIUS")) && atof(e) > 0)
        ao_radius = (float)atof(e);
    if ((e = getenv("INSANE_AO_STRENGTH")))
        ao_strength = (float)atof(e);
    if ((e = getenv("INSANE_AO_TERRAIN")))
        ao_terrain = (float)atof(e);
    if ((e = getenv("INSANE_AO_FALLOFF")) && atof(e) > 0)
        ao_falloff = (float)atof(e);
    if ((e = getenv("INSANE_AO_FAR")) && atof(e) > 0)
        ao_far = (float)atof(e);
    if ((e = getenv("INSANE_AO_FOLIAGE")))
        ao_foliage = fminf(fmaxf((float)atof(e), 0.0f), 1.0f);
    if ((e = getenv("INSANE_AO_MAX")))
        ao_max = fminf(fmaxf((float)atof(e), 0.0f), 1.0f);
    if ((e = getenv("INSANE_AO_PARTS")))
        ao_parts = fminf(fmaxf((float)atof(e), 0.0f), 1.0f);
    if ((e = getenv("INSANE_WATER_DARK")))
        water_dark = fminf(fmaxf((float)atof(e), 0.0f), 1.0f);
    if ((e = getenv("INSANE_WATER_AO")))
        water_ao = fminf(fmaxf((float)atof(e), 0.0f), 1.0f);
    if ((e = getenv("INSANE_WATER_SHADOW")))
        water_shadow = fminf(fmaxf((float)atof(e), 0.0f), 1.0f);
    if ((e = getenv("INSANE_REFL_DETAIL")))
        terrain_detail = fminf(fmaxf((float)atof(e), 0.0f), 1.0f);
    if ((e = getenv("INSANE_REFL_LEN")) && atof(e) > 0)
        refl_len = (float)atof(e);
    parse();
    /* after drawdist.c: its hooks on the same functions run inside ours */
    prev_world = hook_world_draw;
    hook_world_draw = world_hook;
    prev_terrain = hook_terrain_draw;
    hook_terrain_draw = terrain_hook;
    prev_object_shadow = hook_object_shadow;
    hook_object_shadow = object_shadow_hook;
    prev_terrain_buffers = hook_terrain_buffers;
    hook_terrain_buffers = terrain_buffers_hook;
    hook_car_shadow = car_shadow_hook;
    hook_see_through = see_through_hook;
    hook_mirror_view = mirror_hook;
    prev_water = hook_water_draw;
    hook_water_draw = water_hook;
    hook_sky_draw = sky_hook;
}

void shadows_options(void)
{
    if (allowed)
        parse();
}

/* for the Enhanced page: can the shadows, ambient occlusion and reflections be ray traced here? */
int shadows_available(void)
{
    return vk_rt_available();
}
