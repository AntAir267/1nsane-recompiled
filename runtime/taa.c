/* Temporal anti-aliasing and upscaling (Enhanced options "antialiasing = fsr" and "render_scale";
   DESIGN.md decision 4, "Temporal anti-aliasing"): the game's side of vk.c's scene. The scene is
   the race's main view, 0x465950 called from the race frame (its two calls, as drawdist.c knows
   them): everything the world has, the sky dome to the see-through parts. The rear-view mirror
   (0x463000) and the in-car view's mirrors (0x465950 from 0x41cb20) are views of their own,
   drawn after it at the screen's size as they always were; so are the HUD, the name labels and
   the menus. Inside the scene vk.c is told what the draws are: the sky dome (0x41c7a0, it moves
   with the camera), the terrain (0x402a90, with drawdist.c's far terrain), the water (0x406370),
   the see-through parts (0x419280), the skid marks (0x41f7e0) and the shadow shapes (0x41a670: a
   car's moves with it, the others lie still). The camera's place (its 16.16 position) comes from
   the view's first draws (the sky's or the terrain's), after shadows.c's debug camera moved it.
   With "colour_precision = high" and no FSR 2 the same view is vk.c's scene too, drawn in 16 bits a
   channel at the back buffer's own size and dithered into it at its end (no jitter or vectors).
   In the driver camera (0x1bc71bc set) 0x4661c0 draws the cockpit after the world (0x462550: the
   dashboard, skinned to the body, and the gauges), into the same depth as the world: the steering
   wheel and the driver's hands, part of the world, stand in front of the dashboard. So the scene
   stays open through 0x462550 then; ended after the world, the dashboard was drawn into the back
   buffer's own depth, which has none of the scene's, and covered the wheel and hands (Ant: "the
   game normally shows the steering wheel in first person").
   Off in --test runs unless INSANE_ENHANCED is set: the hooks are not even put in. */
#include <stdlib.h>
#include "runtime.h"
#include "funcs.h"
#include "gfx.h"
#include "vk.h"
#include "taa.h"

#define VIEW_RET1       0x465ec8u       /* 0x465950 called for the race's main view (drawdist.c) */
#define VIEW_RET2       0x4660c5u
#define CAM_X           0xa4ca90u       /* the camera, 16.16 game units */
#define CAM_Y           0xa4ca94u
#define CAM_Z           0xa4ca98u

#define OBJ_FLAGS       0x54            /* an object's flags: 0x40 a car (its shadow is 0x419500's) */
#define IN_CAR          0x1bc71bcu      /* the driver camera: the cockpit is drawn after the world */

static int (*prev_world)(CPU *), (*prev_sky)(CPU *), (*prev_terrain)(CPU *), (*prev_water)(CPU *),
    (*prev_parts)(CPU *), (*prev_shadow)(CPU *);
static int in_scene;
static int cockpit_due;                 /* the scene left open for the cockpit (0x462550) */

static VkScene camera(void)
{
    VkScene s = {{(int32_t)R32(CAM_X), (int32_t)R32(CAM_Y), (int32_t)R32(CAM_Z)}};
    return s;
}

/* 0x465950(range, ...): the world round a camera */
static int world_hook(CPU *c)
{
    uint32_t ret = R32(c->esp);
    if (in_scene || (ret != VIEW_RET1 && ret != VIEW_RET2) || !vk_scene_on())
        return prev_world ? prev_world(c) : 0;
    if (cockpit_due) {                  /* (a cockpit that never came: the last scene ends here) */
        cockpit_due = 0;
        vk_scene_end();
    }
    VkScene s = camera();
    vk_scene_begin(&s);
    in_scene = 1;
    int min = vk_tnl_min;
    if (vk_taa_on())
        vk_tnl_min = 3;                 /* small objects' transform on the GPU too: they are matched */
    if (!(prev_world && prev_world(c)))
        F_00465950_orig(c);
    vk_tnl_min = min;
    in_scene = 0;
    if (R32(IN_CAR))
        cockpit_due = 1;                /* (ended after the cockpit) */
    else
        vk_scene_end();
    return 1;
}

/* 0x462550(): the driver camera's cockpit, in the scene the world left open */
static int cockpit_hook(CPU *c)
{
    if (!cockpit_due)
        return 0;
    in_scene = 1;
    int min = vk_tnl_min;
    if (vk_taa_on())
        vk_tnl_min = 3;
    F_00462550_orig(c);
    vk_tnl_min = min;
    in_scene = 0;
    cockpit_due = 0;
    vk_scene_end();
    return 1;
}

/* a part of the scene of kind cls: f draws it (or prev, the hook it chains to, if that does) */
static int part(CPU *c, int cls, int (*prev)(CPU *), void (*f)(CPU *))
{
    if (!in_scene)
        return prev ? prev(c) : 0;
    if (cls == VK_SC_SKY || cls == VK_SC_TERRAIN) {
        VkScene s = camera();           /* (the view's first draws: the camera as it draws) */
        vk_scene_camera(&s);
    }
    vk_scene_class(cls);
    if (!(prev && prev(c)))
        f(c);
    vk_scene_class(VK_SC_OBJECT);
    return 1;
}

static int sky_hook(CPU *c) { return part(c, VK_SC_SKY, prev_sky, F_0041c7a0_orig); }
static int terrain_hook(CPU *c) { return part(c, VK_SC_TERRAIN, prev_terrain, F_00402a90_orig); }
static int water_hook(CPU *c) { return part(c, VK_SC_WATER, prev_water, F_00406370_orig); }
static int parts_hook(CPU *c) { return part(c, VK_SC_PARTS, prev_parts, F_00419280_orig); }
static int skid_hook(CPU *c) { return part(c, VK_SC_DECAL, NULL, F_0041f7e0_orig); }

/* 0x41a670(object, ...): an object's shadow shape on the ground (a car's moves with it) */
static int shadow_hook(CPU *c)
{
    uint32_t obj = R32(c->esp + 4);
    return part(c, obj && (R32(obj + OBJ_FLAGS) & 0x40) ? VK_SC_CAR_SHADOW : VK_SC_DECAL, prev_shadow, F_0041a670_orig);
}

void taa_init(void)
{
    if (deterministic && !getenv("INSANE_ENHANCED"))
        return;
    /* after shadows.c and drawdist.c: theirs run inside ours */
    prev_world = hook_world_draw;
    hook_world_draw = world_hook;
    prev_sky = hook_sky_draw;
    hook_sky_draw = sky_hook;
    prev_terrain = hook_terrain_draw;
    hook_terrain_draw = terrain_hook;
    prev_water = hook_water_draw;
    hook_water_draw = water_hook;
    prev_parts = hook_see_through;
    hook_see_through = parts_hook;
    hook_skid_draw = skid_hook;
    hook_cockpit_draw = cockpit_hook;
    prev_shadow = hook_object_shadow;
    hook_object_shadow = shadow_hook;
}
