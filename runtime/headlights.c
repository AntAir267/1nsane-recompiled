/* Headlights (Enhanced option "headlights": off | on; DESIGN.md decision 4, "Headlights").

   The game's own: a car's .vht [lights] section has "light" lines (colour, x y z, range,
   intensity, attenuation), three in the shipped cars, on its centre line 6.5, 12.5 and 22.5 m
   ahead. The loader (0x499a3b) makes each a lamp object, a child of the car, and keeps them in the
   physics record (+0x900 how many, +0x908 the objects, +0x928 their intensities). When the race
   runs on a night map (mapvariation "Night") the lamps are lit (their object's flags 0x100; never
   on the day and morning variations) and the frame's object pass (0x4185d0) puts each lit lamp near
   the camera in the light list as a D3D point light with only an ambient colour (2 x intensity /
   255, white), attenuated 1 / (att0 + att2 d^2) out to its range: the cars and objects ahead get
   brighter; the terrain, drawn unlit (its light baked in), gets nothing, so at night the road
   ahead of the cars is as dark as the rest.

   ON: each car whose lamps the game has lit casts a beam from its headlights (below: its .vht's, or
   its body's front), along its heading tilted a
   little down, wider across than up and cut off sharply above: a low beam. It lights every surface
   in front of it per pixel, the terrain too, falling off with the distance to nothing at 45 m (vk.c's
   race views with ray tracing: shaders/resolve.frag pass 6, after the draws, where a ray from the
   surface to the lamp that meets something takes it away, so cars, trees and posts cast shadows in
   it). Many cars' beams crossing add up softly under a ceiling, never to daylight; the renderer
   takes the nearest 6 in view. The game's own lamps stay as they are. Day races cost nothing: no
   lamp is lit there. OFF changes nothing. Off in --test runs unless INSANE_ENHANCED is set. Needs
   the ray tracing the shadows use (the views it lights are those). INSANE_HL_LOG=1 logs each car's
   lamp once. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "image.h"
#include "vk.h"
#include "headlights.h"

/* ------------------------------------------------------------------ game addresses (GOG build) */
#define CAM_X           0xa4ca90u       /* the camera, 16.16 game units */
#define PLAYER_BLOCKS   0x1c4d988u      /* 0x4d8 a participant: +0 its car's object (+0x98 the physics record) */
#define OBJ_POS         0x30            /* an object: x, y, z (16.16) */
#define OBJ_FLAGS       0x54
#define OBJ_ROT         0xbc            /* 3 x 3 floats: its right, up and back in the world (rows) */
#define LAMP_LIT        0x100           /* a lamp object's flags: lit */
#define PH_POINTS_N     0xc             /* physics record: its points (0x38 bytes, x y z 16.16 from the
                                           object's place) */
#define PH_POINTS       0x1c
#define PH_LAMPS_N      0x900           /* ... its lamps ("light" lines, at most 8) */
#define PH_LAMPS        0x908
#define UNITS_M         66.67f          /* game units in a metre (the world's look) */
#define FADE_NEAR       (80.0f * UNITS_M)   /* a beam fades out from this far from the camera */
#define FADE_FAR        (150.0f * UNITS_M)

static int allowed, want, logon;        /* want: 0 off, 1 on, 2 volumetric */

/* the beam's look: how far it reaches, its half-widths (tangents) across and above its axis (below
   it it is three times as deep), its tilt down, its colour (halogen white) and strength: the light
   near it on a surface facing it (1: the surface's own colour), and where that has halved */
static float reach = 45.0f * UNITS_M, spread_x = 0.55f, spread_y = 0.05f, tilt = 0.035f;
static float color[3] = {1.0f, 0.93f, 0.82f}, strength = 8.0f, half = 20.0f * UNITS_M;
/* the beams' light summed on a surface (several cars' crossing) rises softly towards this, never past
   it: cap (1 - e^(-sum / cap)), as an eye or a camera takes a lit road in (1: the surface as bright
   as its own colour, daylight's) */
static float cap = 0.7f;
/* VOLUMETRIC: how much of a beam's light the night air scatters towards the eye, per metre (its
   light being as above: 1 a surface's own colour) */
static float haze = 0.012f;

