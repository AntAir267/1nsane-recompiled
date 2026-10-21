/* Fan cars whose data the game cannot cope with as it is (design point 8). Each fix takes
   effect only for data the game would fail on; stock cars and every other fan car run exactly as
   shipped.

   1. Car models bigger than the game's scratch buffers for them (BMW M5, Monster Hummer, Celica
   GT, Bugatti Veyron and other detailed models). Two kinds of per-frame work copy a car model's
   mesh into fixed buffers, with no check:
   - its shadow (0x41a6b0, each race frame): the body and up to 15 parts that cast one (0x465b60
     lists them), each part's mesh pieces turned into vertices (12 bytes each, at 0x21f9ab8:
     room for 2048), their places on the shadow texture (8 bytes each, at 0xa1b1c0) and
     triangles (28 bytes each, at 0xa0ad68: room for 2048);
   - a mesh that follows the physics body (0x4185d0 and the other model draws, by the mesh's
     flags: 0x49c350 -> 0x49bef0 and 0x49c370 skinned to the body's points, 0x49c180 and
     0x49c270 vertex by vertex): the whole mesh is read into the same 2048 vertices at
     0x21f9ab8, moved, and written back.
   Stock cars stay well inside (at most 396 shadow vertices, 758 triangles, 500 mesh vertices). A
   model with more makes the original game overwrite what follows, and it soon crashes (the
   reference build, the original code under emulation, crashes the same way):
   - shadow triangles past 2048 run over the detail levels' table (0xa18d70: pointers to detail
     settings), which the next detail change (0x41e320) writes through: a bad memory write (BMW
     M5, Monster Hummer); further on over the object shadows' range (0xa1b1b8), which 0x419eb0
     divides by: a divide by zero (Celica GT);
   - vertices past 2048 run over a few variables (a physics pointer at 0x21ffab8, an effect's
     resource at 0x2200008) and from 2171 on the list of physics vehicles in use (0x2200074):
     the next frame's shadow loop writes into a vehicle at a garbage address (Veyron's body, a
     skinned mesh of 2179 vertices). 31 more fan cars have body meshes of 2049-2158 vertices
     (the first variables only, no crash seen in a 40 s drive).
   Here such a call works in bigger buffers of ours: TABLES model_verts (these five functions'
   references to 0x21f9ab8), shadow_points and shadow_tris point there for that one call. The
   shadow texture's painter (0x416090) reads any number of triangles from wherever they are.

   2. An air intake under the car (Mini Cooper S: "intake -19 -50 -10", half a metre down, under
   its wheels). The engine floods (0x4a2e6d: the intake under the water's surface for a while
   stops the engine: "flooded", and the car is soon rescued) wherever a map's water lies just
   under its ground, as on Speed Zone: the Mini never gets going there (2 km/h). An intake under
   the bottom of the car's own wheels is a slip of the sign: as the car loads (0x494240), its
   height is mirrored (Mini: 0.5 m up, where other cars have theirs), with a log line.

   3. Parts off the body in the vehicle preview (Ant: "detachable elements hover off of the
   car"; the original does the same): see "parts in the vehicle preview" below.

   4. Parts tied to physics points the car doesn't have, or to points in a line (12 cars): see
   "parts tied to bad points" below.

   5. Cars that drive off during the countdown (9 cars with a negative handbrake): see "the
   countdown's hold" below.

   6. Two slips in a .vht's mass (Dakar: 10000 t, Army Transporter: 358.47 t): see "a car's mass"
   below. */
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "funcs.h"
#include "image.h"
#include "mods.h"

#define VERTS_ROOM 2048u                /* 0x21f9ab8 (12 bytes each), 0xa1b1c0 (8 bytes each) */
#define TRIS_ROOM  2048u                /* 0xa0ad68 (28 bytes each) */
#define MAX_PARTS  16                   /* 0x41a6b0 takes at most 16 */
#define MAX_PIECES 32                   /* 0x40d5f0(root, 32, ...) */

static int (*prev_car_shadow)(CPU *c);
static const TableEntry *t_verts, *t_points, *t_tris;
static uint32_t big_verts, big_points, big_tris, room_v, room_t;
static int log_sizes;

static const TableEntry *table(const char *name)
{
    for (const TableEntry *t = table_list; t->name; t++)
        if (!strcmp(t->name, name))
            return t;
    fatal("fancars: no table %s in this build", name);
}

/* Our buffers, with room for at least `nv` vertices and `nt` triangles (a smaller set made
   before stays allocated: this happens once or twice a session). */
