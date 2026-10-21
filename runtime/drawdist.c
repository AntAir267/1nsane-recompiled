/* Draw distance and distance fade (Enhanced options "draw_distance" and "distance_fade", design
   point 1).

   The game draws the world round the camera out to its visibility setting's range r (Video
   Options VISIBILITY, 2..8 from the table at 0x4c8024): the terrain in a window of r * 16 cells
   (1 cell = 256 units = 3.84 m by the game's own speedometer and distances) that its quadtree
   (0x4032e0, from 0x402a90) walks, objects inside a box of the same size (0x4182d0), the map's
   EXP fog thick enough to hide most of the edge, and the race camera's far plane at 20000 units.

   DRAW DISTANCE (1.5X, 2X, 3X, 4X) leaves the game's own terrain as it is and has the renderer
   draw the rest of it, the far terrain: the map's heights (0x924400, 256 x 256 cells, wrapping)
   as one mesh of the 16 colour tiles (64 x 64 cells each, with the same textures, texture
   coordinates, colour and fog as the game's terrain), in 8 x 8-cell blocks so that the game's
   window is left out, repeated where the map wraps, out to the far plane. Objects come from a
   bigger box (r * 1.5; past 15, the most the box sums take, the whole map), and their last
   distance (the end of their .gem list) moves out by the factor; shadows, decals and other near
   work keep their original distances. The map's fog thins and the far plane moves out by the same
   factor. Chosen when a race starts (0x401580: the terrain's vertex buffers made). ORIGINAL
   changes nothing. The game's draws cannot repeat objects (positions wrap): past half the map
   (128 cells) the map's copies are drawn from kept meshes (below). AUTO (the section of that
   name) follows what the camera can see, up to 8X, and leaves out what the ground hides.

   DISTANCE FADE fades things in by an ordered dither (vk_fade: each pixel drawn or not, so no
   sorting, depth stays right) instead of letting them pop: by distance, objects near the end of
   their .gem list (and, with the whole map, before they wrap to behind the camera); over a third
   of a second, an object that comes into the object box (it moves in steps of 8 cells) and an
   object whose model changed (the old one drawn too, in the other pixels). The far terrain is
   drawn at ORIGINAL too, so the game's window edge (also stepping by 8 cells) never shows, and
   fogged draws reach full fog at the far plane (vk_fog_end). The objects' instances are drawn one
   by one from the game's batches (0x40aea0 adds one, 0x40a770 draws them), so each takes its own
   fade. Off in --test runs unless INSANE_ENHANCED is set. INSANE_DD_LOG=1 logs the set-ups and
   how many objects fade. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "funcs.h"
#include "gfx.h"
#include "vk.h"
#include "drawdist.h"

/* ------------------------------------------------------------------ game addresses (GOG build) */
#define RANGE_TABLE     0x4c8024u       /* int[8]: range per visibility level */
#define VIS_LEVEL       0x4c5ec8u       /* visibility level 0..7 */
#define VIEW_RET1       0x465ec8u       /* 0x465950 called for the race's main view */
#define VIEW_RET2       0x4660c5u
#define FOG_RET_LOAD    0x45ceddu       /* the map's fog made at load (0x45c500) */
#define FOG_RET_LEVEL   0x41e14bu       /* ... and again when the visibility level changes */
#define RACE_FAR        20000.0f        /* the race camera's far plane */
/* the camera: 16.16 units (x, z: 8.24 cells), signed (the map is -128..127 cells and wraps) */
#define CAM_X           0xa4ca90u
#define CAM_Y           0xa4ca94u
#define CAM_Z           0xa4ca98u
#define CAM_FOV         0xa3ca84u       /* degrees (float): the .gem distances scale with it */
#define LOD_SCALE       0x4c601cu       /* float: ... and with this (object detail) */
#define CAM_MATRIX      0xa1f5e0u       /* float[4][4], the objects' view test (column 3: depth) */
/* the object box (0x4182d0), camera-relative 8.24 cells, and distances set from the same range */
#define BOX_X0          0xa199a8u
#define BOX_X1          0xa0ad60u
#define BOX_Z0          0xa1999cu
#define BOX_Z1          0xa0ad64u
static const uint32_t near_dists[] = {0x9d75d8u, 0xa1b1b8u, 0xa1f5d8u, 0xa199a4u, 0xa199a0u};
/* the objects drawn this frame: models {model, first node, count} and nodes {next, object} */
#define MODELS          0xa199b0u
#define NMODELS         0xa3b620u       /* room for 512 */
#define NODES           0x9e6d50u
#define NNODES          0xa199acu       /* room for 4096 */
#define F_DEPTH_ADD     0x418c60u       /* (depth, object, mesh, piece): the see-through parts' list */
/* an object */
#define OBJ_POS         0x30            /* x, y, z */
#define OBJ_FLAGS       0x54            /* 0x2000: drawn part by part */
#define OBJ_TYPE        0x90            /* its type's distance list (.gem) */
#define OBJ_ROT         0xbc            /* 3 x 3 floats */
#define OBJ_CENTRE      0xe0            /* 3 ints, turned by OBJ_ROT */
#define OBJ_RADIUS      0xec            /* int, 16.16 units like the position */
/* a distance list: up to 8 distances (8.24 cells, 127 and up = the end), 9 models */
#define LOD_DIST        0x14
#define LOD_MODEL       0x34
#define MODEL_MESH      0x88
/* object batches: instances added to a vertex buffer, drawn one by one */
#define BATCH_N         0x9d5a4cu
#define PIECE_CUR       0x9d5684u       /* the piece 0x409390 chose (+0x68: its kind, 1 = a mesh) */
/* terrain */
#define HEIGHTS         0x924400u       /* float[256 * 256], z * 256 + x */
#define TILE_TEX        0x1c54a60u      /* the 16 colour tiles' textures (+0x7c: the surface) */
#define TERRAIN_COLOUR  0x4c59c4u       /* its vertex colour where no lights shine */
#define WIN_X           0x818d04u       /* the terrain window's first cell (signed) */
#define WIN_Z           0x818d50u
#define WIN_FROM        0x9103f8u       /* the part of the window drawn: cells from..to-1 */
#define WIN_TO          0x9243fcu
/* the frame's lists (INSANE_DD_LOG) */
#define TILE_COUNTS     0x7d4c68u       /* 16 colour tiles */
#define DETAIL_COUNTS   0x7d2740u       /* 5 detail textures */

#define CELL            16777216.0      /* 8.24 fixed point */

static const float factors[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 4.0f};
#define NLEVELS 6
#define AUTO    5           /* AUTO: the factor follows what the camera can see (auto_update) */
static const char *const level_names[] = {"original", "1.5x", "2x", "3x", "4x", "auto"};
static float auto_f = 4.0f; /* AUTO's factor in force */
static int horizon_on;      /* AUTO: what the ground hides is left out this view (hidden) */
static double cam_cells[2];     /* the far terrain's view: the camera (cells) */
static int blocks_test;         /* ... a tile's blocks each tested (hidden) */
static float tile_range[16][2]; /* the colour tiles' lowest and highest ground, units */
static int lod_on;              /* ... with coarser far blocks (AUTO) */
static int horizon_use = 1;     /* INSANE_DD_HORIZON=0: nothing left out (to compare) */
static int lod_use = 1;         /* INSANE_DD_LOD=0: the far blocks all fine (to compare) */
static int hidden(double x, double z, double r, double y);
static int all_shown(double x, double z, double r, double y);
static double block_height(int x, int z);
static int want;            /* asked for: 0 original, 1..4 = 1.5x .. 4x */
static int level;           /* in force since the last set-up */
static int fade_on;         /* distance fade */
static int allowed, ddlog;
static int far_log;         /* INSANE_FAR_LOG (the far objects, below) */
static int main_view;       /* inside 0x465950 for the race's main view */
static int fading;          /* ... with the distance fade */

/* the visibility level's range */
static int vis_range(void)
{
    uint32_t l = R32(VIS_LEVEL);
    return l < 8 ? (int)R32(RANGE_TABLE + 4 * l) : 8;
}

static float factor(void)
{
    return level == AUTO ? auto_f : factors[level];
}

float drawdist_factor(void)
{
    return factor();
}

/* ------------------------------------------------------------------ objects */
static int obj_range;       /* the object box: range (cells / 16), 16 = the whole map */
static double box_half;     /* the box's half size, cells, for the fade */

/* 0x4182d0(range): the object box and the distances drawn from it */
static int object_box_hook(CPU *c)
{
    if (!main_view || !level)
        return 0;
    int r = (int)ARG(0);
    int n = (int)lroundf((float)r * factor());
    if (r <= 0 || n <= r)
        return 0;
    W32(c->esp + 4, (uint32_t)(n > 15 ? 15 : n));
    F_004182d0_orig(c);
    for (size_t i = 0; i < sizeof near_dists / sizeof *near_dists; i++)
        W32(near_dists[i], (uint32_t)(int32_t)((double)(int32_t)R32(near_dists[i]) * r / (n > 15 ? 15 : n)));
    if (n > 15) {           /* the whole map: positions wrap at half of it */
        W32(BOX_X0, 0x80000000u);
        W32(BOX_Z0, 0x80000000u);
        W32(BOX_X1, 0x7fffffffu);
        W32(BOX_Z1, 0x7fffffffu);
    }
    obj_range = n > 15 ? 16 : n;
    box_half = obj_range * 8.0;
    return 1;
}

/* 0x41a670(object, ...): an object's shadow (0x419eb0, or 0x419500 for flag 0x40), fading out
   by depth to the distance 0x4182d0 set (now the original one); past it, none to work out */
#define SHADOW_DIST     0xa1b1b8u
#define SHADOW40_DIST   0xa1f5d8u
#define SHADOW_CAM      0xa3ca60u       /* float[3][3]: column 2 gives the depth */
#define SHADOW_MARK     0xffffffffu     /* fade table key: 1 = two models, 2 = and its shadow done */
static uint32_t fade_get(uint32_t x, uint32_t y, uint32_t z, uint32_t mesh);
static void fade_put(uint32_t x, uint32_t y, uint32_t z, uint32_t mesh, uint32_t fade);
static unsigned long st_faded, st_twice, st_hits;   /* INSANE_DD_LOG */
static int object_shadow_hook(CPU *c)
{
    uint32_t obj = ARG(0);
    if (!main_view || !obj || (!level && !fading))
        return 0;
    if (fading) {               /* drawn with two models (a cross-fade): still one shadow */
        uint32_t x = R32(obj + OBJ_POS), y = R32(obj + OBJ_POS + 4), z = R32(obj + OBJ_POS + 8);
        uint32_t m = fade_get(x, y, z, SHADOW_MARK);
        if (m == 2)
            goto none;
        if (m == 1)
            fade_put(x, y, z, SHADOW_MARK, 2);
    }
    if (level && !(R32(obj + OBJ_FLAGS) & 0x80)) {
        int32_t lim = (int32_t)R32(R32(obj + OBJ_FLAGS) & 0x40 ? SHADOW40_DIST : SHADOW_DIST);
        double depth = 0;
        static const uint32_t cam[3] = {CAM_X, CAM_Y, CAM_Z};
        for (int a = 0; a < 3; a++)
            depth += (double)(int32_t)(R32(obj + OBJ_POS + 4 * a) - R32(cam[a])) * RF32(SHADOW_CAM + 12 * a + 8);
        if (lim > 0 && depth - 2.0 * (int32_t)R32(obj + OBJ_RADIUS) - 8.0 * CELL > lim)
            goto none;
    }
    return 0;
none:
    c->ret = R32(c->esp);               /* return at once (cdecl) */
    c->esp += 4;
    return 1;
}

/* Fades by object and model: open addressing, emptied each view by a new generation */
#define FADES 8192
static struct Fade { uint32_t x, y, z, mesh, gen, fade; } fades[FADES];
static uint32_t fade_gen = 1;

static uint32_t fade_hash(uint32_t x, uint32_t y, uint32_t z, uint32_t mesh)
{
    return (x * 0x9e3779b1u ^ y * 0x85ebca6bu ^ z * 0xc2b2ae35u ^ mesh * 0x27d4eb2fu) >> 19;
}

static void fade_put(uint32_t x, uint32_t y, uint32_t z, uint32_t mesh, uint32_t fade)
{
    for (uint32_t h = fade_hash(x, y, z, mesh), n = 0; n < 64; n++, h = (h + 1) & (FADES - 1)) {
        struct Fade *f = &fades[h];
        if (f->gen != fade_gen || (f->x == x && f->y == y && f->z == z && f->mesh == mesh)) {
            *f = (struct Fade){x, y, z, mesh, fade_gen, fade};
            return;
        }
    }
}

static uint32_t fade_get(uint32_t x, uint32_t y, uint32_t z, uint32_t mesh)
{
    for (uint32_t h = fade_hash(x, y, z, mesh), n = 0; n < 64; n++, h = (h + 1) & (FADES - 1)) {
        const struct Fade *f = &fades[h];
        if (f->gen != fade_gen)
            return 0;
        if (f->x == x && f->y == y && f->z == z && f->mesh == mesh)
            return f->fade;
    }
    return 0;
}

