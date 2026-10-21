/* Volumetric smoke and dust (Enhanced option "smoke": original | volumetric; DESIGN.md decision 4,
   "Volumetric smoke and dust").

   How the game makes them. Its puffs are objects of three types every map has (data\objtypes.cfg;
   the map loader 0x45c500 looks them up by name, 0x48c000): SMOKE (anim data\dust.ani: dark grey),
   DUST (data\smoke.ani: tan) and WATER (data\water.ani: blue-white splashes). An animation is a
   sprite model a phase (30 or 16 .fff models, SPRS: one camera-facing quad of a cell of
   Data\Smoke.pcx's or wtrsplat.pcx's atlas, 10 to 170 units across, and a colour); the cells
   grow a soft blob and then thin it away, so the phases are the puff's growth and fade. The
   object's dynamics (0x485d60) count its life down and pick the phase from it: dynamics 3 (SMOKE,
   DUST: life 200 + rand 50, 2 a tick, a phase every 4 ticks; drifting towards the wind, rising,
   held above the ground) and 5 (WATER: life 100 + rand 25, 1 a tick, a phase every 8; falling,
   slowed, bounced off the ground and the water). The objects are made by:
   - the exhausts (0x4a21a0, the car's .vht "exhaust x y z dx dy dz rate"): SMOKE at the pipe while
     the throttle runs ahead of the revs (revving, pulling away: the more, the more puffs), blown
     out along the pipe by the throttle with a random spread, flagged 0x10; none under water;
   - wheels sliding or spinning (0x4a1460): the ground's own type (its row of the style's
     talaj.dat, how many by its gnd_objnum): DUST (Africa's sand and tracks), SMOKE (Canyon's),
     WATER (Snowy's snow), or solid bits (MUD, PEBBLE, GRASS, DEBRIS: models; left as they are);
   - wheels in water (0x4a21a0): WATER, the spray;
   - a car put back on its wheels (0x468670, at each of its points): DUST.
   The game has no damage, engine or fire smoke: a wreck throws DEBRIS (solid bits, 0x4a07c0).
   Puffs are drawn with the frame's see-through parts, sorted by depth (0x419280 -> 0x419300, a
   node each -> 0x41d320 -> the batch): one 4-vertex draw each, SRCALPHA / INVSRCALPHA, no depth
   writes, its colour lit on the CPU.

   VOLUMETRIC: in a view of the world (0x465950, the rear-view mirror 0x463000), a node whose
   object is a puff (0x419300, hook see_through_node) is drawn with vk_puff set: its draw is taken,
   not drawn (vk.c vk_smoke_take: the quad's corners, colour and atlas cell), with what the object
   says (its kind, number, age, the light and the water's surface at its place). The other
   see-through parts' draws (0x419280: glass, lattices) are marked (vk_smoke_parts) so that vk.c
   keeps them: puffs behind them are seen through them. At the view's end vk.c draws the view's
   puffs as volumes (shaders/smoke.frag). ORIGINAL changes nothing. Off in --test runs unless
   INSANE_ENHANCED is set. */
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "funcs.h"
#include "gfx.h"
#include "vk.h"
#include "smoke.h"

/* ------------------------------------------------------------------ game addresses (GOG build) */
#define TYPE_SMOKE      0x21f9654u      /* object type numbers (0x45c500): SMOKE, */
#define TYPE_DUST       0x21f7a5cu      /* ... DUST, */
#define TYPE_WATER      0x2200008u      /* ... WATER (0: the map has none) */
#define OBJ_POS         0x30            /* an object: x, y, z (16.16) */
#define OBJ_FLAGS       0x54            /* ... flags (0x10: an exhaust's puff) */
#define OBJ_LIFE        0x5c            /* ... life left (dynamics 3: 2 a tick, 5: 1) */
#define OBJ_TYPE        0x60            /* ... type (byte) */
#define OBJ_DYNAMICS    0x61            /* ... dynamics (byte) */
#define NODE_LEFT       4               /* a node of the see-through parts' tree: drawn before it, */
#define NODE_RIGHT      8               /* ... after it, */
#define NODE_OBJ        0x10            /* ... its object */
#define OBJ_LIGHT       0x4c3070u       /* -> the object light map: byte[256 * 256], z * 256 + x */
#define WATER_MAP       0x4c306cu       /* -> the water's surface: byte[256 * 256] (signed, x16 units; 0 none) */
#define LIGHT_DIR       0x1bf2290u      /* the sun's D3DLIGHT7 direction (floats: -(toward the sun) / 65536) */
#define OBJ_SHADING     0x9eed54u       /* 0x419300 lights objects by the light map (else full) */
#define HEIGHTS         0x924400u       /* float[256 * 256], z * 256 + x: the ground's height (.map byte * 16) */