static void room(uint32_t nv, uint32_t nt, const char *what)
{
    if (nv <= room_v && nt <= room_t)
        return;
    uint32_t rv = (nv + 4095) & ~4095u, rt = (nt + 4095) & ~4095u;
    if (rv < room_v) rv = room_v;
    if (rt < room_t) rt = room_t;
    if (rv < VERTS_ROOM) rv = VERTS_ROOM;
    if (rt < TRIS_ROOM) rt = TRIS_ROOM;
    uint32_t a = vm_alloc_table(rv * 12), b = vm_alloc_table(rv * 8), d = vm_alloc_table(rt * 28);
    if (!a || !b || !d)
        fatal("fancars: no memory for %u vertices and %u triangles", nv, nt);
    big_verts = a; big_points = b; big_tris = d; room_v = rv; room_t = rt;
    logmsg(LOG_ALWAYS, "fancars: %s of %u vertices and %u triangles (the game has room for %u and %u): "
           "worked on in bigger buffers", what, nv, nt, VERTS_ROOM, TRIS_ROOM);
}

static void use_big(int on)
{
    *t_verts->reloc = on ? big_verts : t_verts->base;
    *t_points->reloc = on ? big_points : t_points->base;
    *t_tris->reloc = on ? big_tris : t_tris->base;
}

/* ------------------------------------------------------------------ car shadows */
/* The vertices and triangles 0x41a6b0 makes of `n` parts (part list at `parts`): what its two
   passes add up. A mesh root's pieces are its +0x100 list (0x40d5f0: a node counts while its
   next pointer is set; +0x68 type, +0x70 count, +0x8c index words); its vertices +0xd4
   (0x40d510). Type 1: strips of index words (type, count, indices), count - 2 triangles each
   for strip types 1-3; type 5: count quads (4 more vertices, 2 triangles each). */
static void shadow_size(uint32_t parts, uint32_t n, uint32_t *nv, uint32_t *nt)
{
    uint64_t v = 0, t = 0;
    if (n > MAX_PARTS) n = MAX_PARTS;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = R32(parts + 4 * i), model = p ? R32(p + 0x90) : 0;
        uint32_t mesh = model ? R32(model + 0x54) : 0, root = mesh ? R32(mesh + 0x88) : 0;
        if (!root) {
            if (i == 0) break;          /* no body: no shadow at all */
            continue;
        }
        uint32_t node = R32(root + 0x100), k = 0, pv = 0, pt = 0;
        for (; node && R32(node) && k < MAX_PIECES; node = R32(node), k++) {
            uint32_t type = R32(node + 0x68), count = R32(node + 0x70);
            if (type == 5) {
                pv += 4 * count;
                pt += 2 * count;
            } else if (type == 1) {
                uint32_t w = R32(node + 0x8c), left = count;
                while (left >= 2) {
                    uint32_t st = R16(w), cnt = R16(w + 2);
                    if (st >= 1 && st <= 3 && cnt > 2)
                        pt += cnt - 2;
                    if (cnt + 2 > left) break;      /* (malformed: the game would read on) */
                    left -= cnt + 2;
                    w += 2 * (cnt + 2);
                }
            }
        }
        if (!k) continue;
        v += R32(root + 0xd4) + pv;
        t += pt;
    }
    *nv = v > 0xffffffu ? 0xffffffu : (uint32_t)v;
    *nt = t > 0xffffffu ? 0xffffffu : (uint32_t)t;
}

/* 0x41a6b0(record, parts, part list, light) */
static int car_shadow_hook(CPU *c)
{
    uint32_t n = ARG(1), parts = ARG(2), nv = 0, nt = 0;
    if (n && parts)
        shadow_size(parts, n, &nv, &nt);
    if (log_sizes) {
        static uint32_t last_rec[16], last_v[16], last_t[16];
        uint32_t rec = ARG(0), slot = (rec >> 4) % 16;
        if (last_rec[slot] != rec || last_v[slot] != nv || last_t[slot] != nt)
            logmsg(LOG_ALWAYS, "fancars: car shadow %08x: %u parts, %u vertices, %u triangles", rec, n, nv, nt);
        last_rec[slot] = rec; last_v[slot] = nv; last_t[slot] = nt;
    }
    if (nv <= VERTS_ROOM && nt <= TRIS_ROOM)
        return prev_car_shadow ? prev_car_shadow(c) : 0;
    room(nv, nt, "a car's shadow");
    use_big(1);
    if (!(prev_car_shadow && prev_car_shadow(c)))
        F_0041a6b0_orig(c);
    use_big(0);
    return 1;
}

/* ------------------------------------------------------------------ meshes on the physics body */
/* (object, mesh link): the link's mesh (+0x88) is read whole (its +0xd4 vertices) or up to
   +0xc of them */