static double clamp01(double v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* Another model of an object into this frame's lists, as 0x4185d0 puts the one it picked */
static int add_model(CPU *c, uint32_t obj, uint32_t model, int32_t depth)
{
    uint32_t mesh = R32(model + MODEL_MESH), nm = R32(NMODELS), e = 0, n = R32(NNODES);
    if (!mesh || n >= 0x1000)
        return 0;
    for (uint32_t u = 0; u < nm && !e; u++)
        if (R32(MODELS + 12 * u) == model)
            e = MODELS + 12 * u;
    if (!e) {
        if (nm >= 0x200)
            return 0;
        e = MODELS + 12 * nm;
        W32(e, model);
        W32(e + 4, 0);
        W32(e + 8, 0);
        W32(NMODELS, nm + 1);
    }
    uint32_t node = NODES + 8 * n;
    W32(node, R32(e + 4));
    W32(node + 4, obj);
    W32(e + 4, node);
    W32(e + 8, R32(e + 8) + 1);
    W32(NNODES, n + 1);
    W32(mesh + 0x70, 0);
    if (!(R32(obj + OBJ_FLAGS) & 0x2000))
        guest_call(c, F_DEPTH_ADD, 4, (uint32_t)depth, obj, mesh, 0u);
    return 1;
}

/* Objects' models over the views (a model switch fades over XFADE views of the main camera):
   open addressing by object, a slot free once its object was not drawn for a view */
#define OBJ_STATES 8192
#define XFADE 20
static struct ObjState { uint32_t obj, gem, model, prev, start, seen, appear; } obj_states[OBJ_STATES];
static int cam_cut;         /* the camera jumped since the last view: no fades of model switches */

static struct ObjState *obj_state(uint32_t obj)
{
    struct ObjState *free_slot = NULL;
    for (uint32_t h = (obj * 0x9e3779b1u) >> 19, n = 0; n < 64; n++, h = (h + 1) & (OBJ_STATES - 1)) {
        struct ObjState *o = &obj_states[h];
        if (o->obj == obj)
            return o;
        if (!free_slot && (!o->obj || o->seen + 1 < fade_gen))
            free_slot = o;
    }
    if (!free_slot)
        free_slot = &obj_states[(obj * 0x9e3779b1u) >> 19];
    *free_slot = (struct ObjState){obj, 0, 0, 0, 0, 0, 0};
    return free_slot;
}

/* The fade of an object 0x4185d0 just drew (as node `node`): near the end of its list or the
   edge of the object box by distance, and for a while after its model changed (the old model
   drawn too, in the other pixels) */
static int fade_of(uint32_t obj, uint32_t gem, uint32_t model, uint32_t *hi_out, uint32_t *split_out,
                   uint32_t *other_out, int32_t *depth_out);

static void object_fade(CPU *c, uint32_t obj, uint32_t gem, uint32_t node)
{
    uint32_t model = 0, nm = R32(NMODELS), hi, split, other;
    int32_t depth;
    for (uint32_t u = 0; u < nm && !model; u++)
        if (R32(MODELS + 12 * u + 4) == node)
            model = R32(MODELS + 12 * u);
    if (!model || !fade_of(obj, gem, model, &hi, &split, &other, &depth))
        return;
    st_faded++;
    if (split < hi && other && R32(other + MODEL_MESH) && add_model(c, obj, other, depth)) {
        fade_put(R32(obj + OBJ_POS), R32(obj + OBJ_POS + 4), R32(obj + OBJ_POS + 8), R32(other + MODEL_MESH),
                 VK_FADE(split, hi));
        fade_put(R32(obj + OBJ_POS), R32(obj + OBJ_POS + 4), R32(obj + OBJ_POS + 8), SHADOW_MARK, 1);
        st_twice++;
    } else
        split = hi;
    fade_put(R32(obj + OBJ_POS), R32(obj + OBJ_POS + 4), R32(obj + OBJ_POS + 8), R32(model + MODEL_MESH),
             VK_FADE(0, split));
}

/* An object's fade with `model` (as 0x4185d0 picked it): 0 if none; else its coverage (hi, of 256),
   and while it cross-fades from another model (other) the share the new one takes (split) */
static int fade_of(uint32_t obj, uint32_t gem, uint32_t model, uint32_t *hi_out, uint32_t *split_out,
                   uint32_t *other_out, int32_t *depth_out)
{
    int i = 0;
    while (i < 9 && R32(gem + LOD_MODEL + 4 * i) != model)
        i++;
    if (!model || i == 9)
        return 0;
    /* the object's centre round the camera, its depth and distance as 0x4185d0 has them */
    int32_t p[3];
    float cen[3], rot[9];
    for (int a = 0; a < 3; a++)
        cen[a] = (float)(int32_t)R32(obj + OBJ_CENTRE + 4 * a);
    for (int a = 0; a < 9; a++)
        rot[a] = RF32(obj + OBJ_ROT + 4 * a);
    static const uint32_t cam[3] = {CAM_X, CAM_Y, CAM_Z};
    for (int a = 0; a < 3; a++)
        p[a] = (int32_t)(R32(obj + OBJ_POS + 4 * a) - R32(cam[a])) +
               (int32_t)lrintf(cen[0] * rot[a] + cen[1] * rot[3 + a] + cen[2] * rot[6 + a]);
    float x = (float)p[0] / 65536.0f, y = (float)p[1] / 65536.0f, z = (float)p[2] / 65536.0f;
    float w = x * RF32(CAM_MATRIX + 0x0c) + y * RF32(CAM_MATRIX + 0x1c) + z * RF32(CAM_MATRIX + 0x2c) +
              RF32(CAM_MATRIX + 0x3c);
    double d = RF32(CAM_FOV) * RF32(LOD_SCALE) / 90.0 * w / 256.0;     /* cells, as the list's distances */
    /* the box's edge (with the object's radius) */
    /* The whole map: fade out before an object wraps to behind the camera at 128 cells */
    double m = fabs(p[0] / CELL) > fabs(p[2] / CELL) ? fabs(p[0] / CELL) : fabs(p[2] / CELL);
    double cov = obj_range >= 16 ? clamp01((124.0 - m) / 12.0) : 1, lod = 1;
    uint32_t dist = i < 8 ? R32(gem + LOD_DIST + 4 * i) : 0x7f000000u;
    if ((int32_t)dist < 0x7f000000 && !R32(gem + LOD_MODEL + 4 * (i + 1))) {   /* the end of the list */
        double D = (int32_t)dist / CELL;
        cov = fmin(cov, clamp01((D - d) / (0.15 * D)));
    }
    /* a model switch since the last view (by distance: in the same list; animated objects change
       lists): the old model too, for XFADE views */
    struct ObjState *o = obj_state(obj);
    /* The box (the game's moves in steps of 8 cells, its corner aligned): an object that comes
       into it, in its outer 16 cells, fades in over XFADE views */
    if (o->seen + 1 != fade_gen && !cam_cut && obj_range < 16 && m > box_half - 16)
        o->appear = fade_gen;
    else if (o->seen + 1 != fade_gen)
        o->appear = 0;
    if (o->appear && fade_gen - o->appear < XFADE)
        cov = fmin(cov, (fade_gen - o->appear + 1.0) / (XFADE + 1.0));
    if (o->seen + 1 != fade_gen || !o->model || cam_cut || o->gem != gem)
        o->prev = 0;
    else if (o->model != model) {
        if (ddlog > 1)                  /* INSANE_DD_LOG=2: each model switch */
            LOG(LOG_ALWAYS, "drawdist: frame %lu: a model switch at cell %.1f %.1f, %.1f cells away (screen %.2f %.2f)",
                frame_count, fmod((int32_t)R32(obj + OBJ_POS) / CELL + 256.0, 256.0),
                fmod((int32_t)R32(obj + OBJ_POS + 8) / CELL + 256.0, 256.0), d,
                w > 0 ? (x * RF32(CAM_MATRIX + 0x00) + y * RF32(CAM_MATRIX + 0x10) + z * RF32(CAM_MATRIX + 0x20) +
                         RF32(CAM_MATRIX + 0x30)) / w : 0.0,
                w > 0 ? (x * RF32(CAM_MATRIX + 0x04) + y * RF32(CAM_MATRIX + 0x14) + z * RF32(CAM_MATRIX + 0x24) +
                         RF32(CAM_MATRIX + 0x34)) / w : 0.0);
        uint32_t done = fade_gen - o->start;
        o->start = o->prev == model && done < XFADE ? fade_gen - (XFADE - done) : fade_gen;
        o->prev = o->model;
    }
    o->model = model;
    o->gem = gem;
    o->seen = fade_gen;
    uint32_t other = 0;
    if (o->prev && fade_gen - o->start < XFADE && R32(o->prev + MODEL_MESH) != R32(model + MODEL_MESH)) {
        other = o->prev;
        lod = (fade_gen - o->start + 1.0) / (XFADE + 1.0);
    }
    if (cov >= 1 && lod >= 1)
        return 0;
    *hi_out = (uint32_t)lround(cov * 256);
    *split_out = (uint32_t)lround(*hi_out * lod);
    *other_out = other;
    *depth_out = (int32_t)lrintf(w * 65536.0f);
    return 1;
}

/* 0x4185d0's view test, done first in C with a margin: with a bigger object box most objects
   fail it (half of them are behind the camera), and the game's own test of each costs. It
   rejects an object if the largest of its clip coordinates (x, y, z of CAM_MATRIX, times 65536)
   is past w plus its radius times OBJ_VIEW_SCALE plus 2 cells. Past 128 cells behind the camera
   the game's integers overflow and let some objects through (drawn where they cannot be seen);
   this test drops those too. */
#define OBJ_VIEW_SCALE  0x6d48          /* float at the game object 0xa3ca88 + this */
static int out_of_view(uint32_t obj)
{
    static const uint32_t cam[3] = {CAM_X, CAM_Y, CAM_Z};
    double p[3], cen = 0, m[16], spread = 0;
    for (int a = 0; a < 3; a++) {
        double v = (int32_t)R32(obj + OBJ_CENTRE + 4 * a) / 65536.0;
        p[a] = (int32_t)(R32(obj + OBJ_POS + 4 * a) - R32(cam[a])) / 65536.0;
        cen += v * v;
    }
    for (int i = 0; i < 16; i++)
        m[i] = RF32(CAM_MATRIX + 4 * (uint32_t)i);
    for (int i = 0; i < 12; i++)
        spread += fabs(m[i]);
    double cl[4];
    for (int col = 0; col < 4; col++)
        cl[col] = p[0] * m[col] + p[1] * m[4 + col] + p[2] * m[8 + col] + m[12 + col];
    uint32_t g = R32(0xa3ca88u);
    double cut = cl[3] + ((double)(int32_t)R32(obj + OBJ_RADIUS) * (g ? RF32(g + OBJ_VIEW_SCALE) : 0) + 33554432.0) / 65536.0;
    double margin = sqrt(cen) * spread + 16.0 + 0.01 * fabs(cut);
    return fabs(cl[0]) > cut + margin || fabs(cl[1]) > cut + margin || fabs(cl[2]) > cut + margin;
}

static int far_now(void);
static int far_take(CPU *c, uint32_t obj, uint32_t gem);
static void copy_add(uint32_t obj);
static uint32_t far_model(uint32_t obj, uint32_t gem, int32_t *depth, int *in_view);

/* 0x4185d0(object): picks the object's model by its distance from its type's list (.gem: at
   +0x14 the distances, at +0x34 the models) and puts it in this frame's lists. 145 of the game's
   598 lists end in no model: the object vanishes past its last distance, 64 cells for most
   buildings and trees, the original view's edge. With a longer view those last distances of 32
   cells and more move out with it (small things like grass tufts keep theirs). */
static int object_draw_hook(CPU *c)
{
    uint32_t obj = ARG(0), gem;
    if (!main_view || !obj || !(gem = R32(obj + OBJ_TYPE)) || (!level && !fading))
        return 0;
    if (far_now() && factor() >= 2.0f)
        copy_add(obj);                  /* (the map's copies: every object the box holds, seen or not) */
    if (level && out_of_view(obj)) {
        c->ret = R32(c->esp);           /* not drawn: return at once (cdecl) */
        c->esp += 4;
        return 1;
    }
    if (far_now() && far_take(c, obj, gem)) {
        c->ret = R32(c->esp);           /* drawn after the game's models (far_replay) */
        c->esp += 4;
        return 1;
    }
    uint32_t at = 0, keep = 0, n0 = R32(NNODES);
    for (uint32_t k = 0; k < 7 && level; k++) {
        uint32_t d = R32(gem + LOD_DIST + 4 * k);
        if (d >= 0x7f000000u)
            break;
        if (R32(gem + LOD_MODEL + 4 * (k + 1)))
            continue;
        if (d >= 32u << 24) {
            double n = d * (double)factor();
            at = gem + LOD_DIST + 4 * k;
            keep = d;
            W32(at, n < 0x7e000000 ? (uint32_t)n : 0x7e000000u);
        }
        break;
    }
    F_004185d0_orig(c);
    if (fading && R32(NNODES) == n0 + 1)
        object_fade(c, obj, gem, NODES + 8 * n0);
    if (at)
        W32(at, keep);
    return 1;
}

/* The fades at draw time. An instance is added to a batch (0x40aea0: its mesh's piece chosen by
   0x409390, its position at arg 4 round the camera) and drawn at once (0x40a770(slot)) or with
   the batch (0x40a770(-1): instance by instance, each after a call of 0x429b40 or 0x429980).
   The same hooks tell far_tap which mesh piece a draw is (the far objects' kept meshes). */
static uint32_t cur_mesh, cur_part;
static uint32_t slot_fade[64], slot_mesh[64], slot_part[64];
static int flushing, flush_slot, draw_slot = -1;
static int far_on;          /* the objects past the original box drawn from kept meshes */
static int far_near;        /* INSANE_FAR_NEAR=1: the objects inside it too (an experiment) */
static unsigned long draw_seq;  /* a count of the instances drawn (far_tap: a second draw of one is a second pass) */
static void far_learn_part(uint32_t mesh, uint32_t part);
static void far_batched(uint32_t mesh, uint32_t part, uint32_t piece);

static int far_now(void)
{
    return far_on && level && main_view;
}

static int mesh_piece_hook(CPU *c)
{
    if (fading || far_now()) {
        cur_mesh = ARG(0);
        cur_part = ARG(1);
        if (far_now())
            far_learn_part(cur_mesh, cur_part);
    }
    return 0;
}

static int batch_add_hook(CPU *c)
{
    uint32_t slot = R32(BATCH_N);
    if (far_now() && slot < 64) {
        slot_mesh[slot] = cur_mesh;
        slot_part[slot] = cur_part;
        far_batched(cur_mesh, cur_part, R32(PIECE_CUR));
    }
    if (!fading)
        return 0;
    uint32_t rel = ARG(4);
    uint32_t f = rel ? fade_get(R32(rel) + R32(CAM_X), R32(rel + 4) + R32(CAM_Y), R32(rel + 8) + R32(CAM_Z), cur_mesh)
                     : 0;
    if (slot < 64)
        slot_fade[slot] = f;
    st_hits += f != 0;
    vk_fade = f;
    F_0040aea0_orig(c);
    vk_fade = 0;
    return 1;
}

static int batch_draw_hook(CPU *c)
{
    int32_t a = (int32_t)ARG(0);
    if ((!fading && !far_now()) || a == -2)
        return 0;
    if (a == -1) {
        flushing = 1;
        flush_slot = 0;
        draw_slot = -1;
    } else {
        draw_seq++;
        draw_slot = a < 64 ? a : -1;
        if (fading)
            vk_fade = (uint32_t)a < 64 ? slot_fade[a] : 0;
    }
    F_0040a770_orig(c);
    flushing = 0;
    draw_slot = -1;
    vk_fade = 0;
    return 1;
}

static int slot_light_hook(CPU *c)
{
    (void)c;
    if (flushing) {
        if (fading)
            vk_fade = flush_slot < 64 ? slot_fade[flush_slot] : 0;
        draw_slot = flush_slot < 64 ? flush_slot : -1;
        flush_slot++;
        draw_seq++;
    }
    return 0;
}

/* ------------------------------------------------------------------ far objects
   Past the original object box the game's own path is costly for what it draws (DRAW DISTANCE 4X
   had ~750 batch adds and ~125,000 vertices copied a frame, against ~230 and ~37,000 at
   ORIGINAL): the objects there are drawn from kept meshes instead, as the far terrain is. A mesh is
   kept the first time the game draws it: each of its parts' draw (the device state, its vertices
   in the mesh's own space and its indices, as the batch has them: the game's per-vertex copy of
   the mesh, 0x40aea0's mode 1). An object past the box whose mesh is kept is then left out of the
   game's lists (0x4185d0 not run) and drawn after the game's models (0x418d40) with its own world
   matrix and lights (the game's 0x429980 run for it, as 0x40a770 does: the sun by the light map's
   shade, nearby lamps) and its fade. It still goes into the see-through parts' list (0x418c60), so
   its glass is drawn, sorted, by the game. Left to the game: lamps (they light others), animated
   objects and models, objects drawn part by part, meshes with sprites or lines (camera-facing:
   modes 4, 5), meshes the game draws already placed (some small ones: their vertices round the
   camera, the world matrix none), and any mesh not kept yet (it is kept as the game draws it). An
   object outside the game's own view test is not drawn, as the game. The game's env-mapped second
   draw of a piece (its texture coordinates follow the camera) is not kept: far objects show their
   base pass only. Shadows fall on far objects; they cast none and are in no reflection
   (vk_nocast: the ray-tracing structures cost more than they would show out there). INSANE_FAR=0:
   none (to compare); INSANE_FAR_LOG=1 logs the kept meshes. INSANE_FAR_NEAR=1 (an experiment, off):
   the objects inside the box drawn so too, casting shadows; there a mesh drawn twice for one
   instance (a shiny second pass) and a cross-fade whose other model is not kept stay the game's. */
#define F_LIGHT_LIT     0x429980u       /* (rel, centre, radius, shade): an instance's lights */
#define F_LIGHT_UNLIT   0x429b40u
#define F_SHADE         0x401270u       /* (x, z): the terrain's light map; bits 16-23 the shade */
#define SHADE_ON        0x9eed54u       /* ... used (else 255) */
#define FAR_MESHES      4096
#define FAR_PARTS       16
#define FAR_MAX         4096            /* far instances a view */
#define INST_COPIES_MAX 16384           /* the map's copies a view (far_copies) */

typedef struct {
    D3DState st;
    int prim, nv, ni;
    uint8_t *raw;
    uint16_t *ix;
} FarDraw;
typedef struct {
    uint32_t part;
    int batched, got;
    unsigned long seq;          /* the instance its draw was kept from (draw_seq) */
    FarDraw d;
} FarPart;
typedef struct {
    uint32_t mesh;
    int state;                  /* 0 being learnt, 1 kept, -1 left to the game */
    unsigned long frame;        /* the frame it was learnt in */
    int twice;                  /* a part drawn twice for one instance (a shiny pass: the near ones are the game's) */
    int nparts;
    FarPart p[FAR_PARTS];
} FarMesh;
static FarMesh *far_meshes[FAR_MESHES];
static unsigned long st_far, st_far_kept, st_far_game, st_far_left;     /* INSANE_FAR_LOG */

#define SHADES 8192
static struct { uint32_t obj, shade; } shades[SHADES];  /* far_shade's */

typedef struct {
    uint32_t obj;
    FarMesh *m, *other;         /* its mesh, and the one it cross-fades from */
    uint32_t fade, other_fade;
    int near;                   /* inside the original box (INSANE_FAR_NEAR): it casts shadows */
} FarInst;
static FarInst far_list[FAR_MAX];
static int far_n;

static FarMesh *far_find(uint32_t mesh, int create)
{
    for (uint32_t h = (mesh * 0x9e3779b1u) >> 20, n = 0; n < FAR_MESHES; n++, h = (h + 1) & (FAR_MESHES - 1)) {
        FarMesh *m = far_meshes[h];
        if (m && m->mesh == mesh)
            return m;
        if (!m) {
            if (!create)
                return NULL;
            m = calloc(1, sizeof *m);
            if (!m)
                fatal("drawdist: out of memory");
            m->mesh = mesh;
            far_meshes[h] = m;
            return m;
        }
    }
    return NULL;
}

static void far_forget(FarMesh *m)
{
    for (int i = 0; i < m->nparts; i++) {
        free(m->p[i].d.raw);
        free(m->p[i].d.ix);
    }
    memset(m->p, 0, sizeof m->p);
    m->nparts = 0;
    m->state = 0;
    m->twice = 0;
}

/* every kept mesh gone (a race starts: the game's models and textures are loaded anew) */
static void far_reset(void)
{
    for (int i = 0; i < FAR_MESHES; i++)
        if (far_meshes[i]) {
            far_forget(far_meshes[i]);
            free(far_meshes[i]);
            far_meshes[i] = NULL;
        }
    memset(shades, 0, sizeof shades);
}

/* a surface going: the kept meshes drawn with it are learnt again */
static void tile_keep(int t, const Surface *s);
static Surface *tile_seen[16], *tile_kept[16];

void drawdist_surface_gone(const Surface *s)
{
    for (int t = 0; t < 16; t++)            /* a colour tile's texture let go: ours kept */
        if (tile_seen[t] == s) {
            tile_keep(t, s);
            tile_seen[t] = NULL;
        }
    for (int i = 0; i < FAR_MESHES; i++) {
        FarMesh *m = far_meshes[i];
        for (int k = 0; m && m->state >= 0 && k < m->nparts; k++)
            for (int t = 0; t < STAGES; t++)
                if (m->p[k].d.st.tex[t] == s || m->p[k].d.st.rt == s || m->p[k].d.st.zb == s) {
                    far_forget(m);
                    goto next;
                }
    next:;
    }
}

static FarPart *far_part(FarMesh *m, uint32_t part, int add)
{
    for (int i = 0; i < m->nparts; i++)
        if (m->p[i].part == part)
            return &m->p[i];
    if (!add)
        return NULL;
    if (m->nparts == FAR_PARTS) {
        m->state = -1;
        return NULL;
    }
    m->p[m->nparts].part = part;
    return &m->p[m->nparts++];
}

/* 0x409390(mesh, part): a part of a mesh the game draws: learnt in the frame it is first drawn */
static void far_learn_part(uint32_t mesh, uint32_t part)
{
    FarMesh *m = far_find(mesh, 1);
    if (!m || m->state != 0)
        return;
    if (m->frame != frame_count) {
        far_forget(m);
        m->frame = frame_count;
    }
    far_part(m, part, 1);
}

/* 0x40aea0: an instance of the part added to a batch: only a mesh's own triangles are kept */
static void far_batched(uint32_t mesh, uint32_t part, uint32_t piece)
{
    FarMesh *m = far_find(mesh, 0);
    FarPart *p = m && m->state == 0 ? far_part(m, part, 0) : NULL;
    if (!p)
        return;
    p->batched = 1;
    if (!piece || R32(piece + 0x68) != 1)
        m->state = -1;
}

/* d3d.c: a GPU-lit draw of the game's: the first of a part being learnt is kept */
static void far_tap(const D3DState *st, int prim, const void *raw, int n, const uint16_t *ix, int ni)
{
    if (!far_now() || draw_slot < 0)
        return;
    FarMesh *m = far_find(slot_mesh[draw_slot], 0);
    FarPart *p = m && m->state == 0 && m->frame == frame_count ? far_part(m, slot_part[draw_slot], 0) : NULL;
    if (p && p->got && p->seq == draw_seq)
        m->twice = 1;
    if (!p || p->got)
        return;
    const Mat4 *w = &st->xf[XF_WORLD];
    if (w->m[3][0] == 0 && w->m[3][1] == 0 && w->m[3][2] == 0) {
        m->state = -1;                  /* (its vertices placed by the game, round the camera: not the mesh's own) */        return;
    }
    FarDraw *d = &p->d;
    d->st = *st;
    d->prim = prim;
    d->nv = n;
    d->ni = ni;
    d->raw = malloc((size_t)n * 40);
    d->ix = ni ? malloc((size_t)ni * 2) : NULL;
    if (!d->raw || (ni && !d->ix))
        fatal("drawdist: out of memory");
    memcpy(d->raw, raw, (size_t)n * 40);
    if (ni)
        memcpy(d->ix, ix, (size_t)ni * 2);
    p->got = 1;
    p->seq = draw_seq;
}

/* after the game's models: the meshes learnt this frame with every part kept are ready */
static void far_learnt(void)
{
    for (int i = 0; i < FAR_MESHES; i++) {
        FarMesh *m = far_meshes[i];
        if (!m || m->state != 0 || m->frame != frame_count || !m->nparts)
            continue;
        int ok = 1, any = 0;
        for (int k = 0; k < m->nparts; k++) {
            if (m->p[k].batched && !m->p[k].got)
                ok = 0;
            any |= m->p[k].got;
        }
        if (ok && any) {
            m->state = 1;
            if (far_log)
                LOG(LOG_ALWAYS, "drawdist: far objects: mesh %08x kept, %d parts (frame %lu)", m->mesh, m->nparts,
                    frame_count);
        }
    }
}

/* The model 0x4185d0 would pick for the object (its distance list by the depth of its centre; the
   last distance moved out as object_draw_hook moves it), and that depth (the see-through list's) */
static uint32_t far_model(uint32_t obj, uint32_t gem, int32_t *depth, int *in_view)
{
    static const uint32_t cam[3] = {CAM_X, CAM_Y, CAM_Z};
    float rot[9];
    for (int a = 0; a < 9; a++)
        rot[a] = RF32(obj + OBJ_ROT + 4 * a);
    double s = RF32(0x4bf380u), k360 = RF32(0x4bf360u), cl[4];
    for (int col = 0; col < 4; col++)
        cl[col] = RF32(CAM_MATRIX + 0x30 + 4 * (uint32_t)col);
    for (int a = 0; a < 3; a++) {
        float cen = 0;
        for (int b = 0; b < 3; b++)
            cen += (float)(int32_t)R32(obj + OBJ_CENTRE + 4 * b) * rot[3 * b + a];
        int32_t p = (int32_t)(R32(obj + OBJ_POS + 4 * a) - R32(cam[a])) + (int32_t)lrintf(cen);
        for (int col = 0; col < 4; col++)
            cl[col] += (double)(float)(p * s) * RF32(CAM_MATRIX + 0x10 * (uint32_t)a + 4 * (uint32_t)col);
    }
    double w54 = trunc(cl[3] * k360);
    *depth = (int32_t)w54;
    /* the game's view test: each clip coordinate within w plus the radius times the view's scale */
    uint32_t g = R32(0xa3ca88u);
    double lim = trunc((float)(int32_t)R32(obj + OBJ_RADIUS) * (double)(g ? RF32(g + OBJ_VIEW_SCALE) : 0)) + w54;
    *in_view = 1;
    for (int a = 0; a < 3; a++)
        if (fabs(trunc(cl[a] * k360)) > lim)
            *in_view = 0;
    double d = trunc((double)RF32(CAM_FOV) * RF32(LOD_SCALE) * RF32(0x4bf3d8u) * w54 * RF32(0x4bf3d4u));
    int32_t dv = d > 0x1fffff ? 0x1fffff : (int32_t)d;
    int i = 0;
    for (; i < 8; i++) {
        uint32_t dist = R32(gem + LOD_DIST + 4 * i);
        if (i < 7 && dist < 0x7f000000u && !R32(gem + LOD_MODEL + 4 * (i + 1)) && dist >= 32u << 24) {
            double n = dist * (double)factor();
            dist = n < 0x7e000000 ? (uint32_t)n : 0x7e000000u;
        }
        if (((int32_t)dist >> 10) >= dv)
            break;
    }
    return R32(gem + LOD_MODEL + 4 * (uint32_t)i);
}

/* 0x4185d0's work for an object past the original box whose mesh is kept: it goes into far_list
   (and the see-through list); 1 if so (the game's own then not run) */
/* An object's rotation (OBJ_ROT) from its angles, as 0x4185d0 makes it the first time it draws the
   object (flag 0x400 then): the objects the game never drew have none yet */
static void obj_rot(CPU *c, uint32_t obj)
{
    if (R32(obj + OBJ_FLAGS) & 0x400u)
        return;
    guest_call(c, 0x4804d0u, 2, obj + 0x48, obj + OBJ_ROT);
    W32(obj + OBJ_FLAGS, R32(obj + OBJ_FLAGS) | 0x400u);
}

static int far_take(CPU *c, uint32_t obj, uint32_t gem)
{
    uint32_t fl = R32(obj + OBJ_FLAGS);
    double m = fmax(fabs((int32_t)(R32(obj + OBJ_POS) - R32(CAM_X)) / CELL),
                    fabs((int32_t)(R32(obj + OBJ_POS + 8) - R32(CAM_Z)) / CELL));
    int near = m <= vis_range() * 8.0 + 2;      /* (the original box, and a margin) */
    if (near && !far_near)
        return 0;
    if (fl & (0x2000u | 0x100u | 1u) || R32(obj + 0x98)) {
        st_far_left++;
        return 0;
    }
    obj_rot(c, obj);                    /* (far_model's centre and the world matrix need it) */
    int32_t depth;
    int vis;
    uint32_t model = far_model(obj, gem, &depth, &vis);
    if (!model || !vis)
        return 1;                       /* (past its list's end, or out of view: none, as the game) */
    /* (models whose flags 1, 4, 8 animate them only do with the object's animation, +0x98, left to the
       game above; flag 2, 0x49c350, bends the model once a frame by the same: without it the kept
       shape is the model's) */
    uint32_t mesh = R32(model + MODEL_MESH);
    if (!mesh)
        return 0;
    FarMesh *fm = far_find(mesh, 0);
    if (!fm || fm->state != 1 || far_n >= FAR_MAX || (near && fm->twice)) {
        if (fm && (fm->state < 0 || (near && fm->twice)))
            st_far_left++;
        else
            st_far_game++;
        return 0;
    }
    FarInst fi = {obj, fm, NULL, 0, 0, near};
    uint32_t hi, split, other;
    int32_t fdepth;
    if (fading && fade_of(obj, gem, model, &hi, &split, &other, &fdepth)) {
        FarMesh *om = split < hi && other ? far_find(R32(other + MODEL_MESH), 0) : NULL;
        if (om && om->state == 1 && om != fm && !(near && om->twice)) {   /* cross-fading from its other model */
            fi.other = om;
            fi.other_fade = VK_FADE(split, hi);
            fi.fade = VK_FADE(0, split);
        } else if (near && split < hi && other && om != fm) {
            st_far_game++;              /* (near, its other model not kept: the game cross-fades it) */
            return 0;
        } else
            fi.fade = VK_FADE(0, hi);
    }
    far_list[far_n++] = fi;
    W32(mesh + 0x70, 0);
    guest_call(c, F_DEPTH_ADD, 4, (uint32_t)depth, obj, mesh, 0u);
    st_far++;
    return 1;
}

/* An object's shade (the terrain light map at its place, 0x401270's bits 16-23; 255 without it),
   kept by object: they do not move */
static uint32_t far_shade(CPU *c, uint32_t obj)
{
    if (!R32(SHADE_ON))
        return 255;
    uint32_t h = (obj * 0x9e3779b1u) >> 19;
    if (shades[h].obj == obj)
        return shades[h].shade;
    uint32_t s = guest_call(c, F_SHADE, 2, R32(obj + OBJ_POS), R32(obj + OBJ_POS + 8)) >> 16 & 255;
    shades[h].obj = obj;
    shades[h].shade = s;
    return s;
}

/* a far instance's draws: one mesh and fade (an object cross-fading has two) */
typedef struct {
    FarMesh *m;
    int inst;
    uint32_t fade;
} FarItem;
static FarItem far_items[2 * FAR_MAX];

static int far_item_cmp(const void *a, const void *b)
{
    const FarItem *x = a, *y = b;
    return x->m != y->m ? (x->m < y->m ? -1 : 1) : x->inst - y->inst;
}

/* The far instances drawn (after the game's models), mesh by mesh: each part's kept state with
   this frame's view, fog and lights, and the object's world matrix and sun as 0x40a770 sets them up
   (0x429980: the light list's sun times the shade / 255; nearby lamps too, so with lamps in the list
   the game's own function is run for each) */
static void far_replay(CPU *c)
{
    const D3DState *base = d3d_last_state();
    if (!base || !far_n) {
        far_n = 0;
        return;
    }
    static uint32_t scratch;
    if (!scratch)
        scratch = galloc(32);
    static D3DState st;
    static Mat4 world[FAR_MAX];
    static float sun[FAR_MAX][3];
    static uint8_t lit[FAR_MAX];
    static const uint32_t cam[3] = {CAM_X, CAM_Y, CAM_Z};
    float s = RF32(0x4bf380u), k255 = RF32(0x4bf424u);
    uint32_t game = R32(0xa3ca88u);
    int lamps = game && R32(game + 0x3d8) > 1;  /* (the list's first is the sun) */
    int nitems = 0, lights_set = 0;
    for (int i = 0; i < far_n; i++) {
        FarInst *f = &far_list[i];
        uint32_t obj = f->obj;
        int32_t rel[3];
        for (int a = 0; a < 3; a++)
            rel[a] = (int32_t)(R32(obj + OBJ_POS + 4 * a) - R32(cam[a]));
        Mat4 *w = &world[i];                    /* the object's rotation, its place round the camera */
        memset(w, 0, sizeof *w);
        for (int r = 0; r < 3; r++)             /* (as the game's: OBJ_ROT's rows) */
            for (int k = 0; k < 3; k++)
                w->m[r][k] = RF32(obj + OBJ_ROT + 4 * (uint32_t)(3 * r + k));
        for (int k = 0; k < 3; k++)
            w->m[3][k] = (float)rel[k] * s;
        w->m[3][3] = 1;
        lit[i] = !(R32(obj + OBJ_FLAGS) & 8);
        if (lit[i] && game) {
            float f255 = (float)(far_shade(c, obj) * (double)k255);
            for (int k = 0; k < 3; k++)
                sun[i][k] = (float)(RF32(game + 0x3dc + 4 + 4 * (uint32_t)k) * (double)f255);
            if (!lights_set && !lamps) {        /* the device's lights as 0x429980 leaves them, once */
                for (int a = 0; a < 3; a++) {
                    W32(scratch + 4 * (uint32_t)a, (uint32_t)rel[a]);
                    W32(scratch + 12 + 4 * (uint32_t)a, 0);
                }
                guest_call(c, F_LIGHT_LIT, 4, scratch, scratch + 12, 0u, 255u);
                lights_set = 1;
            }
        }
        far_items[nitems++] = (FarItem){f->m, i, f->fade};
        if (f->other)
            far_items[nitems++] = (FarItem){f->other, i, f->other_fade};
    }
    qsort(far_items, (size_t)nitems, sizeof *far_items, far_item_cmp);
    for (int a = 0; a < nitems;) {
        FarMesh *m = far_items[a].m;
        int b = a;
        while (b < nitems && far_items[b].m == m)
            b++;
        for (int k = 0; k < m->nparts; k++) {
            const FarDraw *d = &m->p[k].d;
            if (!m->p[k].got)
                continue;
            st = d->st;                         /* the part's state, this frame's view, fog and lights */
            st.xf[XF_VIEW] = base->xf[XF_VIEW];
            st.xf[XF_PROJ] = base->xf[XF_PROJ];
            st.rs[RS_AMBIENT] = base->rs[RS_AMBIENT];
            for (int r = RS_FOGENABLE; r <= RS_FOGDENSITY; r++)
                st.rs[r] = base->rs[r];
            st.rs[RS_FOGVERTEXMODE] = base->rs[RS_FOGVERTEXMODE];
            st.rs[RS_RANGEFOGENABLE] = base->rs[RS_RANGEFOGENABLE];
            st.vp = base->vp;
            st.rt = base->rt;
            st.zb = base->zb;
            if (!lamps)
                memcpy(st.lights, base->lights, sizeof st.lights);
            for (int j = a; j < b; j++) {
                int i = far_items[j].inst;
                uint32_t obj = far_list[i].obj;
                st.xf[XF_WORLD] = world[i];
                st.rs[RS_LIGHTING] = lit[i];
                if (lit[i] && lamps) {          /* lamps in the list: the game's own lights for it */
                    uint32_t mesh = m->mesh;
                    for (int q = 0; q < 3; q++) {
                        W32(scratch + 4 * (uint32_t)q, (uint32_t)(int32_t)(R32(obj + OBJ_POS + 4 * (uint32_t)q) - R32(cam[q])));
                        W32(scratch + 12 + 4 * (uint32_t)q,
                            (uint32_t)(int32_t)trunc(0.5 + RF32(mesh + 0xbc + 4 * (uint32_t)q)));
                    }
                    float radius = (float)(int32_t)trunc(RF32(mesh + 0xc8));
                    uint32_t rb;
                    memcpy(&rb, &radius, 4);
                    guest_call(c, F_LIGHT_LIT, 4, scratch, scratch + 12, rb, far_shade(c, obj));
                    memcpy(st.lights, base->lights, sizeof st.lights);
                } else if (lit[i])
                    memcpy(st.lights[0].diffuse, sun[i], sizeof sun[i]);
                vk_fade = far_items[j].fade;
                vk_nocast = !far_list[i].near;  /* (shadows fall on far ones; they cast none: out there, the
                                                   ray-tracing structures would cost more than they show) */
                if (far_log < 2)                /* (INSANE_FAR_LOG=2: taken, not drawn: where they are) */
                    d3d_tnl_draw(&st, d->prim, d->raw, d->nv, d->ix, d->ni);
                st_far_kept++;
            }
        }
        a = b;
    }
    vk_fade = 0;
    vk_nocast = 0;
    far_n = 0;
}

/* ------------------------------------------------------------------ the map's copies
   The map wraps every 256 cells and the game draws each object once, where it is nearest (out to
   128 cells, faded out by 124). Past that, out to the far plane (2X 156 cells, 3X 234, 4X 312),
   the objects of the map's copies round it are drawn too (Ant approved, 2026-10-19: "repeats with
   trees and buildings"): each static object the game has put in a view (copy_add) is kept in a
   16 x 16 grid of the map, each square's list biggest first; each view, for the copies round the
   one the camera is in (the 8 next to it; AUTO's 8X the next rings too) in reach and in view, the
   grid's squares in reach, in view, with something big enough to see and (AUTO) not wholly behind
   the ground, then their objects in reach, in view and big enough to see (their radius over
   COPY_MIN_PX pixels: small things drop out at long range, trees, buildings and rocks stay) are drawn
   with their model's last (lowest) level of detail, if its mesh is kept, all copies of a mesh in
   one instanced draw (vk_draw_tnl_inst) with their own world matrix and sun (the shade as theirs),
   faded in from 128 cells and out towards the far plane with DISTANCE FADE. Not lit by lamps; not
   ray-traced casters (past the shadows' reach). INSANE_FAR_COPIES=0: none. */
#define COPY_OBJS       16384
#define COPY_GRID       16              /* squares a side, of 16 cells */
#define COPY_MIN_PX     1.25            /* the radius in pixels below which a copy is left out */
static uint32_t copy_obj[COPY_OBJS];
static int ncopy;
static uint16_t copy_next[COPY_OBJS];   /* the grid's lists (index + 1; 0 the end) */
static uint16_t copy_head[COPY_GRID * COPY_GRID];
static uint32_t copy_pos[COPY_OBJS][3];  /* its place (16.16 units) and radius (units), kept */
static double copy_rad[COPY_OBJS];
static double copy_maxrad[COPY_GRID * COPY_GRID];    /* the biggest object's radius in a square, units */
static int32_t copy_y[COPY_GRID * COPY_GRID][2];     /* its objects' lowest and highest place (16.16) */
static uint32_t copy_set[2 * COPY_OBJS]; /* (open addressing: the objects in the list) */
static int copies_on = 1;
static unsigned long st_copies, st_copy_draws;

static void copy_reset(void)
{
    ncopy = 0;
    memset(copy_head, 0, sizeof copy_head);
    memset(copy_maxrad, 0, sizeof copy_maxrad);
    memset(copy_set, 0, sizeof copy_set);
}

static int copy_square(uint32_t v)      /* a 16.16 map coordinate's grid column (or row) */
{
    return (int)((((int32_t)v >> 24) + 128) & 255) >> 4;
}

/* an object the game is drawing (0x4185d0): kept in the list if it can have copies */
static void copy_add(uint32_t obj)
{
    if (ncopy >= COPY_OBJS || R32(obj + OBJ_FLAGS) & (0x2000u | 0x100u | 1u | 8u) || R32(obj + 0x98) ||
        !R32(obj + OBJ_TYPE))
        return;
    for (uint32_t h = (obj * 0x9e3779b1u) >> 17, n = 0; n < 2 * COPY_OBJS; n++, h = (h + 1) & (2 * COPY_OBJS - 1)) {
        if (copy_set[h] == obj)
            return;
        if (!copy_set[h]) {
            copy_set[h] = obj;
            int sq = copy_square(R32(obj + OBJ_POS + 8)) * COPY_GRID + copy_square(R32(obj + OBJ_POS));
            double rad = (int32_t)R32(obj + OBJ_RADIUS) / 65536.0;
            int32_t y = (int32_t)R32(obj + OBJ_POS + 4);
            copy_obj[ncopy] = obj;
            for (int a = 0; a < 3; a++)
                copy_pos[ncopy][a] = R32(obj + OBJ_POS + 4 * (uint32_t)a);
            copy_rad[ncopy] = rad;
            if (!copy_head[sq] || y < copy_y[sq][0])
                copy_y[sq][0] = y;
            if (!copy_head[sq] || y > copy_y[sq][1])
                copy_y[sq][1] = y;
            copy_maxrad[sq] = fmax(copy_maxrad[sq], rad);
            /* the square's list biggest first (far_copies stops at the first too small to see) */
            uint16_t *at = &copy_head[sq];
            while (*at && copy_rad[*at - 1] >= rad)
                at = &copy_next[*at - 1];
            copy_next[ncopy] = *at;
            *at = (uint16_t)(ncopy + 1);
            ncopy++;
            return;
        }
    }
}

/* the mesh of a type's last level of detail, if kept */
static FarMesh *copy_mesh(uint32_t gem)
{
    uint32_t model = 0;
    for (int i = 0; i < 9 && R32(gem + LOD_MODEL + 4 * (uint32_t)i); i++)
        model = R32(gem + LOD_MODEL + 4 * (uint32_t)i);
    uint32_t mesh = model ? R32(model + MODEL_MESH) : 0;
    FarMesh *m = mesh ? far_find(mesh, 0) : NULL;
    return m && m->state == 1 ? m : NULL;
}

typedef struct {
    FarMesh *m;
    float r[16];
} CopyRec;
static CopyRec copy_recs[INST_COPIES_MAX];

static int copy_cmp(const void *a, const void *b)
{
    const CopyRec *x = a, *y = b;
    return x->m != y->m ? (x->m < y->m ? -1 : 1) : 0;
}

static void far_copies(CPU *c)
{
    const D3DState *base = d3d_last_state();
    uint32_t game = R32(0xa3ca88u);
    if (!copies_on || !base || !game || factor() < 2.0f || !ncopy)
        return;
    double reach = RACE_FAR * factor() / 256.0;              /* cells */
    double cam[3] = {(int32_t)R32(CAM_X) / CELL, (int32_t)R32(CAM_Y) / CELL, (int32_t)R32(CAM_Z) / CELL};
    double m[16], spread = 0;
    for (int i = 0; i < 16; i++)
        m[i] = RF32(CAM_MATRIX + 4 * (uint32_t)i);
    for (int i = 0; i < 12; i++)
        spread += fabs(m[i]);
    const float *P = &base->xf[XF_PROJ].m[0][0];
    double focal = 0.5 * fabs(P[5]) * base->vp.h;           /* (the game's pixels: its 640 x 480) */
    float k255 = RF32(0x4bf424u);
    int n = 0, ring = (int)ceil((reach + 128) / 256);        /* (AUTO's 8X: three rings of copies) */
    for (int oz = -ring; oz <= ring; oz++)
        for (int ox = -ring; ox <= ring; ox++) {
            if (!ox && !oz)
                continue;
            {   /* the copy (256 cells a side, round the camera's square) in reach and in view at all */
                double x0 = 256.0 * ox - 128, z0 = 256.0 * oz - 128;
                double dx = fmax(fmax(x0, -(x0 + 256)), 0), dz = fmax(fmax(z0, -(z0 + 256)), 0);
                if (dx * dx + dz * dz > reach * reach)
                    continue;
                double ps[3] = {(x0 + 128) * 256.0, 0, (z0 + 128) * 256.0}, rs = 182.0 * 256.0, cl[4];
                for (int col = 0; col < 4; col++)
                    cl[col] = ps[0] * m[col] + ps[1] * m[4 + col] + ps[2] * m[8 + col] + m[12 + col];
                double cut = cl[3] + rs + 0.01 * fabs(cl[3]) + rs * spread;
                if (cl[3] < -rs || fabs(cl[0]) > cut || fabs(cl[1]) > cut)
                    continue;
            }
            for (int sz = 0; sz < COPY_GRID; sz++)
                for (int sx = 0; sx < COPY_GRID; sx++) {
                    /* the square's centre round the camera in this copy (cells) */
                    double qx = fmod(sx * 16.0 - 128 + 8 - cam[0] + 384.0, 256.0) - 128 + 256.0 * ox;
                    double qz = fmod(sz * 16.0 - 128 + 8 - cam[2] + 384.0, 256.0) - 128 + 256.0 * oz;
                    int sq = sz * COPY_GRID + sx;
                    double qd = sqrt(qx * qx + qz * qz), mr = copy_maxrad[sq], near = fmax(qd - 12, 1) * 256.0;
                    if (qd - 12 > reach || !copy_head[sq] || mr * focal < COPY_MIN_PX * near)
                        continue;           /* out of reach, or nothing in it big enough to see */
                    {   /* the square in view (its objects' sphere, as out_of_view tests an object) */
                        double y0 = (int32_t)(copy_y[sq][0] - (int32_t)R32(CAM_Y)) / 65536.0;
                        double y1 = (int32_t)(copy_y[sq][1] - (int32_t)R32(CAM_Y)) / 65536.0;
                        double ps[3] = {qx * 256.0, 0.5 * (y0 + y1), qz * 256.0}, rs = 11.32 * 256.0 + mr + 0.5 * (y1 - y0);
                        double cl[4];
                        for (int col = 0; col < 4; col++)
                            cl[col] = ps[0] * m[col] + ps[1] * m[4 + col] + ps[2] * m[8 + col] + m[12 + col];
                        double cut = cl[3] + rs + 16.0 + 0.01 * fabs(cl[3]) + rs * spread;
                        if (cl[3] < -rs || fabs(cl[0]) > cut || fabs(cl[1]) > cut)
                            continue;
                    }
                    if (hidden(qx, qz, 12 + mr / 256.0, copy_y[sq][1] / 65536.0 + 2 * mr))
                        continue;           /* all of it behind the ground before it (AUTO) */
                    /* ... or all of it over the ground before it, even the lowest at its far side */
                    int test = !all_shown(qx, qz, 12, copy_y[sq][0] / 65536.0);
                    for (int i = copy_head[sq]; i; i = copy_next[i - 1]) {
                        uint32_t obj = copy_obj[i - 1];
                        double rad = copy_rad[i - 1];
                        if (rad * focal < COPY_MIN_PX * near)
                            break;              /* this and the rest too small to see from the square */
                        double p[3];            /* units round the camera */
                        p[0] = (int32_t)(copy_pos[i - 1][0] - R32(CAM_X)) / 65536.0 + 65536.0 * ox;
                        p[1] = (int32_t)(copy_pos[i - 1][1] - R32(CAM_Y)) / 65536.0;
                        p[2] = (int32_t)(copy_pos[i - 1][2] - R32(CAM_Z)) / 65536.0 + 65536.0 * oz;
                        double d = sqrt(p[0] * p[0] + p[2] * p[2]) / 256.0, mx = fmax(fabs(p[0]), fabs(p[2])) / 256.0;
                        if (d > reach || mx < 124)
                            continue;
                        if (rad * focal < COPY_MIN_PX * d * 256.0)
                            continue;           /* too small to see from here */
                        double cl[4];           /* in view (as out_of_view: its radius and a margin) */
                        for (int col = 0; col < 4; col++)
                            cl[col] = p[0] * m[col] + p[1] * m[4 + col] + p[2] * m[8 + col] + m[12 + col];
                        double cut = cl[3] + rad + 16.0 + 0.01 * fabs(cl[3]) + rad * spread;
                        if (cl[3] < -rad || fabs(cl[0]) > cut || fabs(cl[1]) > cut)
                            continue;
                        if (test && hidden(p[0] / 256.0, p[2] / 256.0, rad / 256.0, (int32_t)copy_pos[i - 1][1] / 65536.0 + 2 * rad))
                            continue;           /* (AUTO: behind the ground before it) */
                        FarMesh *fm = copy_mesh(R32(obj + OBJ_TYPE));
                        if (!fm || n >= INST_COPIES_MAX)
                            continue;
                        CopyRec *r = &copy_recs[n++];
                        r->m = fm;
                        obj_rot(c, obj);
                        for (int row = 0; row < 3; row++) {     /* (OBJ_ROT's columns: tnl.vert's rows) */
                            for (int k = 0; k < 3; k++)
                                r->r[4 * row + k] = RF32(obj + OBJ_ROT + 4 * (uint32_t)(3 * k + row));
                            r->r[4 * row + 3] = (float)p[row];
                        }
                        r->r[12] = (float)(far_shade(c, obj) * (double)k255);
                        uint32_t fade = 0;
                        if (fading) {           /* in from 128 cells, out towards the far plane */
                            double cov = fmin(clamp01((mx - 128.0) / 12.0), clamp01((reach - d) / (0.1 * reach)));
                            if (cov < 1)
                                fade = VK_FADE(0, (uint32_t)lround(cov * 256));
                        }
                        memcpy(&r->r[13], &fade, 4);
                        r->r[14] = r->r[15] = 0;
                    }
                }
        }
    if (!n)
        return;
    qsort(copy_recs, (size_t)n, sizeof *copy_recs, copy_cmp);
    /* the sun at full (each copy scales it), no lamps */
    static uint32_t scratch;
    if (!scratch)
        scratch = galloc(32);
    for (int a = 0; a < 6; a++)
        W32(scratch + 4 * (uint32_t)a, 0);
    guest_call(c, F_LIGHT_LIT, 4, scratch, scratch + 12, 0u, 255u);
    static D3DState st;
    static float recs[INST_COPIES_MAX][16];
    for (int a = 0; a < n;) {
        FarMesh *fm = copy_recs[a].m;
        int b = a;
        while (b < n && copy_recs[b].m == fm) {
            memcpy(recs[b - a], copy_recs[b].r, sizeof recs[0]);
            b++;
        }
        for (int k = 0; k < fm->nparts; k++) {
            const FarDraw *dr = &fm->p[k].d;
            if (!fm->p[k].got)
                continue;
            st = dr->st;
            st.xf[XF_VIEW] = base->xf[XF_VIEW];
            st.xf[XF_PROJ] = base->xf[XF_PROJ];
            memset(&st.xf[XF_WORLD], 0, sizeof st.xf[XF_WORLD]);
            for (int q = 0; q < 4; q++)
                st.xf[XF_WORLD].m[q][q] = 1;
            st.rs[RS_AMBIENT] = base->rs[RS_AMBIENT];
            for (int r = RS_FOGENABLE; r <= RS_FOGDENSITY; r++)
                st.rs[r] = base->rs[r];
            st.rs[RS_FOGVERTEXMODE] = base->rs[RS_FOGVERTEXMODE];
            st.rs[RS_RANGEFOGENABLE] = base->rs[RS_RANGEFOGENABLE];
            st.rs[RS_LIGHTING] = 1;
            st.vp = base->vp;
            st.rt = base->rt;
            st.zb = base->zb;
            memcpy(st.lights, base->lights, sizeof st.lights);
            for (int q = 0; q < LIGHTS_MAX; q++)        /* (the sun only: lamps light no copies) */
                if (st.lights[q].type != 3)
                    st.lights[q].enabled = 0;
            vk_fade = 0;
            d3d_tnl_draw_inst(&st, dr->prim, dr->raw, dr->nv, dr->ix, dr->ni, recs[0], b - a);
            st_copy_draws++;
        }
        st_copies += (unsigned long)(b - a);
        a = b;
    }
}

/* 0x418d40: the frame's object models drawn; then the far ones and the map's copies */
static int models_draw_hook(CPU *c)
{
    if (!far_now())
        return 0;
    F_00418d40_orig(c);
    far_learnt();
    far_replay(c);
    far_copies(c);
    return 1;
}

/* ------------------------------------------------------------------ the colour tiles' textures
   The game loads a colour tile's texture while its own terrain needs it and lets it go again
   (TILE_TEX + 4t -> +0x7c, 0 when not loaded). The far terrain draws every copy of every tile, so a
   tile let go was a hole in it, at any draw distance, coming and going as the game's window moved
   (Ant, 2026-10-19: flicker, and objects of the map's copies standing over nothing). Each tile's
   texture is now kept: when the game lets one go (drawdist_surface_gone, before its memory goes)
   its pixels are copied into a texture of ours (not the program's: no interfaces, in no list),
   which the far terrain draws until the game loads that tile again. A tile the game has not loaded
   at all is loaded by the game's own routine for it (tiles_load). Dropped when a race starts. */
static Surface *tile_seen[16];      /* the game's texture last drawn for each tile */
static Surface *tile_kept[16];      /* ours, a copy of the last one it let go */

static void tile_drop(int t)
{
    Surface *k = tile_kept[t];
    if (!k)
        return;
    tile_kept[t] = NULL;
    vk_surface_destroyed(k);
    gfree(k->mem);
    free(k);
}

static void tile_keep(int t, const Surface *s)
{
    if (!s->mem || !s->memsize || !s->width || !s->height)
        return;
    Surface *k = calloc(1, sizeof *k);
    if (!k)
        return;
    *k = *s;
    k->id = -1 - t;
    k->refs = 1;
    k->dead = 0;
    k->obj7 = k->obj1 = 0;
    k->link = NULL;
    k->privdata = NULL;
    k->gpu = NULL;
    k->vk_seen = 0;
    k->root = k->next = NULL;
    memset(k->attached, 0, sizeof k->attached);
    k->nattached = 0;
    k->mip_level = 0;
    k->mip_count = 1;
    k->palette = k->clipper = k->gamma_obj = 0;
    k->locks = 0;
    k->vidmem = 0;
    k->nonlocal = 0;
    k->mem = galloc(s->memsize);
    k->mem_kind = 1;
    memcpy(gptr(k->mem), gptr(s->mem), s->memsize);
    k->dirty = s->dirty + 1;
    tile_drop(t);
    tile_kept[t] = k;
}

static int tile_tried[16];          /* the game asked once this race to load it (tiles_load) */

static void tiles_reset(void)
{
    for (int t = 0; t < 16; t++) {
        tile_drop(t);
        tile_seen[t] = NULL;
        tile_tried[t] = 0;
    }
}

/* A tile the game has not loaded since the race started (its own terrain has not needed it): the
   game loads it, as its terrain does when it first sets a tile's texture (0x412440's texture
   states call 0x415b20(texture), which makes the surface and fills it), once a race */
#define F_TEX_RESIDENT  0x415b20u
static void tiles_load(CPU *c)
{
    for (int t = 0; t < 16; t++) {
        uint32_t h = R32(TILE_TEX + 4 * (uint32_t)t);
        if (!h || R32(h + 0x7c) || tile_kept[t] || tile_tried[t])
            continue;
        tile_tried[t] = 1;
        guest_call(c, F_TEX_RESIDENT, 1, h);
        if (ddlog)
            LOG(LOG_ALWAYS, "drawdist: colour tile %d's texture loaded for the far terrain: %s", t,
                R32(h + 0x7c) ? "done" : "failed");
    }
}

/* the texture the far terrain draws tile t with: the game's if loaded, else ours (NULL: none yet) */
static Surface *tile_texture(int t)
{
    uint32_t h = R32(TILE_TEX + 4 * (uint32_t)t), obj = h ? R32(h + 0x7c) : 0;
    Surface *tex = obj ? surface_get(obj) : NULL;
    if (tex && !tex->dead) {
        tile_seen[t] = tex;
        return tex;
    }
    return tile_kept[t];
}

/* ------------------------------------------------------------------ the far terrain */
#define TILE        64                          /* cells */
#define TV          (TILE + 1)                  /* vertices a row */
#define BLOCK       8
#define BLOCK_IDX   (BLOCK * BLOCK * 6)
#define TILE_IDX    (TILE / BLOCK * TILE / BLOCK * BLOCK_IDX)
/* coarser copies of the indices for the far tiles (every 2nd and every 4th point): a tile whose
   nearest point is lod_near[1] or lod_near[2] cells away is drawn with them (AUTO; a cell there
   is a few pixels wide, in the haze; where a fine tile meets a coarse one the cracks are under
   a pixel) */
static const int lod_step[3] = {1, 2, 4};
static const double lod_near[3] = {0, 256, 448};
#define LBLOCK_IDX(l)  ((BLOCK / lod_step[l]) * (BLOCK / lod_step[l]) * 6)
#define LTILE_IDX(l)   (TILE / BLOCK * TILE / BLOCK * LBLOCK_IDX(l))
static uint32_t lod_first[3];       /* each level's indices start here */
static int far_ready;       /* the mesh is the current map's */

static void far_build(void)
{
    lod_first[0] = 0;
    lod_first[1] = 16 * LTILE_IDX(0);
    lod_first[2] = lod_first[1] + 16 * LTILE_IDX(1);
    uint32_t nv = 16 * TV * TV, ni = lod_first[2] + 16 * LTILE_IDX(2);
    uint8_t *v = malloc((size_t)nv * 40);
    uint32_t *ix = malloc((size_t)ni * 4);
    if (!v || !ix)
        fatal("drawdist: out of memory");
    uint32_t colour = R32(TERRAIN_COLOUR) | 0xff000000u, spec = 0xff000000u;
    for (int t = 0; t < 16; t++) {
        int tx = t & 3, tz = t >> 2;
        for (int j = 0; j < TV; j++)
            for (int i = 0; i < TV; i++) {
                int x = tx * TILE + i, z = tz * TILE + j;
                float f[10] = {(float)x * 256.0f, RF32(HEIGHTS + 4 * (uint32_t)((z & 255) << 8 | (x & 255))),
                               (float)z * 256.0f, 0, 1, 0, 0, 0,
                               ((float)i * 3.984375f + 0.5f) / 256.0f, ((float)j * 3.984375f + 0.5f) / 256.0f};
                uint8_t *o = v + ((size_t)t * TV * TV + (size_t)j * TV + (size_t)i) * 40;
                memcpy(o, f, 40);
                memcpy(o + 24, &colour, 4);
                memcpy(o + 28, &spec, 4);
            }
        /* blocks of 8 x 8 cells, row by row, so that a row's blocks are one range; the cells
           split as the game's heights (0x401270): (0,0)-(1,1); each level of detail likewise */
        for (int l = 0; l < 3; l++) {
            int st = lod_step[l];
            uint32_t *o = ix + lod_first[l] + (size_t)t * LTILE_IDX(l);
            for (int bz = 0; bz < TILE / BLOCK; bz++)
                for (int bx = 0; bx < TILE / BLOCK; bx++)
                    for (int cz = 0; cz < BLOCK; cz += st)
                        for (int cx = 0; cx < BLOCK; cx += st) {
                            uint32_t a = (uint32_t)((bz * BLOCK + cz) * TV + bx * BLOCK + cx), b = a + (uint32_t)st,
                                     d = a + (uint32_t)st * TV, e = d + (uint32_t)st;
                            *o++ = a; *o++ = d; *o++ = e;
                            *o++ = a; *o++ = e; *o++ = b;
                        }
        }
    }
    far_ready = vk_far_mesh(v, nv, ix, ni);
    free(v);
    free(ix);
    if (ddlog)
        LOG(LOG_ALWAYS, "drawdist: far terrain mesh %s (%u vertices, %u triangles)", far_ready ? "made" : "not made",
            nv, ni / 3);
}

static void mat_mul(double r[16], const double a[16], const Mat4 *b)
{
    double t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b->m[0][j] + a[i * 4 + 1] * b->m[1][j] + a[i * 4 + 2] * b->m[2][j] +
                           a[i * 4 + 3] * b->m[3][j];
    memcpy(r, t, sizeof t);
}

/* is any of the box's corners (mesh coordinates) in front of the camera and inside the widened
   view? (generous: the widescreen view is wider than the projection says) */
static int box_visible(const double m[16], double x0, double x1, double z0, double z1)
{
    int out[5] = {0, 0, 0, 0, 0};
    for (int k = 0; k < 8; k++) {
        double x = k & 1 ? x1 : x0, y = k & 2 ? 4096.0 : 0.0, z = k & 4 ? z1 : z0;
        double cx = x * m[0] + y * m[4] + z * m[8] + m[12], cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        double cz = x * m[2] + y * m[6] + z * m[10] + m[14], cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        out[0] += cw <= 0.0;
        out[1] += cx > 3 * cw;
        out[2] += cx < -3 * cw;
        out[3] += cy > 1.5 * cw || cy < -1.5 * cw;
        out[4] += cz > cw;
    }
    return out[0] < 8 && out[1] < 8 && out[2] < 8 && out[4] < 8;
}

/* The blocks of tile t (its corner at x0, z0 cells) inside rectangle A and outside rectangle B
   (cells, from-to; aligned to blocks), whole rows joined into one range */
typedef struct { int x0, x1, z0, z1; } Rect;
static int far_blocks(const D3DState *st, const TnlConst *k, int t, int x0, int z0, Rect a, Rect b, int l)
{
    int n = 0;
    uint32_t run = 0, run_n = 0;
    for (int bz = 0; bz < 8; bz++) {
        int lo[2] = {0, 0}, hi[2] = {0, 0}, zc = z0 + bz * BLOCK;
        if (zc >= a.z0 && zc < a.z1) {
            int a0 = (a.x0 - x0) / BLOCK, a1 = (a.x1 - x0) / BLOCK;
            a0 = a0 < 0 ? 0 : a0 > 8 ? 8 : a0;
            a1 = a1 < 0 ? 0 : a1 > 8 ? 8 : a1;
            lo[0] = a0;
            hi[0] = a1;
            if (zc >= b.z0 && zc < b.z1) {
                int b0 = (b.x0 - x0) / BLOCK, b1 = (b.x1 - x0) / BLOCK;
                b0 = b0 < a0 ? a0 : b0 > a1 ? a1 : b0;
                b1 = b1 < a0 ? a0 : b1 > a1 ? a1 : b1;
                if (b0 < b1) {
                    hi[0] = b0;
                    lo[1] = b1;
                    hi[1] = a1;
                }
            }
        }
        /* the blocks drawn, in index order, joined into ranges where they follow on (AUTO: not
           those the ground before them hides) */
        for (int bx = 0; bx < 8; bx++) {
            double rx = x0 + bx * BLOCK + BLOCK / 2 - cam_cells[0], rz = zc + BLOCK / 2 - cam_cells[1];
            int in = ((bx >= lo[0] && bx < hi[0]) || (bx >= lo[1] && bx < hi[1])) &&
                     !(blocks_test && hidden(rx, rz, BLOCK * 0.71, block_height(x0 + bx * BLOCK, zc)));
            if (in) {
                if (!run_n)
                    run = lod_first[l] + (uint32_t)t * LTILE_IDX(l) + (uint32_t)(bz * 8 + bx) * LBLOCK_IDX(l);
                run_n += LBLOCK_IDX(l);
            } else if (run_n) {
                vk_far_draw(st, k, run, run_n, t * TV * TV);
                n++;
                run_n = 0;
            }
        }
    }
    if (run_n) {
        vk_far_draw(st, k, run, run_n, t * TV * TV);
        n++;
    }
    return n;
}

/* The far terrain meets the game's at the edge of its window, where the game's patches are big
   and their edges straight: on a rise the game's edge is lower than the far terrain's, and the sky
   would show through the crack. A skirt hangs from the far terrain's edge there: a wall, cell by
   cell, from its height down SKIRT units, in the colour tile of its cell. */
#define SKIRT 192.0         /* units */
#define SKIRT_MAX 1024      /* cells round the window */

static float height_at(int x, int z)
{
    return RF32(HEIGHTS + 4 * (uint32_t)((z & 255) << 8 | (x & 255)));
}

static void skirt_vertex(uint8_t *o, int x, int z, float y, int lx, int lz, uint32_t colour)
{
    float f[10] = {(float)x * 256.0f, y, (float)z * 256.0f, 0, 1, 0, 0, 0,
                   ((float)lx * 3.984375f + 0.5f) / 256.0f, ((float)lz * 3.984375f + 0.5f) / 256.0f};
    uint32_t spec = 0xff000000u;
    memcpy(o, f, 40);
    memcpy(o + 24, &colour, 4);
    memcpy(o + 28, &spec, 4);
}

/* the skirt round the window (cells x0..x1, z0..z1), one draw per colour tile */
static int far_skirt(D3DState *st, const TnlConst *k, int x0, int x1, int z0, int z1)
{
    static uint8_t v[16][SKIRT_MAX * 4 * 40];
    static uint16_t ix[16][SKIRT_MAX * 6];
    int nv[16] = {0}, n = 0;
    uint32_t colour = R32(TERRAIN_COLOUR) | 0xff000000u;
    /* each edge: its first cell corner, the step along it, the far terrain's side (cells) */
    int edges[4][6] = {{x0, z0, 1, 0, 0, -1}, {x0, z1, 1, 0, 0, 0}, {x0, z0, 0, 1, -1, 0}, {x1, z0, 0, 1, 0, 0}};
    for (int e = 0; e < 4; e++) {
        int len = edges[e][2] ? x1 - x0 : z1 - z0;
        for (int i = 0; i < len; i++) {
            int ax = edges[e][0] + i * edges[e][2], az = edges[e][1] + i * edges[e][3];
            int bx = ax + edges[e][2], bz = az + edges[e][3];
            int fx = ax + edges[e][4], fz = az + edges[e][5];         /* the far terrain's cell */
            int t = ((fz & 255) >> 6) * 4 + ((fx & 255) >> 6);
            int ox = fx - (fx & 63), oz = fz - (fz & 63);               /* its tile's corner */
            if (nv[t] + 4 > SKIRT_MAX * 4)
                continue;
            uint8_t *o = v[t] + (size_t)nv[t] * 40;
            float ha = height_at(ax, az), hb = height_at(bx, bz);
            skirt_vertex(o, ax, az, ha, ax - ox, az - oz, colour);
            skirt_vertex(o + 40, ax, az, ha - (float)SKIRT, ax - ox, az - oz, colour);
            skirt_vertex(o + 80, bx, bz, hb, bx - ox, bz - oz, colour);
            skirt_vertex(o + 120, bx, bz, hb - (float)SKIRT, bx - ox, bz - oz, colour);
            uint16_t *q = ix[t] + nv[t] / 4 * 6, b = (uint16_t)nv[t];
            q[0] = b; q[1] = (uint16_t)(b + 1); q[2] = (uint16_t)(b + 2);
            q[3] = (uint16_t)(b + 1); q[4] = (uint16_t)(b + 3); q[5] = (uint16_t)(b + 2);
            nv[t] += 4;
        }
    }
    for (int t = 0; t < 16; t++) {
        uint32_t h = R32(TILE_TEX + 4 * (uint32_t)t), obj = h ? R32(h + 0x7c) : 0;
        Surface *tex = obj ? surface_get(obj) : NULL;
        if (!nv[t] || !tex || !vk_tnl_ok(st, nv[t]))
            continue;
        st->tex[0] = tex;
        vk_draw_tnl(st, PT_TRIANGLELIST, v[t], nv[t], ix[t], nv[t] / 4 * 6, k);
        n++;
    }
    return n;
}

static void far_draw(void)
{
    const D3DState *cur = d3d_last_state();
    if (!cur)
        return;
    if (!far_ready)
        far_build();
    if (!far_ready)
        return;
    static D3DState st;
    st = *cur;
    uint32_t *rs = st.rs;
    /* as the game draws its terrain's colour tiles: texture times colour, blended by the
       texture's alpha (over the sky), alpha 0 left out */
    rs[RS_ALPHABLENDENABLE] = 1;
    rs[RS_SRCBLEND] = 5;
    rs[RS_DESTBLEND] = 6;
    rs[RS_ALPHATESTENABLE] = 1;
    rs[RS_ALPHAFUNC] = 6;
    rs[RS_ALPHAREF] = 0;
    rs[RS_ZENABLE] = 1;
    rs[RS_ZWRITEENABLE] = 1;
    rs[RS_ZFUNC] = 4;
    rs[RS_CULLMODE] = 1;
    rs[RS_SPECULARENABLE] = 0;
    rs[RS_COLORKEYENABLE] = 0;
    rs[RS_ZBIAS] = 0;
    st.tss[0][TSS_COLOROP] = 4;
    st.tss[0][TSS_COLORARG1] = 2;
    st.tss[0][TSS_COLORARG2] = 0;
    st.tss[0][TSS_ALPHAOP] = 4;
    st.tss[0][TSS_ALPHAARG1] = 2;
    st.tss[0][TSS_ALPHAARG2] = 0;
    st.tss[0][TSS_TEXCOORDINDEX] = 0;
    st.tss[1][TSS_COLOROP] = 1;
    st.tss[1][TSS_ALPHAOP] = 1;
    st.tex[1] = NULL;
    /* the camera (units) and the game's terrain (cells, as drawn this frame) */
    double cx = (int32_t)R32(CAM_X) / 65536.0, cy = (int32_t)R32(CAM_Y) / 65536.0, cz = (int32_t)R32(CAM_Z) / 65536.0;
    int hx0 = (int32_t)R32(WIN_X) + (int32_t)R32(WIN_FROM), hx1 = (int32_t)R32(WIN_X) + (int32_t)R32(WIN_TO);
    int hz0 = (int32_t)R32(WIN_Z) + (int32_t)R32(WIN_FROM), hz1 = (int32_t)R32(WIN_Z) + (int32_t)R32(WIN_TO);
    double reach = RACE_FAR * factor() / 256.0 + 1;      /* cells */
    cam_cells[0] = cx / 256;
    cam_cells[1] = cz / 256;
    lod_on = level == AUTO && lod_use;
    TnlConst k;
    memset(&k, 0, sizeof k);
    Rect all = {-100000, 100000, -100000, 100000}, hole = {hx0, hx1, hz0, hz1};
    int ndraws = 0;
    int ox0 = (int)floor((cx / 256 - reach) / 256), ox1 = (int)floor((cx / 256 + reach) / 256);
    int oz0 = (int)floor((cz / 256 - reach) / 256), oz1 = (int)floor((cz / 256 + reach) / 256);
    for (int oz = oz0; oz <= oz1; oz++)
        for (int ox = ox0; ox <= ox1; ox++) {
            /* this copy of the map: mesh coordinates -> round the camera */
            double w[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, ox * 65536.0 - cx, -cy, oz * 65536.0 - cz, 1};
            double wv[16], wvp[16];
            mat_mul(wv, w, &st.xf[XF_VIEW]);
            mat_mul(wvp, wv, &st.xf[XF_PROJ]);
            for (int i = 0; i < 16; i++) {
                k.wvp[i] = (float)wvp[i];
                k.wv[i] = (float)wv[i];
            }
            for (int i = 0; i < 3; i++) {       /* the world matrix (ray-traced shadows) */
                for (int j = 0; j < 3; j++)
                    k.world[i][j] = (float)w[j * 4 + i];
                k.world[i][3] = (float)w[12 + i];
            }
            for (int t = 0; t < 16; t++) {
                int x0 = ox * 256 + (t & 3) * TILE, z0 = oz * 256 + (t >> 2) * TILE;
                double dx = fmax(fmax(x0 - cx / 256, cx / 256 - (x0 + TILE)), 0);
                double dz = fmax(fmax(z0 - cz / 256, cz / 256 - (z0 + TILE)), 0);
                if (dx * dx + dz * dz > reach * reach ||
                    !box_visible(wvp, (t & 3) * TILE * 256.0, ((t & 3) + 1) * TILE * 256.0, (t >> 2) * TILE * 256.0,
                                 ((t >> 2) + 1) * TILE * 256.0))
                    continue;
                Surface *tex = tile_texture(t);
                if (!tex)
                    continue;
                st.tex[0] = tex;
                /* AUTO: a far tile, from its nearest point, with a coarser level of detail; the tile
                   as a whole hidden by the ground before it (left out), or wholly shown (its blocks
                   not tested one by one) */
                double near = sqrt(dx * dx + dz * dz);
                int l = !lod_on ? 0 : near >= lod_near[2] ? 2 : near >= lod_near[1] ? 1 : 0;
                double tx = x0 + TILE / 2 - cam_cells[0], tz = z0 + TILE / 2 - cam_cells[1];
                if (horizon_on && hidden(tx, tz, TILE * 0.71, tile_range[t][1]))
                    continue;
                blocks_test = !all_shown(tx, tz, TILE * 0.71, tile_range[t][0]);
                ndraws += far_blocks(&st, &k, t, x0, z0, all, hole, l);
            }
            if (ox == 0 && oz == 0)
                ndraws += far_skirt(&st, &k, hx0, hx1, hz0, hz1);
        }
    static int most;
    if (ddlog && ndraws > most) {
        most = ndraws;
        LOG(LOG_ALWAYS, "drawdist: far terrain in %d draws (frame %lu)", ndraws, frame_count);
    }
}

/* 0x402a90(range): the terrain of a view */
static int terrain_draw_hook(CPU *c)
{
    if (!main_view || (!level && !fading))
        return 0;
    F_00402a90_orig(c);
    tiles_load(c);
    far_draw();
    return 1;
}

/* ------------------------------------------------------------------ AUTO
   DRAW DISTANCE AUTO (Ant, 2026-10-19: "queue the auto setting"): the reach follows what the
   camera can see, and what the ground hides is left out. Rays over the map's heights round the
   camera (AUTO_DIRS of them) keep the horizon along them: the steepest ground so far at each step
   out. Every 8th view (and at a cut) all round: the factor aims at the farthest place where ground
   with an object up to AUTO_TALL on it would show over that horizon (2X to 8X; turning the camera
   changes nothing); it eases there (out in about half a second, back in over two) and the map's
   fog thins and thickens with it, so what comes into reach comes out of the fog. Every view the
   rays within 100 degrees of the view's direction: far terrain tiles and blocks wholly under the
   horizon of the rays they span (by their highest ground) are not drawn, nor the map's copies (by
   the tops of their objects). The maps' ground is at most 4080 units (61 m) high: from the ground
   about half of all directions are closed within a few hundred metres, but nearly always some far
   peak shows over the rest, so the factor is 8X almost everywhere and the saving is in what the
   ground hides. What hides things is taken as low as it may be drawn: only the far terrain (the
   game's own window, with its coarser patches, and colour tiles whose texture the game has not
   loaded hide nothing), the lowest of the patches its level of detail and the rays' spacing span
   (a notch between two rays may show what is behind it); and a thing within HIDE_ANGLE of the
   horizon is drawn. Checked by frames with and without (INSANE_DD_HORIZON=0): the same to the
   pixel but for the anti-aliasing's noise. INSANE_DD_AUTO=<f> holds the factor at f (tests). */
#define AUTO_MIN    2.0
#define AUTO_MAX    8.0
#define AUTO_TALL   2000.0          /* units (30 m): what the factor allows for objects on the ground */
#define AUTO_DIRS   128
#define AUTO_STEPS  128
#define HIDE_MARGIN 400.0           /* units over its top a thing is taken to reach */
#define HIDE_ANGLE  3.0             /* units a cell (a few pixels): under the horizon by less, it is drawn
                                       (a ridge's edge pixels show some of what is behind) */
#define FOG_MAIN    0x1bf3e10u      /* the map's fog (0x412b00's {colour, density, mode}) */
#define FOG_SCALE   0x4bf3c0u       /* ... whose density is the one asked for times this */
static double auto_hold;    /* INSANE_DD_AUTO */
static float fog_asked;     /* the map's fog density as the game asked for it */
static int heights_known;   /* heights_host, heights_top and block_top are the map's */
static float heights_host[65536];   /* the map's heights, units */
static double heights_top;  /* ... the highest */
static float block_top[32 * 32];    /* ... the highest of each 8 x 8-cell block (its 9 x 9 points) */
/* ... the lowest of each 2^l-cell patch (its points, edges included): what hides things is taken at
   its lowest over the patch the ground there is drawn with (the game's own terrain near the camera
   by its coarser patches, the far terrain's coarser levels), so nothing is left out that shows */
static float patch_low[6][128 * 128];
static double auto_last;    /* the last view's time, ms (0: none yet) */
static double auto_reach_last, auto_target_last;
static double step_d[AUTO_STEPS];   /* the rays' steps, cells */
#define STEP_OF_MAX 1024
static int16_t step_of[STEP_OF_MAX]; /* ... the last at or before each whole cell (-1: none) */
static int nsteps;
static float ray_hz[AUTO_DIRS][AUTO_STEPS];     /* the horizon (units a cell) up to each step */
static int ray_n[AUTO_DIRS];        /* ... steps worked out (past them nothing more can show) */
static double ray_cy;               /* the camera's height, units */

/* the ground's height at (x, z) cells, on its triangles as the far terrain draws them (a cell split
   (0,0)-(1,1)) */
static double height_ground(double x, double z)
{
    double fx = floor(x), fz = floor(z), ax = x - fx, az = z - fz;
    int ix = (int)fx & 255, iz = (int)fz & 255, jx = (ix + 1) & 255, jz = (iz + 1) & 255;
    const float *r0 = heights_host + (iz << 8), *r1 = heights_host + (jz << 8);
    if (ax >= az)
        return r0[ix] + ax * (r0[jx] - r0[ix]) + az * (r1[jx] - r0[jx]);
    return r0[ix] + az * (r1[ix] - r0[ix]) + ax * (r1[jx] - r1[ix]);
}

/* what draws the ground this view: the game's window (cells, a block wider each side: it moves by
   blocks), whose terrain is taken to hide nothing (its patches are coarser and do not fill it all),
   and which colour tiles the far terrain has (a tile whose texture the game has not loaded is not
   drawn, and hides nothing) */
static int occ_win[4];
static int occ_tile[16];

/* the ground at (x, z) cells, as low as it may be drawn (see patch_low) and as low as it is anywhere
   between this ray and the next ones (a notch between two rays may show what is behind it): level
   l (step_level: at least the far terrain's level of detail there and the rays' spacing), the
   lowest of the 3 x 3 patches round it (patch_round); level 0 the ground itself */
static float patch_round[6][128 * 128];
static uint8_t step_level[AUTO_STEPS];

static double height_occluder(double x, double z, int l)
{
    int ix = (int)floor(x), iz = (int)floor(z);
    if ((ix >= occ_win[0] && ix < occ_win[1] && iz >= occ_win[2] && iz < occ_win[3]) ||
        !occ_tile[((iz & 255) >> 6) * 4 + ((ix & 255) >> 6)])
        return -1e30;   /* the game's own terrain (coarser patches, and not all of its window) or not drawn */
    if (!l)
        return height_ground(x, z);
    int n = 256 >> l;
    return patch_round[l][(((iz & 255) >> l) * n) + ((ix & 255) >> l)];
}

/* Directions round the camera by a pseudo-angle (0..4 round the circle, no trigonometry) */
static double pseudo_angle(double x, double z)
{
    double s = fabs(x) + fabs(z), p = s > 0 ? z / s : 0;
    return x < 0 ? 2 - p : z < 0 ? 4 + p : p;
}

/* the highest ground of the 8 x 8-cell block at (x, z) cells (aligned, wrapping) */
static double block_height(int x, int z)
{
    return block_top[((z & 255) >> 3) * 32 + ((x & 255) >> 3)];
}

/* something at (x, z) cells round the camera, radius r cells, its top at y units, cannot show over
   the ground before it: under the lowest horizon of the rays round it, the ground well before its
   nearest point (not the ground beside it, where the rays pass it) */
/* Over the rays an item at (x, z) cells, radius r, spans (and one more each side), at the last
   step before dist: whether every ray's horizon is over thr (want 1) or every one at or under it
   (want 0); 0 if not, or a ray there was not worked out */
static int horizon_all(double x, double z, double r, double dist, double thr, int want)
{
    double before = dist - fmax(8, 0.06 * dist), d = sqrt(x * x + z * z);
    if (before < 2 || d <= r)
        return 0;
    int k = before < STEP_OF_MAX ? step_of[(int)before] : nsteps - 1;     /* the last step before it */
    if (k < 0)
        return 0;
    int a = (int)(pseudo_angle(x, z) * (AUTO_DIRS / 4.0));
    /* (its half-angle is at most r/d * pi/2; a pseudo-angle turns at most 1 a radian) */
    int w = (int)(r / d * (M_PI / 2) * (AUTO_DIRS / 4.0)) + 2;
    if (w > AUTO_DIRS / 2)
        return 0;
    for (int j = -w; j <= w; j++) {
        int b = ((a + j) % AUTO_DIRS + AUTO_DIRS) % AUTO_DIRS;
        if (ray_n[b] <= 0 || (ray_hz[b][k < ray_n[b] ? k : ray_n[b] - 1] > thr) != want)
            return 0;
    }
    return 1;
}

/* something at (x, z) cells round the camera, radius r cells, its top at y units, cannot show over
   the ground before it: under the horizon of every ray it spans, the ground well before its
   nearest point (not the ground beside it, where the rays pass it) */
static int hidden(double x, double z, double r, double y)
{
    if (!horizon_on)
        return 0;
    double dn = sqrt(x * x + z * z) - r;
    return dn > 0 && horizon_all(x, z, r, dn, (y + HIDE_MARGIN - ray_cy) / dn + HIDE_ANGLE, 1);
}

/* everything in (x, z, r), nothing lower than y, shows over the ground before it, even at its far
   side (so each thing in it need not be tested) */
static int all_shown(double x, double z, double r, double y)
{
    if (!horizon_on)
        return 1;
    /* the lowest any of it can be seen at: its lowest point at the far side if over the camera,
       else at the near side */
    double d = sqrt(x * x + z * z), df = d + r;
    return horizon_all(x, z, r, df, (y - ray_cy) / (y >= ray_cy ? df : fmax(d - r, 1)), 0);
}

/* the rays: their horizons, for the rays round the view's direction (all of them with all_round),
   and, with all_round, how far from the camera something could be seen (cells; else -1) */
static double ray_dir[AUTO_DIRS][2];

static double auto_reach(int all_round)
{
    double cx = (int32_t)R32(CAM_X) / CELL, cz = (int32_t)R32(CAM_Z) / CELL, cy = (int32_t)R32(CAM_Y) / 65536.0;
    double dmax = AUTO_MAX * RACE_FAR / 256.0, best = all_round ? 0 : -1;
    if (!nsteps) {
        for (double d = 1; d <= dmax + 8 && nsteps < AUTO_STEPS; d += fmax(1.0, d / 25)) {
            int l = d < lod_near[1] ? 0 : d < lod_near[2] ? 1 : 2;
            while (l < 5 && (1 << l) < d * (2 * M_PI / AUTO_DIRS))
                l++;
            step_level[nsteps] = (uint8_t)l;
            step_d[nsteps++] = d;
        }
        for (int c = 0, k = -1; c < STEP_OF_MAX; c++) {
            while (k + 1 < nsteps && step_d[k + 1] <= c)
                k++;
            step_of[c] = (int16_t)k;
        }
        for (int a = 0; a < AUTO_DIRS; a++) {
            double pa = 4.0 * (a + 0.5) / AUTO_DIRS, r = pa - floor(pa), dx, dz;
            switch ((int)pa) {
            case 0: dx = 1 - r; dz = r; break;
            case 1: dx = -r; dz = 1 - r; break;
            case 2: dx = r - 1; dz = -r; break;
            default: dx = r; dz = r - 1; break;
            }
            double len = sqrt(dx * dx + dz * dz);
            ray_dir[a][0] = dx / len;
            ray_dir[a][1] = dz / len;
        }
    }
    ray_cy = cy;
    occ_win[0] = (int32_t)R32(WIN_X) + (int32_t)R32(WIN_FROM) - BLOCK;
    occ_win[1] = (int32_t)R32(WIN_X) + (int32_t)R32(WIN_TO) + BLOCK;
    occ_win[2] = (int32_t)R32(WIN_Z) + (int32_t)R32(WIN_FROM) - BLOCK;
    occ_win[3] = (int32_t)R32(WIN_Z) + (int32_t)R32(WIN_TO) + BLOCK;
    for (int t = 0; t < 16; t++)
        occ_tile[t] = tile_texture(t) != NULL;
    /* the view's direction (the objects' view test's depth row), and the rays within 100 degrees
       of it: what is drawn lies there (the rest hide nothing this view) */
    int a0 = 0, na = AUTO_DIRS;
    if (!all_round) {
        double fx = RF32(CAM_MATRIX + 12), fz = RF32(CAM_MATRIX + 44);
        int af = (int)(pseudo_angle(fx, fz) * (AUTO_DIRS / 4.0));
        na = AUTO_DIRS * 200 / 360 + 4;
        a0 = af - na / 2;
    }
    for (int a = 0; a < AUTO_DIRS; a++)
        ray_n[a] = 0;
    for (int q = 0; q < na; q++) {
        int a = ((a0 + q) % AUTO_DIRS + AUTO_DIRS) % AUTO_DIRS;
        double dx = ray_dir[a][0], dz = ray_dir[a][1];
        double horizon = -1e30, seen = 0;           /* the steepest ground so far, units a cell */
        int k = 0;
        if (!all_round)                             /* (the game's window hides nothing: 48 cells round
                                                       the camera are inside it whichever way) */
            for (; k < nsteps && step_d[k] < 48; k++)
                ray_hz[a][k] = -1e30f;
        for (; k < nsteps; k++) {
            double d = step_d[k], x = cx + dx * d, z = cz + dz * d;
            if (all_round && d <= dmax) {
                double h = heights_host[((int)floor(z) & 255) << 8 | ((int)floor(x) & 255)];
                if ((h + AUTO_TALL - cy) / d > horizon)
                    seen = d;
            }
            double ho = height_occluder(x, z, step_level[k]);
            if ((ho - cy) / d > horizon)
                horizon = (ho - cy) / d;
            ray_hz[a][k] = (float)horizon;
            if ((heights_top + AUTO_TALL + HIDE_MARGIN - cy) / d <= horizon) {
                k++;
                break;                              /* nothing farther can show over it */
            }
        }
        ray_n[a] = k;
        if (all_round && seen > best)
            best = seen;
    }
    return best;
}

static void auto_update(void)
{
    if (!heights_known) {
        heights_top = -1e30;
        for (uint32_t i = 0; i < 65536; i++) {
            heights_host[i] = RF32(HEIGHTS + 4 * i);
            if (heights_host[i] > heights_top)
                heights_top = heights_host[i];
        }
        for (int l = 1; l <= 5; l++) {
            int size = 1 << l, n = 256 >> l;
            for (int pz = 0; pz < n; pz++)
                for (int px = 0; px < n; px++) {
                    float m = 1e30f;
                    for (int j = 0; j <= size; j++)
                        for (int i = 0; i <= size; i++)
                            m = fminf(m, heights_host[((pz * size + j) & 255) << 8 | ((px * size + i) & 255)]);
                    patch_low[l][pz * n + px] = m;
                }
            for (int pz = 0; pz < n; pz++)
                for (int px = 0; px < n; px++) {
                    float m = 1e30f;
                    for (int j = -1; j <= 1; j++)
                        for (int i = -1; i <= 1; i++)
                            m = fminf(m, patch_low[l][((pz + j) & (n - 1)) * n + ((px + i) & (n - 1))]);
                    patch_round[l][pz * n + px] = m;
                }
        }
        for (int t = 0; t < 16; t++) {
            float lo = 1e30f, hi = -1e30f;
            for (int j = 0; j <= TILE; j++)
                for (int i = 0; i <= TILE; i++) {
                    float h = heights_host[(((t >> 2) * TILE + j) & 255) << 8 | (((t & 3) * TILE + i) & 255)];
                    lo = fminf(lo, h);
                    hi = fmaxf(hi, h);
                }
            tile_range[t][0] = lo;
            tile_range[t][1] = hi;
        }
        for (int bz = 0; bz < 32; bz++)
            for (int bx = 0; bx < 32; bx++) {
                float m = -1e30f;
                for (int j = 0; j <= 8; j++)
                    for (int i = 0; i <= 8; i++)
                        m = fmaxf(m, heights_host[((bz * 8 + j) & 255) << 8 | ((bx * 8 + i) & 255)]);
                block_top[bz * 32 + bx] = m;
            }
        heights_known = 1;
    }
    double now = deterministic ? auto_last + 1000.0 / 60.0 : now_ms();
    double dt = auto_last > 0 ? fmin(now - auto_last, 250.0) : -1;
    auto_last = now;
    /* the reach all round every 8th view (the factor eases anyway), the rays in view every view */
    static unsigned long views;
    double seen = auto_reach(dt < 0 || cam_cut || views++ % 8 == 0);
    horizon_on = horizon_use;
    if (seen >= 0) {
        auto_reach_last = seen * 1.1 + 8;           /* (a margin) */
        auto_target_last = fmin(fmax(auto_reach_last * 256.0 / RACE_FAR, AUTO_MIN), AUTO_MAX);
    }
    double t = auto_target_last;
    if (auto_hold > 0)
        auto_f = (float)auto_hold;
    else if (dt < 0 || cam_cut)
        auto_f = (float)t;
    else
        auto_f += (float)((t - auto_f) * (1 - exp(-dt / (t > auto_f ? 500.0 : 2000.0))));
}

/* ------------------------------------------------------------------ views, fog, far plane */
/* 0x465950(range, ...): draws the world round the camera */
static int world_draw_hook(CPU *c)
{
    uint32_t ret = R32(c->esp);
    main_view = ret == VIEW_RET1 || ret == VIEW_RET2;
    fading = main_view && fade_on;
    if (!main_view || (!level && !fading)) {
        main_view = fading = 0;
        return 0;
    }
    fade_gen++;
    static uint32_t last_cam[2];
    cam_cut = abs((int32_t)(R32(CAM_X) - last_cam[0])) > (2 << 24) || abs((int32_t)(R32(CAM_Z) - last_cam[1])) > (2 << 24);
    last_cam[0] = R32(CAM_X);
    last_cam[1] = R32(CAM_Z);
    if (level == AUTO) {
        auto_update();
        uint32_t fog = R32(FOG_MAIN);               /* the map's fog thins with the reach */
        if (fog && fog_asked > 0)
            WF32(fog + 4, (float)((long double)(fog_asked / auto_f) * RF32(FOG_SCALE)));
        if (ddlog && frame_count % 100 == 0)
            LOG(LOG_ALWAYS, "drawdist: auto %.2fX (reach %.0f cells, aiming at %.2fX; camera %.0f units over the ground)",
                auto_f, auto_reach_last, auto_target_last,
                (int32_t)R32(CAM_Y) / 65536.0 - height_ground((int32_t)R32(CAM_X) / CELL, (int32_t)R32(CAM_Z) / CELL));
    }
    obj_range = vis_range();
    box_half = obj_range * 8.0;
    if (ddlog) {                /* the game's own visibility level (AUTO changes it with the frame rate) */
        static int last_vis = -1;
        int v = (int)R32(VIS_LEVEL);
        if (v != last_vis)
            LOG(LOG_ALWAYS, "drawdist: the game's visibility level %d (range %d) at frame %lu", v, obj_range, frame_count);
        last_vis = v;
    }
    if (fading) {               /* full fog at the far plane */
        vk_fog_end = RACE_FAR * factor() * 0.98f;
        vk_fog_start = RACE_FAR * factor() * 0.85f;
    }
    if (level)                  /* many more small objects: their transform on the GPU too */
        vk_tnl_min = 3;
    far_n = 0;
    F_00465950_orig(c);
    vk_tnl_min = 16;
    horizon_on = 0;
    vk_fog_end = vk_fog_start = 0;
    vk_fade = 0;
    main_view = fading = 0;
    if (ddlog && fade_on && frame_count % 500 == 0) {
        LOG(LOG_ALWAYS, "drawdist: fades this view: %lu objects, %lu of them with two models, %lu instances drawn faded",
            st_faded, st_twice, st_hits);
    }
    st_faded = st_twice = st_hits = 0;
    if (far_log && frame_count % 500 == 0)
        LOG(LOG_ALWAYS, "drawdist: far objects this view: %lu drawn from kept meshes (%lu draws); left to the game: %lu "
            "not kept yet, %lu not for kept meshes (lamps, animated, part by part, sprites)", st_far, st_far_kept,
            st_far_game, st_far_left);
    if (far_log && frame_count % 500 == 0)
        LOG(LOG_ALWAYS, "drawdist: the map's copies this view: %lu drawn in %lu instanced draws (%d objects listed)",
            st_copies, st_copy_draws, ncopy);
    if (ddlog && frame_count % 500 == 0) {
        int loaded = 0, kept = 0;
        for (int t = 0; t < 16; t++) {
            uint32_t h = R32(TILE_TEX + 4 * (uint32_t)t);
            if (h && R32(h + 0x7c))
                loaded++;
            else if (tile_kept[t])
                kept++;
        }
        LOG(LOG_ALWAYS, "drawdist: colour tiles: %d the game's, %d kept, %d none", loaded, kept, 16 - loaded - kept);
    }
    st_far = st_far_kept = st_far_game = st_far_left = 0;
    st_copies = st_copy_draws = 0;
    if (ddlog) {
        static uint32_t most[2];
        uint32_t n[2] = {0, 0};
        for (int i = 0; i < 16; i++) if (R32(TILE_COUNTS + 4 * i) > n[0]) n[0] = R32(TILE_COUNTS + 4 * i);
        for (int i = 0; i < 5; i++) if (R32(DETAIL_COUNTS + 4 * i) > n[1]) n[1] = R32(DETAIL_COUNTS + 4 * i);
        if (n[0] > most[0] || n[1] > most[1]) {
            most[0] = n[0] > most[0] ? n[0] : most[0];
            most[1] = n[1] > most[1] ? n[1] : most[1];
            LOG(LOG_ALWAYS, "drawdist: patches: tile %u, detail %u (frame %lu)", most[0], most[1], frame_count);
        }
    }
    return 1;
}

/* 0x412b00(colour, density, mode): makes a fog; the map's own one thins with the longer view */
static int fog_new_hook(CPU *c)
{
    uint32_t ret = R32(c->esp);
    if (level && (ret == FOG_RET_LOAD || ret == FOG_RET_LEVEL)) {
        float d = RF32(c->esp + 8), k = factor();
        fog_asked = d;                              /* (AUTO thins it each view) */
        WF32(c->esp + 8, d / k);
        if (ddlog)
            LOG(LOG_ALWAYS, "drawdist: fog density %g -> %g", d, d / k);
    }
    return 0;
}

/* widescreen.c's camera hook (0x42a350): the race camera's far plane moves out too */
void drawdist_camera(uint32_t args)
{
    float far = RF32(args + 28);
    if (level && far >= RACE_FAR)
        WF32(args + 28, far * factor());
}

/* 0x401580: makes the terrain's vertex buffers, as a race or the menus start */
static int terrain_buffers_hook(CPU *c)
{
    (void)c;
    level = want;
    tiles_reset();
    heights_known = 0;
    auto_last = 0;
    far_ready = 0;
    far_reset();
    copy_reset();
    if (ddlog)
        LOG(LOG_ALWAYS, "drawdist: %s view, distance fade %s (frame %lu)", level_names[level], fade_on ? "on" : "off",
            frame_count);
    return 0;
}

/* ------------------------------------------------------------------ options */
static void parse(void)
{
    const char *v = option_str("draw_distance", "original");
    want = 0;
    for (int i = 0; i < NLEVELS; i++)
        if (!strcasecmp(v, level_names[i]))
            want = i;
    if (want == 0 && strcasecmp(v, "original"))
        LOG(LOG_ALWAYS, "options: draw_distance \"%s\" is not original, 1.5x, 2x, 3x, 4x or auto", v);
    fade_on = !strcasecmp(option_str("distance_fade", "off"), "on");
}

void drawdist_init(void)
{
    ddlog = getenv("INSANE_DD_LOG") ? (atoi(getenv("INSANE_DD_LOG")) > 1 ? 2 : 1) : 0;
    allowed = !deterministic || getenv("INSANE_ENHANCED");
    if (!allowed)
        return;
    auto_hold = getenv("INSANE_DD_AUTO") ? atof(getenv("INSANE_DD_AUTO")) : 0;
    horizon_use = !(getenv("INSANE_DD_HORIZON") && !strcmp(getenv("INSANE_DD_HORIZON"), "0"));
    lod_use = !(getenv("INSANE_DD_LOD") && !strcmp(getenv("INSANE_DD_LOD"), "0"));
    parse();
    hook_terrain_buffers = terrain_buffers_hook;
    hook_world_draw = world_draw_hook;
    hook_fog_new = fog_new_hook;
    hook_object_draw = object_draw_hook;
    hook_object_box = object_box_hook;
    hook_terrain_draw = terrain_draw_hook;
    hook_mesh_piece = mesh_piece_hook;
    hook_batch_add = batch_add_hook;
    hook_batch_draw = batch_draw_hook;
    hook_slot_unlit = slot_light_hook;
    hook_slot_lit = slot_light_hook;
    hook_object_shadow = object_shadow_hook;
    hook_models_draw = models_draw_hook;
    const char *fe = getenv("INSANE_FAR");
    far_on = !(fe && !strcmp(fe, "0"));
    far_log = getenv("INSANE_FAR_LOG") ? atoi(getenv("INSANE_FAR_LOG")) : 0;
    far_near = getenv("INSANE_FAR_NEAR") ? atoi(getenv("INSANE_FAR_NEAR")) : 0;
    const char *ce = getenv("INSANE_FAR_COPIES");
    copies_on = !(ce && !strcmp(ce, "0"));
    if (far_on)
        d3d_draw_tap = far_tap;
}

/* the Enhanced page changed an option: the draw distance follows at the next race, the fade now */
void drawdist_options(void)
{
    if (allowed)
        parse();
}