/* Where a car's headlights are (Ant: "an El Camino with floating lights": the old guess, the front of
   its physics points' box, stands out past many bodies). Two sources, checked against its body:
   - its .vht's [lights] lines (front, rear, brake, reverse: a lamp model at x y z in the car's frame),
     as the loader (0x498ec6 on) keeps them in the physics record: +0x874 how many (at most 32), +0x1414
     each line's record (+8 x y z, 16.16), +0x868 a bit for each lit at night (front and rear). Its
     headlights: those lit at night in the front quarter of its body (z, its heading being -z), the
     lowest of them (within 15% of the body's height of the lowest: a roof bar over headlights left
     out), of those the outermost pair (within 15% of the widest). Used if they are on the body: each
     within 12 units of a vertex of it and none in front of its nose. Many fan cars' lines are another
     car's, copied and never moved (the Dakar's under its cab), or have none in front;
   - else its body: two thirds out to each side of its bulk (5th..95th percentiles: stray bits left
     out), on its front there (its vertices in its front 15%), two fifths up those, on the frontmost of
     them at that height.
   The body: the physics record's body object (+0x4a8, the car's object itself), its model (+0x90),
   its body mesh at the highest detail (+0x34; +0x54 is its shadow's, a box), root (+0x88): +0xd4
   vertices (the .fff's POS count), +0xec x y z floats in the body's own frame. tools/lamp_check.py does the same from the files (for the fan cars whose .vht is plain text)
   and draws them. The flares go either side of the result, the beam from just in front of it. */
#define PH_LAMP_N       0x874
#define PH_LAMP_LIT     0x868
#define PH_LAMP_REC     0x1414
#define PH_BODY         0x4a8
#define MODEL_BODY      0x34
#define BODY_MAX        20000

static float body[BODY_MAX][3];
static int nbody;

/* the body's vertices in the car's frame (obj's), into body[]; their box */
static int body_verts(uint32_t obj, uint32_t ph, float *lo, float *hi)
{
    uint32_t b = R32(ph + PH_BODY), model = b ? R32(b + 0x90) : 0, mesh = model ? R32(model + MODEL_BODY) : 0;
    uint32_t root = mesh ? R32(mesh + 0x88) : 0, n = root ? R32(root + 0xd4) : 0, v = root ? R32(root + 0xec) : 0;
    nbody = 0;
    if (!v || n < 8 || n > BODY_MAX)
        return 0;
    float off[3];
    for (int k = 0; k < 3; k++)
        off[k] = (float)(int32_t)(R32(b + OBJ_POS + 4 * (uint32_t)k) - R32(obj + OBJ_POS + 4 * (uint32_t)k)) / 65536.0f;
    for (int k = 0; k < 3; k++) {
        lo[k] = 1e9f;
        hi[k] = -1e9f;
    }
    for (uint32_t i = 0; i < n; i++) {
        float w[3];
        for (int k = 0; k < 3; k++) {
            w[k] = off[k];
            for (int r = 0; r < 3; r++)
                w[k] += RF32(v + 12 * i + 4 * (uint32_t)r) * RF32(b + OBJ_ROT + 4 * (uint32_t)(3 * r + k));
        }
        for (int r = 0; r < 3; r++) {
            float c = 0;
            for (int k = 0; k < 3; k++)
                c += w[k] * RF32(obj + OBJ_ROT + 4 * (uint32_t)(3 * r + k));
            if (!isfinite(c))
                return nbody = 0;
            body[i][r] = c;
            lo[r] = fminf(lo[r], c);
            hi[r] = fmaxf(hi[r], c);
        }
    }
    nbody = (int)n;
    return 1;
}

static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float sorted[BODY_MAX];

/* the q-th fraction of the n values sorted[] (sorted here) */
static float pct(int n, float q)
{
    qsort(sorted, (size_t)n, sizeof *sorted, cmp_f);
    int i = (int)(q * (float)(n - 1) + 0.5f);
    return sorted[i < 0 ? 0 : i >= n ? n - 1 : i];
}

static float near_body(const float *p)
{
    float best = 1e18f;
    for (int i = 0; i < nbody; i++) {
        float dx = body[i][0] - p[0], dy = body[i][1] - p[1], dz = body[i][2] - p[2];
        best = fminf(best, dx * dx + dy * dy + dz * dz);
    }
    return sqrtf(best);
}