static int mesh_call(CPU *c, void (*orig)(CPU *c))
{
    uint32_t link = ARG(1), root = link ? R32(link + 0x88) : 0;
    uint32_t n = root ? R32(root + 0xd4) : 0, m = link ? R32(link + 0xc) : 0;
    if (m > n) n = m;
    if (log_sizes) {
        static uint32_t last_link[64], last_n[64];
        uint32_t slot = (link >> 4) % 64;
        if (last_link[slot] != link || last_n[slot] != n)
            logmsg(LOG_ALWAYS, "fancars: mesh %08x on a body: %u vertices", link, n);
        last_link[slot] = link; last_n[slot] = n;
    }
    if (n <= VERTS_ROOM)
        return 0;
    room(n, 0, "a mesh on a car's body");
    use_big(1);
    orig(c);
    use_big(0);
    return 1;
}

static int mesh_skin_nodes_hook(CPU *c) { return mesh_call(c, F_0049bef0_orig); }
static int mesh_parts_hook(CPU *c) { return mesh_call(c, F_0049c180_orig); }
static int mesh_points_hook(CPU *c) { return mesh_call(c, F_0049c270_orig); }
static int mesh_skin_points_hook(CPU *c) { return mesh_call(c, F_0049c370_orig); }

/* ------------------------------------------------------------------ a car's mass */
/* The .vht's [misc] "mass" (the stock files: "total vehicle weight, tons (only counts for
   collisions)") goes to the car's object (0x494240: +0x80, and +0x84 = mass x 7000, its turning
   inertia, unless a "teta" line sets that). The car's own physics never reads it: its points move
   as equal masses held by springs, the engine drives the wheels, so speed and acceleration don't
   depend on it. What reads it (a watch on +0x80 through a race and the vehicle screen):
   - two objects meeting (0x488070, cars, and cars with the map's loose objects): the push that
     parts them is shared by their masses, each taking the other's share (m2 / (m1 + m2)); the
     share also decides whether a loose object is knocked over or broken; for a car (0x49eff0)
     the share scales its bounce and spin (the spin's 1 / (1 / m + r^2 / teta) over teta: with
     teta = 7000 m the mass itself cancels, only the share counts);
   - the AI (0x444540, as a race starts): a car over 4 t with a wide track is a heavy vehicle,
     steering round obstacles twice as hard (0x443790);
   - the vehicle screen's "Mass: ... KG" (MV_VSTAT_MASS, 0x47cdb0: x 1000).
   Stock cars: 0.35 (ATV, Gnu) to 2.4 t, the trucks 5 (4x4 Freighter), 6 (6x6 Goliath) and
   7 t (8x8 Behemoth). Two fan cars carry an obvious slip, fixed here as the car loads, keyed to
   the file name and the exact value (one log line each):
   - Dakar (dakar, a Kamaz Dakar rally truck on the Goliath's physics body): 10000 t, kilograms
     typed where tonnes go: 10 t, as its authors meant (a real one weighs about 10 t; in its class
     between the DAF Dakar's 8 and the rigs' 12.5). As it was, every car met it as a wall: a
     1.5 t car took 99.98% of the push, the 8x8 Behemoth 99.9%;
   - Army Transporter (armytran, a 6x6 on a body the size of the stock trucks'): 358.47 t, the
     decimal point two places off (its engine's "mass", a ratio the stock files keep within
     0.3-1, reads 50.60 the same way): 3.5847 t. 35.8 t would have kept it five times the
     heaviest stock truck and a near wall to every stock car.
   The bathtub's 850000 t (a joke), the Komatsu 930E's 190 t (the real truck) and the 0.01 t
   skateboard and speeder bike are meant and stay. */
#define TETA_PER_TONNE 7000.0f          /* 0x494240: +0x84 = mass x 7000 (0x4bf81c) */

static int stock_car(const char *name);
static float fl(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static void wf32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }

static const struct { const char *car; float was, now; const char *why; } mass_slips[] = {
    {"dakar", 10000.0f, 10.0f, "kilograms typed where tonnes go"},
    {"armytran", 358.47f, 3.5847f, "the decimal point two places off"},
};

static void mass_fix(uint32_t obj, const char *car)
{
    if (!obj || !mods_active || stock_car(car))
        return;
    float m = fl(obj + 0x80), teta = fl(obj + 0x84);
    for (size_t k = 0; k < sizeof mass_slips / sizeof *mass_slips; k++) {
        const typeof(mass_slips[0]) *s = &mass_slips[k];
        if (strcasecmp(car, s->car) || m != s->was)
            continue;
        wf32(obj + 0x80, s->now);
        if (fabsf(teta - s->was * TETA_PER_TONNE) <= 1e-6f * s->was * TETA_PER_TONNE)
            wf32(obj + 0x84, s->now * TETA_PER_TONNE);     /* (no "teta" line: as the game works it out) */
        static uint32_t told;
        if (!(told & 1u << k))
            logmsg(LOG_ALWAYS, "fancars: %s: its .vht mass %g t is a slip (%s): taken as %g t", car, s->was,
                   s->why, s->now);
        told |= 1u << k;
        return;
    }
}