static int allowed, want;               /* want: 0 ORIGINAL, 2 VOLUMETRIC */
static int depth;                       /* inside a view whose puffs we take */
static int (*prev_world)(CPU *), (*prev_mirror)(CPU *), (*prev_parts)(CPU *);

int smoke_available(void)
{
    return vk_on();
}

static int active(void)
{
    return allowed && want && vk_on();
}

/* ------------------------------------------------------------------ the map at a puff */
/* a byte map's value at 16.16 x, z, bilinear between its 4 points (sign: the bytes are signed) */
static float map_at(uint32_t map, int32_t x, int32_t z, int sign, int *any)
{
    uint32_t cx = (uint32_t)x >> 24, cz = (uint32_t)z >> 24;
    float fx = (float)(((uint32_t)x >> 8) & 0xffff) / 65536.0f, fz = (float)(((uint32_t)z >> 8) & 0xffff) / 65536.0f;
    float v[4];
    int nz = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t i = ((cz + (uint32_t)(k >> 1)) & 255) << 8 | ((cx + (uint32_t)(k & 1)) & 255);
        uint8_t b = (uint8_t)R8(map + i);
        v[k] = sign ? (float)(int8_t)b : (float)b;
        nz |= b != 0;
    }
    if (any)
        *any = nz;
    return (v[0] * (1 - fx) + v[1] * fx) * (1 - fz) + (v[2] * (1 - fx) + v[3] * fx) * fz;
}

/* ------------------------------------------------------------------ the hooks */
/* call 0x419300 for node n from a hook (the same return address) */
static void node_call(CPU *c, uint32_t n, uint32_t ret)
{
    c->esp -= 4;
    W32(c->esp, n);
    c->esp -= 4;
    W32(c->esp, ret);
    F_00419300(c);                      /* (pops the return address) */
    c->esp += 4;
}

/* 0x419300(node): a node of the see-through parts' tree, the farther side first, then the node's
   object, then the nearer side. A puff's draw is taken (vk_puff) instead of drawn. */
static int node_hook(CPU *c)
{
    if (!depth || !want)
        return 0;
    uint32_t node = ARG(0), obj = node ? R32(node + NODE_OBJ) : 0;
    if (!obj)
        return 0;
    uint32_t t = R8(obj + OBJ_TYPE), ts = R32(TYPE_SMOKE), td = R32(TYPE_DUST), tw = R32(TYPE_WATER);
    VkPuff p = {0};
    if (ts && t == ts)
        p.kind = (R32(obj + OBJ_FLAGS) & 0x10) ? VK_PUFF_EXHAUST : VK_PUFF_SMOKE;
    else if (td && t == td)
        p.kind = VK_PUFF_DUST;
    else if (tw && t == tw)
        p.kind = VK_PUFF_SPRAY;
    else
        return 0;
    int32_t life = (int32_t)R32(obj + OBJ_LIFE);
    int step = R8(obj + OBJ_DYNAMICS) == 5 ? 1 : 2;
    p.serial = R32(obj);
    p.grow = (float)(7 - (life & 7)) / 8.0f;                /* (a phase each 8 of life) */
    p.age = (float)(256 - life) / (float)step;
    int32_t x = (int32_t)R32(obj + OBJ_POS), y = (int32_t)R32(obj + OBJ_POS + 4), z = (int32_t)R32(obj + OBJ_POS + 8);
    uint32_t lm = R32(OBJ_LIGHT), wm = R32(WATER_MAP);
    p.light = lm && R32(OBJ_SHADING) ? map_at(lm, x, z, 0, NULL) / 255.0f : 1.0f;
    int water = 0;
    float wy = wm ? map_at(wm, x, z, 1, &water) * 16.0f : 0.0f;
    p.floor_dy = water ? wy - (float)y / 65536.0f : -1.0e9f;
    {   /* the ground below it (bilinear between the 4 points of its cell) */
        uint32_t cx = (uint32_t)x >> 24, cz = (uint32_t)z >> 24;
        float fx = (float)(((uint32_t)x >> 8) & 0xffff) / 65536.0f, fz = (float)(((uint32_t)z >> 8) & 0xffff) / 65536.0f;
        float h[4];
        for (int k = 0; k < 4; k++)
            h[k] = RF32(HEIGHTS + 4 * (((cz + (uint32_t)(k >> 1)) & 255) << 8 | ((cx + (uint32_t)(k & 1)) & 255)));
        float g = (h[0] * (1 - fx) + h[1] * fx) * (1 - fz) + (h[2] * (1 - fx) + h[3] * fx) * fz;
        p.ground_dy = g - (float)y / 65536.0f;
    }
    uint32_t ret = R32(c->esp), left = R32(node + NODE_LEFT), right = R32(node + NODE_RIGHT);
    if (left) {
        W32(node + NODE_LEFT, 0);
        node_call(c, left, ret);
    }
    W32(node + NODE_RIGHT, 0);
    vk_puff = &p;
    F_00419300_orig(c);                 /* (this node alone now; pops the return address) */
    vk_puff = NULL;
    if (right) {
        c->esp -= 4;                    /* (back to the frame as called, for the nearer side) */
        W32(c->esp, ret);
        node_call(c, right, ret);
        c->ret = R32(c->esp);
        c->esp += 4;
    }
    return 1;
}