static int vht_lamps(uint32_t ph, const float *lo, const float *hi, float *at)
{
    uint32_t n = R32(ph + PH_LAMP_N), lit = R32(ph + PH_LAMP_LIT);
    float p[32][3], wide = -1.0f, cx = 0.5f * (lo[0] + hi[0]), y0 = 1e9f;
    int c[32], nc = 0;
    for (uint32_t i = 0; i < n && i < 32; i++) {
        uint32_t s = R32(ph + PH_LAMP_REC + 4 * i);
        if (!(lit >> i & 1) || !s)
            continue;
        for (int k = 0; k < 3; k++)
            p[i][k] = (float)(int32_t)R32(s + 8 + 4 * (uint32_t)k) / 65536.0f;
        if (p[i][2] > lo[2] + 0.25f * (hi[2] - lo[2]))
            continue;
        c[nc++] = (int)i;
        y0 = fminf(y0, p[i][1]);
    }
    int m0 = 0;
    for (int j = 0; j < nc; j++)            /* (the lowest group) */
        if (p[c[j]][1] <= y0 + 0.15f * (hi[1] - lo[1])) {
            c[m0++] = c[j];
            wide = fmaxf(wide, fabsf(p[c[j]][0] - cx));
        }
    nc = m0;
    if (!nc)
        return 0;
    float sum[3] = {0, 0, 0}, dx = 0, zmin = 1e9f;
    int m = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int j = 0; j < nc; j++) {
            const float *q = p[c[j]];
            if (fabsf(q[0] - cx) < 0.85f * wide)
                continue;
            if (pass) {                 /* (either side of their middle: one lamp alone has both flares) */
                dx += fabsf(q[0] - at[0]);
                continue;
            }
            if (nbody && near_body(q) > 12.0f)      /* (not on its body: another car's lines) */
                return 0;
            for (int k = 0; k < 3; k++)
                sum[k] += q[k];
            zmin = fminf(zmin, q[2]);
            m++;
            at[0] = sum[0] / (float)m;
        }
    if (nbody && zmin < lo[2] - 3.0f)       /* (in front of its nose) */
        return 0;
    at[1] = sum[1] / (float)m;
    at[2] = zmin - 2.0f;
    at[3] = dx / (float)m;
    return 1;
}

static int body_lamps(float *at)
{
    int n = nbody;
    if (n < 8)
        return 0;
    for (int i = 0; i < n; i++) sorted[i] = body[i][0];
    float cx = 0.5f * (pct(n, 0.05f) + pct(n, 0.95f));
    for (int i = 0; i < n; i++) sorted[i] = fabsf(body[i][0] - cx);
    float hw = pct(n, 0.95f), lx = 0.65f * hw;
    for (int i = 0; i < n; i++) sorted[i] = body[i][2];
    float zlo = pct(n, 0.02f), zhi = pct(n, 0.98f);
    for (int i = 0; i < n; i++) sorted[i] = body[i][1];
    float band = 0.12f * (pct(n, 0.95f) - pct(n, 0.05f));
    static const float wx[3] = {0.35f, 0.7f, 1e9f};
    int nf = 0, w = 0;
    for (; w < 3; w++) {
        nf = 0;
        for (int i = 0; i < n; i++)
            if (body[i][2] < zlo + 0.15f * (zhi - zlo) && fabsf(fabsf(body[i][0] - cx) - lx) < wx[w] * hw)
                sorted[nf++] = body[i][1];
        if (nf >= 4)
            break;
    }
    if (!nf)
        return 0;
    if (w > 2)
        w = 2;
    float ly = pct(nf, 0.4f), z = 1e9f, zany = 1e9f;
    for (int i = 0; i < n; i++)
        if (body[i][2] < zlo + 0.15f * (zhi - zlo) && fabsf(fabsf(body[i][0] - cx) - lx) < wx[w] * hw) {
            zany = fminf(zany, body[i][2]);
            if (fabsf(body[i][1] - ly) < band)
                z = fminf(z, body[i][2]);
        }
    at[0] = cx;
    at[1] = ly;
    at[2] = (z < 1e8f ? z : zany) - 2.0f;
    at[3] = lx;
    return 1;
}

/* a car's nose: where its lamp sits in its own frame, game units (worked out once a physics record) */
static struct { uint32_t ph, npts; float at[4]; } noses[16];   /* at[3]: its lamps either side of it */