/* ------------------------------------------------------------------ an intake under the car */
/* 0x493d70(record, file): the car's .vhx physics points (record +0xc count, +0x1c points of
   0x38 bytes: x, y, z 16.16, +0x18 radius, +0x1c wheel number or 0), as in the file: the lowest
   wheel bottom, for the load that follows */
#define LOADS 4
static struct { uint32_t rec; int32_t bottom; } wheels[LOADS];
static uint32_t loaded_obj;             /* the last car loaded, for the preview's log line */
static char loaded_name[32];
static int loading;                     /* inside 0x494240, loading loading_name */
static char loading_name[32];
static int stock_car(const char *name);
static int nwheels_seen;

static int vhx_load_hook(CPU *c)
{
    uint32_t rec = ARG(0);
    F_00493d70_orig(c);
    if (!c->eax || !rec)
        return 1;
    uint32_t n = R32(rec + 0xc), pts = R32(rec + 0x1c);
    int64_t bottom = INT64_MAX;
    for (uint32_t i = 0; i < n && i < 4096; i++) {
        uint32_t p = pts + 0x38 * i;
        if (R32(p + 0x1c)) {
            int64_t b = (int64_t)(int32_t)R32(p + 4) - (int32_t)R32(p + 0x18);
            if (b < bottom) bottom = b;
        }
    }
    if (bottom == INT64_MAX || bottom < INT32_MIN)
        return 1;
    int k = nwheels_seen++ % LOADS;
    wheels[k].rec = rec;
    wheels[k].bottom = (int32_t)bottom;
    return 1;
}

/* 0x494240(name, ...) -> the car's object (+0x98 its physics record), 0 if none */
static int vehicle_load_hook(CPU *c)
{
    uint32_t name = ARG(0);
    char car[32] = "?";
    for (int i = 0; name && i < 31 && R8(name + i); i++) {
        car[i] = (char)R8(name + i);
        car[i + 1] = 0;
    }
    int seen = nwheels_seen;
    snprintf(loading_name, sizeof loading_name, "%s", car);
    loading = 1;
    F_00494240_orig(c);
    loading = 0;
    uint32_t obj = c->eax, rec = obj ? R32(obj + 0x98) : 0;
    loaded_obj = obj;
    snprintf(loaded_name, sizeof loaded_name, "%s", car);
    mass_fix(obj, car);
    if (!rec || nwheels_seen == seen)
        return 1;
    int32_t bottom = 0;
    int found = 0;
    for (int k = 0; k < LOADS && !found; k++)
        if (wheels[k].rec == rec) {
            bottom = wheels[k].bottom;
            found = 1;
        }
    if (!found)
        return 1;
    /* the points as loaded are scaled after 0x493d70 by the world scale, as the intake is */
    uint32_t u = R32(0x4cf958);
    float scale;
    memcpy(&scale, &u, 4);
    double low = bottom * (double)scale;
    int32_t y = (int32_t)R32(rec + 0x2a4);
    if (y >= low || y == INT32_MIN)
        return 1;
    W32(rec + 0x2a4, (uint32_t)-y);
    static char told[8][32];            /* once a car (AI copies and restarts load it again) */
    for (int k = 0; k < 8; k++)
        if (!strcmp(told[k], car))
            return 1;
    static int ntold;
    snprintf(told[ntold++ % 8], sizeof told[0], "%s", car);
    logmsg(LOG_ALWAYS, "fancars: %s: its air intake (%.2f, %.2f, %.2f m) is under its wheels (lowest point "
           "%.2f m), where its engine floods on dry ground: put at %.2f m up", car,
           (int32_t)R32(rec + 0x2a0) / 6553600.0, y / 6553600.0, (int32_t)R32(rec + 0x2a8) / 6553600.0,
           low / 6553600.0, -y / 6553600.0);
    return 1;
}

/* ------------------------------------------------------------------ the countdown's hold */
/* 0x49f9b0(object, controls, participant): a car's controls for the next physics steps: steer,
   throttle, brake and handbrake (+0x1b14, +0x1b18, +0x1b1c; 0-127). During the countdown (race
   state 1) the game holds every car with its handbrake: the player's at 100 (and the AI's at
   120, 0x4461da). The handbrake's strength is the .vht's "handbrake" (+0x254, 16.16): each step
   it takes that much off each handbrake wheel's spin, stopping it at 0 (0x4a381c). Nine fan
   cars have a negative handbrake (Porsche 911 AWD Turbo, fido's drag "nos", Dragster, Nissan
   Silvia Drift and Falken, McLaren Based KitCar, K.I.T.T., N.O.S. Mitsubishi Eclipse, Toyota
   Supra: -1.0 to -5.0, a boost: the wheels spin faster the longer it is held), so the hold
   spins them up instead and they are at 180-420 km/h when GO shows.
   For such a car the countdown's hold is put on its foot brake (positive on every car) instead:
   it stands still until GO, and the handbrake stays the boost its author made it afterwards. */