/* 0x419280: the frame's see-through parts, sorted by depth: their draws but the puffs (glass,
   lattices, fences) are what puffs behind them are seen through (vk.c sm_glass_add) */
static int parts_hook(CPU *c)
{
    if (!depth || !want)
        return prev_parts ? prev_parts(c) : 0;
    vk_smoke_parts = 1;
    if (!(prev_parts && prev_parts(c)))
        F_00419280_orig(c);
    vk_smoke_parts = 0;
    return 1;
}

/* a view of the world: f draws it (or prev, the hook it chains to, if that does) */
static void view(CPU *c, int (*prev)(CPU *), void (*f)(CPU *))
{
    VkSmokeView v;
    memset(&v, 0, sizeof v);
    double l = 0, d[3];
    for (int i = 0; i < 3; i++) {
        d[i] = -(double)RF32(LIGHT_DIR + 4 * (uint32_t)i);
        l += d[i] * d[i];
    }
    l = sqrt(l);
    for (int i = 0; i < 3; i++) {
        v.sun[i] = l > 0 ? (float)(d[i] / l) : (float)(i == 1);
        /* (the sun's D3DLIGHT7: its diffuse colour 60 bytes before the direction) */
        v.sun_color[i] = fminf(fmaxf(RF32(LIGHT_DIR - 60 + 4 * (uint32_t)i), 0.0f), 1.0f);
    }
    v.level = want;
    vk_smoke_view_begin(&v);
    depth++;
    if (!(prev && prev(c)))
        f(c);
    depth--;
    vk_smoke_view_end();
}

/* 0x465950(range, ...): the world round a camera (the race's main view, the in-car view's mirrors) */
static int world_hook(CPU *c)
{
    if (!active())
        return prev_world ? prev_world(c) : 0;
    view(c, prev_world, F_00465950_orig);
    return 1;
}

/* 0x463000: the rear-view mirror over the race view */
static int mirror_hook(CPU *c)
{
    if (!active())
        return prev_mirror ? prev_mirror(c) : 0;
    view(c, prev_mirror, F_00463000_orig);
    return 1;
}

static void parse(void)
{
    const char *v = option_str("smoke", "original");
    want = !strcasecmp(v, "volumetric") ? 2 : 0;
    if (!want && strcasecmp(v, "original"))
        LOG(LOG_ALWAYS, "options: smoke \"%s\" is not original or volumetric", v);
    vk_smoke_depth = allowed && want;
}

void smoke_init(void)
{
    allowed = !deterministic || getenv("INSANE_ENHANCED");
    if (!allowed)
        return;
    parse();
    /* after shadows.c (its views run inside ours: the volumes go on after its shadows and
       reflections) and before taa.c (ours inside its scene) */
    prev_world = hook_world_draw;
    hook_world_draw = world_hook;
    prev_mirror = hook_mirror_view;
    hook_mirror_view = mirror_hook;
    hook_see_through_node = node_hook;
    prev_parts = hook_see_through;
    hook_see_through = parts_hook;
}

void smoke_options(void)
{
    if (allowed)
        parse();
}