static const float *nose(uint32_t obj, uint32_t ph, int car)
{
    uint32_t n = R32(ph + PH_POINTS_N), pts = R32(ph + PH_POINTS);
    int slot = -1;
    for (int i = 0; i < 16; i++)
        if (noses[i].ph == ph && noses[i].npts == n)
            return noses[i].at;
    for (int i = 0; i < 16 && slot < 0; i++)
        if (noses[i].ph == ph || !noses[i].ph)
            slot = i;
    if (slot < 0)
        slot = car & 15;
    /* (the points are kept in the world: into the car's frame by its rows) */
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t i = 0; pts && i < n && i < 256; i++) {
        float w[3];
        for (int k = 0; k < 3; k++)
            w[k] = (float)(int32_t)(R32(pts + 0x38 * i + 4 * (uint32_t)k) - R32(obj + OBJ_POS + 4 * (uint32_t)k)) / 65536.0f;
        for (int r = 0; r < 3; r++) {
            float v = 0;
            for (int k = 0; k < 3; k++)
                v += w[k] * RF32(obj + OBJ_ROT + 4 * (uint32_t)(3 * r + k));
            lo[r] = fminf(lo[r], v);
            hi[r] = fmaxf(hi[r], v);
        }
    }
    if (!pts || !n)
        for (int k = 0; k < 3; k++)
            lo[k] = hi[k] = 0;
    /* the front (its heading is -z), a little out from it, two fifths up its height */
    float y = lo[1] + 0.4f * (hi[1] - lo[1]);
    noses[slot].ph = ph;
    noses[slot].npts = n;
    noses[slot].at[0] = 0.5f * (lo[0] + hi[0]);
    noses[slot].at[1] = y;
    noses[slot].at[2] = lo[2] - 8.0f;
    noses[slot].at[3] = 0.62f * 0.5f * (hi[0] - lo[0]);   /* (its two lamps, near its front corners) */
    if (logon)
        LOG(LOG_ALWAYS, "headlights: car %d: %u points, x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f: its lamp at %.0f %.0f %.0f",
            car, n, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], noses[slot].at[0], noses[slot].at[1], noses[slot].at[2]);
    /* its own lamps if they are on its body, else on its body as its shape says (above) */
    float lamp[4], blo[3], bhi[3];
    const char *src = "its physics points' box (no body)";
    if (body_verts(obj, ph, blo, bhi)) {
        if (vht_lamps(ph, blo, bhi, lamp))
            src = "its .vht's lamps";
        else if (body_lamps(lamp))
            src = "its body's shape";
        else
            src = NULL;
        if (src)
            memcpy(noses[slot].at, lamp, sizeof lamp);
        else
            src = "its physics points' box (body unsure)";
    } else if (vht_lamps(ph, lo, hi, lamp)) {
        memcpy(noses[slot].at, lamp, sizeof lamp);
        src = "its .vht's lamps (no body to check them on)";
    }
    if (logon)
        LOG(LOG_ALWAYS, "headlights: car %d: lamps from %s: at %.0f %.0f %.0f, either side %.0f (body %d vertices)",
            car, src, noses[slot].at[0], noses[slot].at[1], noses[slot].at[2], noses[slot].at[3], nbody);
    return noses[slot].at;
}

/* car p's lamps are lit (the game's: a night map) */
static int lit(uint32_t ph)
{
    uint32_t n = R32(ph + PH_LAMPS_N);
    for (uint32_t k = 0; k < n && k < 8; k++) {
        uint32_t l = R32(ph + PH_LAMPS + 4 * k);
        if (l && R32(l + OBJ_FLAGS) & LAMP_LIT)
            return 1;
    }
    return 0;
}

static uint32_t car_ph(uint32_t p, uint32_t *obj)
{
    *obj = R32(PLAYER_BLOCKS + 0x4d8u * p);
    return *obj ? R32(*obj + 0x98) : 0;
}

int headlights_on(void)
{
    if (!allowed || !want)
        return 0;
    uint32_t np = R32(GAME_PARTICIPANT_COUNT), obj;
    for (uint32_t p = 0; p < np && p < 16; p++) {
        uint32_t ph = car_ph(p, &obj);
        if (ph && lit(ph))
            return 1;
    }
    return 0;
}

typedef struct {
    VkBeam b;
    float d;
} Cand;

static int cand_cmp(const void *a, const void *b)
{
    float x = ((const Cand *)a)->d, y = ((const Cand *)b)->d;
    return x < y ? -1 : x > y;
}