static int vehicle_controls_hook(CPU *c)
{
    uint32_t obj = ARG(0);
    F_0049f9b0_orig(c);
    uint32_t ph = obj ? R32(obj + 0x98) : 0;
    if (!ph || !mods_active || R32(GAME_RACE_STATE) != 1 || (int32_t)R32(ph + 0x254) >= 0)
        return 1;
    int32_t hold = (int32_t)R32(ph + 0x1b1c);
    if (hold <= 0)
        return 1;
    if ((int32_t)R32(ph + 0x1b18) < hold)
        W32(ph + 0x1b18, (uint32_t)hold);
    W32(ph + 0x1b1c, 0);
    static uint32_t told[16];
    for (int k = 0; k < 16; k++)
        if (told[k] == ph)
            return 1;
    static int ntold;
    told[ntold++ % 16] = ph;
    logmsg(LOG_ALWAYS, "fancars: a car with a negative handbrake (%.1f) is held on its foot brake through the "
           "countdown", (int32_t)R32(ph + 0x254) / 65536.0);
    return 1;
}

/* ------------------------------------------------------------------ parts tied to bad points */
/* 0x4940d0(record, position, offset, a, b, c): as a car loads (0x494240), a part, light, the
   steering wheel or a seat/pedal is tied to three of the car's physics points: its position is
   kept as an offset in the frame of the triangle a, b, c (b - a, c - a and their cross product),
   which 0x49ca10 turns back into a place every frame. The numbers come from the .vht and are
   stored before the call (12 bytes before: 0x180 for the parts and lights, 0x90 for the rest);
   0 0 0 means not tied. Of the 5419 ties in Ant's 406 fan cars, all but 23 give back their .vht
   place within 4.5 cm (0.2 cm for 99%; 4.5 is a light tied three times to the point it sits on,
   which the game puts on that point). The 23, in 12 cars: rear lights on points 25-27 of a
   24-point car (Monster Hummer, Honda Civic, Honda CRX, Plymouth Duster, VW Jetta, Mitsubishi
   Eclipse and its N.O.S. copy, '69 Oldsmobile: the game reads past the points, the light sits
   wherever that memory says) and parts on points in a line (IRC Cruiser, Rock Crusher, Slinky,
   Madmax Truck: the frame has no third direction, the offset is garbage, the part is flung tens
   of kilometres off and knocked off).
   Here such a part (tied to points past the car's, or given back more than 25 cm from its .vht
   place by the game's own offset) is tied instead to the three points nearest its .vht position
   that make a proper triangle (not the wheels' own points, which spin and steer), stored where
   the game keeps its numbers: it sits on the car in races and previews and comes off in crashes
   like any other part; one log line each. Stock cars are left as they are. */
#define TRI_MIN_EDGE (5 * 65536.0)      /* 5 cm */

static void point_at(uint32_t pts, uint32_t i, double *v)
{
    for (int k = 0; k < 3; k++)
        v[k] = (int32_t)R32(pts + 0x38 * i + 4 * k);
}

/* sin of the angle at a in the triangle a, b, c (0 for a degenerate one) */
static double tri_sin(const double *a, const double *b, const double *c)
{
    double u[3], v[3], x[3];
    for (int k = 0; k < 3; k++) {
        u[k] = b[k] - a[k];
        v[k] = c[k] - a[k];
    }
    x[0] = u[1] * v[2] - u[2] * v[1];
    x[1] = u[2] * v[0] - u[0] * v[2];
    x[2] = u[0] * v[1] - u[1] * v[0];
    double lu = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), lv = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (lu < TRI_MIN_EDGE || lv < TRI_MIN_EDGE)
        return 0;
    return sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) / (lu * lv);
}

/* Where the game will put a part tied to t with the offset at off, at rest (0x49ca10's sum:
   a + (b - a) o0 + (c - a) o1 + ((b - a) x (c - a)) o2, the cross product in 16.16 as 0x480340
   does it, in floats as 0x480a90), against its .vht position: the distance, in 16.16 units. */
static double attach_error(uint32_t pts, const uint32_t *t, const double *p, uint32_t off)
{
    int32_t A[3], B[3], C[3], u[3], v[3], w[3], o[3];
    for (int k = 0; k < 3; k++) {
        A[k] = (int32_t)R32(pts + 0x38 * t[0] + 4 * k);
        B[k] = (int32_t)R32(pts + 0x38 * t[1] + 4 * k);
        C[k] = (int32_t)R32(pts + 0x38 * t[2] + 4 * k);
        u[k] = (int32_t)((uint32_t)B[k] - (uint32_t)A[k]);
        v[k] = (int32_t)((uint32_t)C[k] - (uint32_t)A[k]);
        o[k] = (int32_t)R32(off + 4 * k);
    }
#define FX(x, y) ((int32_t)(uint32_t)((uint64_t)((int64_t)(x) * (int64_t)(y)) >> 16))
    w[0] = (int32_t)((uint32_t)FX(u[1], v[2]) - (uint32_t)FX(u[2], v[1]));
    w[1] = (int32_t)((uint32_t)FX(u[2], v[0]) - (uint32_t)FX(u[0], v[2]));
    w[2] = (int32_t)((uint32_t)FX(u[0], v[1]) - (uint32_t)FX(u[1], v[0]));
#undef FX
    double e = 0;
    for (int k = 0; k < 3; k++) {
        float d = (float)u[k] * 1.5258789e-05f * (float)o[0] + (float)v[k] * 1.5258789e-05f * (float)o[1] +
                  (float)w[k] * 1.5258789e-05f * (float)o[2];
        double place = (double)A[k] + (isfinite(d) ? d : 1e30);
        e += (place - p[k]) * (place - p[k]);
    }
    return sqrt(e);
}

#define ATTACH_OFF_MAX (25 * 65536.0)   /* farther from its .vht place than 25 cm: not where meant */

static int in_attach;

static int attach_hook(CPU *c)
{
    if (in_attach || !mods_active || !loading || stock_car(loading_name))
        return 0;
    uint32_t ph = ARG(0), pos = ARG(1), off = ARG(2), t[3] = {ARG(3), ARG(4), ARG(5)};
    if (!t[0] && !t[1] && !t[2])
        return 0;
    uint32_t n = R32(ph + 0xc), pts = R32(ph + 0x1c);
    double p[3];
    for (int k = 0; k < 3; k++)
        p[k] = (int32_t)R32(pos + 4 * k);
    F_004940d0_orig(c);
    const char *why = NULL;
    double err = 0;
    if (t[0] >= n || t[1] >= n || t[2] >= n)
        why = "points the car doesn't have";
    else if ((err = attach_error(pts, t, p, off)) > ATTACH_OFF_MAX)
        why = "points in a line";
    if (log_sizes)
        logmsg(LOG_ALWAYS, "fancars: %s: attached at %u %u %u (of %u), %.1f cm from its place", loading_name,
               t[0], t[1], t[2], n, err / 65536);
    if (!why)
        return 1;
    /* the nearest point, the next one 5 cm or more from it, then the nearest making a triangle */
    int best[3] = {-1, -1, -1};
    for (int pass = 0; pass < 3; pass++) {
        double bd = 1e300;
        for (uint32_t i = 0; i < n && i < 4096; i++) {
            if (R32(pts + 0x38 * i + 0x1c) & 8)         /* a wheel's point */
                continue;
            if ((int)i == best[0] || (int)i == best[1])
                continue;
            double v[3], d = 0;
            point_at(pts, i, v);
            for (int k = 0; k < 3; k++)
                d += (v[k] - p[k]) * (v[k] - p[k]);
            if (d >= bd)
                continue;
            if (pass >= 1) {
                double a[3], e = 0;
                point_at(pts, best[0], a);
                for (int k = 0; k < 3; k++)
                    e += (v[k] - a[k]) * (v[k] - a[k]);
                if (sqrt(e) < TRI_MIN_EDGE)
                    continue;
            }
            if (pass == 2) {
                double a[3], b[3];
                point_at(pts, best[0], a);
                point_at(pts, best[1], b);
                if (tri_sin(a, b, v) < 0.3 || tri_sin(b, a, v) < 0.3)
                    continue;
            }
            bd = d;
            best[pass] = (int)i;
        }
        if (best[pass] < 0) {
            logmsg(LOG_ALWAYS, "fancars: %s: a part at (%.0f, %.0f, %.0f) is tied to %s (%u %u %u of %u); "
                   "no three points near it to tie it to: left as it is", loading_name, p[0] / 65536,
                   p[1] / 65536, p[2] / 65536, why, t[0], t[1], t[2], n);
            return 1;
        }
    }
    uint32_t store = 0;
    for (int k = 0; k < 2 && !store; k++) {
        uint32_t at = off - (k ? 0x90 : 0x180);
        if (R32(at) == t[0] && R32(at + 4) == t[1] && R32(at + 8) == t[2])
            store = at;
    }
    if (store)
        for (int k = 0; k < 3; k++)
            W32(store + 4 * k, (uint32_t)best[k]);
    in_attach = 1;
    guest_call(c, 0x4940d0, 6, ph, pos, off, (uint32_t)best[0], (uint32_t)best[1], (uint32_t)best[2]);
    in_attach = 0;
    uint32_t nt[3] = {(uint32_t)best[0], (uint32_t)best[1], (uint32_t)best[2]};
    logmsg(LOG_ALWAYS, "fancars: %s: a part at (%.0f, %.0f, %.0f) is tied to %s (%u %u %u of %u): tied to "
           "%d %d %d, the nearest, %.1f cm from its place%s", loading_name, p[0] / 65536, p[1] / 65536,
           p[2] / 65536, why, t[0], t[1], t[2], n, best[0], best[1], best[2],
           attach_error(pts, nt, p, off) / 65536, store ? "" : " (its numbers not found: offset only)");
    return 1;
}