int headlights_beams(VkBeam *out, int max, float *sum_cap, float *air, float *tint)
{
    *sum_cap = cap;
    *air = want == 2 ? haze : 0.0f;
    for (int k = 0; k < 3; k++)
        tint[k] = color[k] / color[1];
    if (!headlights_on())
        return 0;
    Cand c[16];
    int n = 0;
    uint32_t np = R32(GAME_PARTICIPANT_COUNT), obj;
    int32_t cam[3];
    for (int k = 0; k < 3; k++)
        cam[k] = (int32_t)R32(CAM_X + 4 * (uint32_t)k);
    for (uint32_t p = 0; p < np && p < 16; p++) {
        uint32_t ph = car_ph(p, &obj);
        if (!ph || !lit(ph))
            continue;
        const float *at = nose(obj, ph, (int)p);
        float rot[3][3];
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++)
                rot[r][k] = RF32(obj + OBJ_ROT + 4 * (uint32_t)(3 * r + k));
        Cand *q = &c[n];
        float d2 = 0;
        for (int k = 0; k < 3; k++) {
            float w = 0;
            for (int r = 0; r < 3; r++)
                w += at[r] * rot[r][k];
            q->b.pos[k] = (float)((int32_t)((uint32_t)R32(obj + OBJ_POS + 4 * (uint32_t)k) - (uint32_t)cam[k])) / 65536.0f + w;
            d2 += q->b.pos[k] * q->b.pos[k];
            q->b.up[k] = rot[1][k];
            q->b.dir[k] = -rot[2][k] - tilt * rot[1][k];
        }
        float l = sqrtf(q->b.dir[0] * q->b.dir[0] + q->b.dir[1] * q->b.dir[1] + q->b.dir[2] * q->b.dir[2]);
        if (l <= 0 || !isfinite(l))
            continue;
        for (int k = 0; k < 3; k++)
            q->b.dir[k] /= l;
        /* far from the camera faded out (80 to 150 m; a pool that far is small and fogged), so a car
           leaving the nearest few the renderer takes goes without a jump */
        q->d = sqrtf(d2);
        float fade = 1.0f - fminf(fmaxf((q->d - FADE_NEAR) / (FADE_FAR - FADE_NEAR), 0.0f), 1.0f);
        if (fade <= 0)
            continue;
        q->b.range = reach;
        q->b.spread[0] = spread_x;
        q->b.spread[1] = spread_y;
        for (int k = 0; k < 3; k++)
            q->b.color[k] = color[k] * strength * fade;
        q->b.half = half;
        q->b.lamp_dx = at[3];
        q->b.fade = fade;
        n++;
    }
    qsort(c, (size_t)n, sizeof *c, cand_cmp);     /* the nearest the camera first */
    if (n > max)
        n = max;
    for (int i = 0; i < n; i++)
        out[i] = c[i].b;
    return n;
}

/* ------------------------------------------------------------------ options */
static void parse(void)
{
    const char *v = option_str("headlights", "off");
    want = !strcasecmp(v, "on") ? 1 : !strcasecmp(v, "volumetric") ? 2 : 0;
    if (!want && strcasecmp(v, "off"))
        LOG(LOG_ALWAYS, "options: headlights \"%s\" is not off, on or volumetric", v);
}

void headlights_init(void)
{
    logon = getenv("INSANE_HL_LOG") != NULL;
    allowed = !deterministic || getenv("INSANE_ENHANCED");
    if (!allowed)
        return;
    const char *e;
    if ((e = getenv("INSANE_HL_STRENGTH")) && atof(e) > 0)
        strength = (float)atof(e);
    if ((e = getenv("INSANE_HL_REACH")) && atof(e) > 0)
        reach = (float)atof(e) * UNITS_M;
    if ((e = getenv("INSANE_HL_HALF")) && atof(e) > 0)
        half = (float)atof(e) * UNITS_M;
    if ((e = getenv("INSANE_HL_CAP")) && atof(e) > 0)
        cap = (float)atof(e);
    if ((e = getenv("INSANE_HL_HAZE")) && atof(e) >= 0)
        haze = (float)atof(e);
    if ((e = getenv("INSANE_HL_SPREAD")))
        sscanf(e, "%f,%f", &spread_x, &spread_y);
    if ((e = getenv("INSANE_HL_TILT")))
        tilt = (float)atof(e);
    parse();
}

void headlights_options(void)
{
    if (allowed)
        parse();
}