/* ------------------------------------------------------------------ parts in the vehicle preview */
/* 0x49b750(object, menu): a loaded car placed (menu 1: the vehicle screens' previews, on level
   ground; 0 in races). The car's object sits at the centre of its first four physics points
   (0x49f660) and turns with them (0x49f6f0). What hangs on the body are links holding a place
   relative to that object: the .vht's "child" parts (lights, bars, racks, mirrors, plates,
   engines; up to 32 at physics +0xf14, those knocked off flagged in +0x98c), the [lights] glows
   (+0x1414, count +0x874), the steering wheel (+0x1b00) and a few more (+0x1b04, +0x1afc).
   Each is loaded at its .vht position and tied to three physics points. In a race 0x49ca10
   moves each, every frame, to where its points put it (a quarter of the way, at once when a
   wheel's point is one of them): the .vht position measured from the model's origin. The
   previews never call it, so there a part keeps its .vht position measured from the points'
   centre. Stock cars have their four points centred on the origin (Pod and Wall within 4 cm);
   345 of the 406 fan cars in Ant's folders do not, by up to a metre (Partiot's body
   93 cm above its chassis, a Land Cruiser's roof rack 55 cm above the roof; the Mini Cooper S's
   own readme: "the car in menu will have missplaced lights and parts"). The reference build
   places them the same way. Parts with no points (0 0 0) keep their .vht place in races too.

   With fan content, a fan car's preview runs 0x49ca10 until the parts settle (64 times: their
   places at rest; a race start is within about 1 cm, more where soft suspension has sagged,
   up to 17 cm on the Truggy) when that moves one by 1 cm or more; otherwise, and for stock
   cars, everything it wrote is put back. A part tied to point numbers the car doesn't have
   (the game reads past its points: the Monster Hummer's and Honda Civic's rear lights) or to
   points that give it no sensible place (in a line: flung far off, as in a race, where it is
   knocked off at once: IRC Cruiser, Rock Crusher) stays where the game put it: 13 cars have
   one. INSANE_FANCARS_LOG=1 logs every preview's parts. */
#define PREVIEW_STEP_MIN 65536          /* 1 cm (16.16 game units) */
#define LINK_PARTS 32
#define PLACES_MAX 128

/* where 0x49ca10 writes: x, y, z of the parts' links (+8), the other linked things it moves
   (+0x1b04, +0x1b00, +0x1afc, the lights' links at +0x1414, count +0x874) and a point of the
   record's own (+0x27c); the attached parts first, in their .vht order */
typedef struct { uint32_t at, points; int32_t pos[3]; } Place;     /* points: its 3 point numbers */

static int preview_places(uint32_t ph, Place *out, int *parts)
{
    int n = 0;
    uint32_t count = R32(ph + 0x988), off = R32(ph + 0x98c);
    for (uint32_t i = 0; i < count && i < LINK_PARTS; i++) {
        uint32_t lk = R32(ph + 0xf14 + 4 * i);
        if (lk && !(off & 1u << i))
            out[n++] = (Place){lk + 8, ph + 0xf94 + 12 * i, {0}};
    }
    *parts = n;
    for (int i = 0; i < 4; i++)
        if (R32(ph + 0x1b04 + 4 * i))
            out[n++] = (Place){R32(ph + 0x1b04 + 4 * i) + 8, ph + 0x17c4 + 12 * i, {0}};
    if (R32(ph + 0x1b00))
        out[n++] = (Place){R32(ph + 0x1b00) + 8, ph + 0x17ac, {0}};
    if (R32(ph + 0x1afc))
        out[n++] = (Place){R32(ph + 0x1afc) + 8, ph + 0x17a0, {0}};
    for (uint32_t i = 0; i < R32(ph + 0x874) && n < PLACES_MAX - 1; i++)
        if (R32(ph + 0x1414 + 4 * i))
            out[n++] = (Place){R32(ph + 0x1414 + 4 * i) + 8, ph + 0x1494 + 12 * i, {0}};
    out[n++] = (Place){ph + 0x27c, ph + 0x17b8, {0}};
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++)
            out[i].pos[k] = (int32_t)R32(out[i].at + 4 * k);
    return n;
}

static void places_restore(const Place *p, int n)
{
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++)
            W32(p[i].at + 4 * k, (uint32_t)p[i].pos[k]);
}

/* a stock car (Data\Vehicles of the game folder): left as shipped */
static int stock_car(const char *name)
{
    static char names[64][16];
    static int n = -1;
    if (n < 0) {
        n = 0;
        char p[1100];
        snprintf(p, sizeof p, "%s/Data/Vehicles", game_dir);
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d)) && n < 64) {
            size_t k = strlen(e->d_name);
            if (k > 4 && k - 4 < sizeof names[0] && !strcasecmp(e->d_name + k - 4, ".idf"))
                snprintf(names[n++], sizeof names[0], "%.*s", (int)(k - 4), e->d_name);
        }
        if (d) closedir(d);
    }
    for (int i = 0; i < n; i++)
        if (!strcasecmp(names[i], name))
            return 1;
    return 0;
}

static int vehicle_place_hook(CPU *c)
{
    uint32_t obj = ARG(0), menu = ARG(1);
    F_0049b750_orig(c);
    uint32_t ph = obj ? R32(obj + 0x98) : 0;
    const char *car = obj == loaded_obj ? loaded_name : "?";
    if (!menu || !mods_active || !ph || stock_car(car))
        return 1;
    static Place was[PLACES_MAX];
    int parts, n = preview_places(ph, was, &parts);
    for (int k = 0; k < 64; k++)
        guest_call(c, 0x49ca10, 1, obj);
    /* the attachments were mended as the car loaded (attach_hook): a part still tied to point
       numbers the car doesn't have, or flung more than 50 m (points in a line), is left where
       the game put it */
    const double reach = 5000 * 65536.0;
    uint32_t npts = R32(ph + 0xc);
    double most = 0;
    int which = -1, wild = 0;
    for (int i = 0; i < n; i++) {
        double d = 0, r = 0;
        for (int k = 0; k < 3; k++) {
            double now = (int32_t)R32(was[i].at + 4 * k), e = now - was[i].pos[k];
            d += e * e;
            r += now * now;
        }
        int bad = 0;
        for (int k = 0; k < 3; k++)
            bad |= R32(was[i].points + 4 * k) >= npts;
        if (was[i].at != ph + 0x27c && (bad || sqrt(r) > reach)) {
            if (log_sizes)
                logmsg(LOG_ALWAYS, "fancars:   part %d set aside: %d %d %d (points %u %u %u of %u)", i,
                       (int32_t)R32(was[i].at), (int32_t)R32(was[i].at + 4), (int32_t)R32(was[i].at + 8),
                       R32(was[i].points), R32(was[i].points + 4), R32(was[i].points + 8), npts);
            places_restore(&was[i], 1);
            wild++;
            continue;
        }
        if (sqrt(d) > most && was[i].at != ph + 0x27c) {    /* (a point inside, not drawn) */
            most = sqrt(d);
            which = i;
        }
    }
    if (log_sizes) {
        logmsg(LOG_ALWAYS, "fancars: preview of %s: %d parts, the farthest (%d) %.2f cm from its race place, "
               "%d with no sensible place", car, parts, which, most / 65536.0, wild);
        for (int i = 0; i < n; i++)
            logmsg(LOG_ALWAYS, "fancars:   part %d: %d %d %d -> %d %d %d", i, was[i].pos[0], was[i].pos[1],
                   was[i].pos[2], (int32_t)R32(was[i].at), (int32_t)R32(was[i].at + 4),
                   (int32_t)R32(was[i].at + 8));
    }
    if (most < PREVIEW_STEP_MIN) {
        places_restore(was, n);         /* as the game left them */
        return 1;
    }
    static char told[8][32];
    for (int k = 0; k < 8; k++)
        if (!strcmp(told[k], car))
            return 1;
    static int ntold;
    snprintf(told[ntold++ % 8], sizeof told[0], "%s", car);
    logmsg(LOG_ALWAYS, "fancars: %s: its parts sit up to %.1f cm off the body in the vehicle preview: placed "
           "as in a race", car, most / 65536.0);
    return 1;
}

/* After shadows_init: its hook on the car shadow (ray-traced shadows: no painted one) runs
   inside this one. INSANE_FANCARS_LOG=1 logs every car shadow's and body mesh's size. */
void fancars_init(void)
{
    t_verts = table("model_verts");
    t_points = table("shadow_points");
    t_tris = table("shadow_tris");
    log_sizes = getenv("INSANE_FANCARS_LOG") || getenv("INSANE_BIGMODELS_LOG");
    prev_car_shadow = hook_car_shadow;
    hook_car_shadow = car_shadow_hook;
    hook_mesh_skin_nodes = mesh_skin_nodes_hook;
    hook_mesh_parts = mesh_parts_hook;
    hook_mesh_points = mesh_points_hook;
    hook_mesh_skin_points = mesh_skin_points_hook;
    hook_vhx_load = vhx_load_hook;
    hook_vehicle_load = vehicle_load_hook;
    hook_vehicle_place = vehicle_place_hook;
    hook_attach_offset = attach_hook;
    hook_vehicle_controls = vehicle_controls_hook;
}
