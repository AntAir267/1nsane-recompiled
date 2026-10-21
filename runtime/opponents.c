/* More AI opponents (Enhanced option "max_opponents = 8 | 15"): single-player races with up to
   15 AI cars and the player, the 16 the participant table (0x1c4d8d0, 16 records of 0x4d8) holds.

   The game sizes most of a race for 16 cars (network games: 8 players and 8 AI): standings,
   per-car state, replay input, gate times, vehicle slots, sounds. A single player picks at most
   8 opponents (MV_OPPONENTNUM's maximum, an int at 0x4c8f10 that ML_CHAT shares; at 15 it points
   at an int of ours), and with more cars these run out (found by racing 9 to 16 cars):
   - the link nodes that tie a car's parts together (0x4854d0: 400 of 0x78 bytes from 0x21b4778,
     ~33 a car with the scene's own; taking the 400th crashes the game): 1600 in memory of ours,
     their limit a CONSTS literal;
   - the start grid (0x4c8438: 16 spots of x, y, z in 16.16 fixed, only 13 filled: rows of 3-2-3-
     2-3, the rest at the start itself, so cars 14 to 16 land on top of each other): the pattern
     goes on for three more spots (Capture the Flag's own grid has all 16; Return the Flag puts
     the cars on a circle);
   - the cars' colours (0x4c7f38: 8, then zeros; the 9th car on gets the word after the table,
     the opponent count, an invisible colour): 8 more;
   - the caches of models (500 of 0x90 bytes), their textures (800 of 0x4c) and other textures
     (128 of 0x4c): 16 different cars fill the first two; three times as many, moved (TABLES
     models, model_textures, textures) while they are empty (0x449dc0 empties them as races and
     menus start);
   - the race HUD's opponent table (0x4621c0) has 8 rows: with more cars it shows the four leaders
     and four rows round the player (TABLES hud_racers: that function reads a compact copy of
     the chosen records);
   - the results list (row k at -0.54 + 0.12 k) runs off the screen after 10 rows: the rows close
     up to end where the 9th does, in smaller text when they must (CONSTS results_row_*);
   - the 8 drivers came round twice: 8 more (see "the AI drivers" below);
   - tied cars share a place ("7" for 7th to 16th in a Jamboree): every car shows its own
     (see "places"), and "10." to "16." in the HUD table move their dot and name clear.
   - points existed for 2 to 8 cars only (places in a bigger race scored 0): rows for 9 to 16
     cars (see "points"); championships gave every event 3 opponents: the championship setup's
     OPPONENTS line (see "championships").

   At 8 (the default) nothing changes: tables, data and the menu stay as shipped, and a race
   never gets more than 8 opponents (an edited or left-over registry value is held to 8). A
   replay of a bigger race still gets the bigger limits. Off in --test runs unless
   INSANE_ENHANCED is set. A change applies to the menu at once and to the tables when the next
   race starts. */
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "runtime.h"
#include "options.h"
#include "funcs.h"
#include "image.h"
#include "opponents.h"
#include "mods.h"
#include "widescreen.h"

/* game addresses (GOG build: DATA in builds/gog_1_0.py) */
#define PART_TABLE      GAME_PARTICIPANTS       /* 16 records of PART_SIZE */
#define PART_SIZE       0x4d8u
#define PART_COUNT      GAME_PARTICIPANT_COUNT
#define PART_ROW        0x380u                  /* place in the standings order (0x452d00) */
#define PLAYER_INDEX    GAME_HUD_PLAYER
#define MARKED_INDEX    GAME_HUD_MARKED
#define OPP_VALUE       GAME_OPPONENTS
#define OPP_ENTRY       GAME_OPPONENTS_VAR
#define OPP_MAX_ORIG    GAME_OPPONENTS_MAX
#define MODE_SETTINGS   GAME_RACE_SETTINGS      /* 4 of 0x78 bytes */
#define MODE_OPPONENTS  0x70u
#define GRID            GAME_GRID
#define COLOURS         GAME_COLOURS
#define LINK_FREE       GAME_LINK_FREE
#define LINK_ENTRY      0x78u
#define LINK_NODES      1600u
#define CACHE_TEXTURES  GAME_TEXTURE_COUNT
#define CACHE_MODELS    GAME_MODEL_COUNT
#define CACHE_MODEL_TEXTURES GAME_MODEL_TEXTURE_COUNT

#define SHIPPED_MAX     8
#define MORE_MAX        15

static int allowed;
static int oplog;               /* INSANE_OPP_LOG: log each race's set-up */
static int want = SHIPPED_MAX;  /* the opponent maximum asked for */
static int big;                 /* the bigger caches in force */
static int more;                /* the race being run has the bigger limits (pool, grid, colours, HUD) */
static uint32_t more_max;       /* program memory: the int MV_OPPONENTNUM's maximum points at */

/* ------------------------------------------------------------------ static data */
/* the grid's pattern continued (row 6: two spots, row 7: three, 600 units apart) */
static const int32_t grid_more[3][3] = {{-150, 0, 3300}, {150, 0, 3300}, {-300, 0, 3900}};
/* colours 9-16 (ARGB), told apart from the game's white, green, red, blue, orange, blue-green,
   yellow and purple */
static const uint32_t colours_more[8] = {
    0xffff6eb4, 0xff9cff3c, 0xff9a6a3a, 0xffa8a8a8, 0xff8cc8ff, 0xff00a890, 0xffb4003c, 0xffffbe8c};

static void data_apply(int on)
{
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++)
            W32(GRID + 12 * (13 + k) + 4 * j, on ? (uint32_t)(grid_more[k][j] * 65536) : 0);
    for (int k = 0; k < 8; k++)
        W32(COLOURS + 4 * (8 + k), on ? colours_more[k] : 0);
}

/* ------------------------------------------------------------------ the menu's maximum */
static void menu_apply(void)
{
    /* (a menu int's maximum is one past the most it offers: the shipped 8 offers 7 opponents) */
    if (!more_max) {
        more_max = galloc(4);
        W32(more_max, MORE_MAX + 1);
    }
    W32(OPP_ENTRY + 20, want > SHIPPED_MAX ? more_max : OPP_MAX_ORIG);
    /* back to 8: no race may start with more than the game was made for (7 opponents) */
    if (want <= SHIPPED_MAX) {
        if ((int32_t)R32(OPP_VALUE) > SHIPPED_MAX - 1)
            W32(OPP_VALUE, SHIPPED_MAX - 1);
        for (uint32_t m = 0; m < 4; m++)
            if ((int32_t)R32(MODE_SETTINGS + 0x78 * m + MODE_OPPONENTS) > SHIPPED_MAX - 1)
                W32(MODE_SETTINGS + 0x78 * m + MODE_OPPONENTS, SHIPPED_MAX - 1);
    }
}

/* ------------------------------------------------------------------ moved tables */
static const TableEntry *table(const char *name)
{
    for (const TableEntry *t = table_list; t->name; t++)
        if (!strcmp(t->name, name))
            return t;
    fatal("opponents: no table %s in this build", name);
}

static void tables_apply(int on)
{
    static const char *const names[] = {"textures", "models", "model_textures"};
    static uint32_t mem[3];
    for (int i = 0; i < 3; i++) {
        const TableEntry *t = table(names[i]);
        uint32_t n = t->count * 3;
        if (on && !mem[i] && !(mem[i] = vm_alloc_table(n * t->entry)))
            fatal("opponents: no memory for the %s table", names[i]);
        *t->reloc = on ? mem[i] : t->base;
        *t->limit = on ? n : t->count;
    }
    big = on;
}

/* The physics vehicles: 16 records of 0x1bec bytes (+0 in use) and the list of those in use
   (count at 0x21f9650). The flag modes load their flag as one, so 16 cars and the flag need 17:
   room for 24, moved while none is loaded. */
#define VEHICLE_COUNT   GAME_VEHICLE_COUNT
#define VEHICLE_ENTRY   0x1becu
#define VEHICLES_MORE   24u
static int vehicles_big;

static void vehicles_switch(int on)
{
    static uint32_t mem, list;
    const TableEntry *t = table("vehicles"), *l = table("vehicle_list");
    if (on == vehicles_big || R32(VEHICLE_COUNT))
        return;
    for (uint32_t k = 0; k < *t->limit; k++)
        if (R32(*t->reloc + k * VEHICLE_ENTRY))
            return;
    if (on && !mem) {
        if (!(mem = vm_alloc_table(VEHICLES_MORE * VEHICLE_ENTRY)) || !(list = vm_alloc_table(VEHICLES_MORE * 4)))
            fatal("opponents: no memory for the vehicles");
    }
    *t->reloc = on ? mem : t->base;
    *t->limit = on ? VEHICLES_MORE : t->count;
    *l->reloc = on ? list : l->base;
    *l->limit = *t->limit;
    rc_vehicles_end = *t->reloc + *t->limit * VEHICLE_ENTRY;
    rc_vehicles_end_field = rc_vehicles_end + 0x20;
    vehicles_big = on;
}

/* ------------------------------------------------------------------ the AI drivers */
/* data\drivers.cfg (read by 0x44ea50 at start-up and when the world size changes) gives 8 drivers,
   records of 0x344 bytes at GAME_DRIVERS (room for 32): +0 name (32 bytes, kept with its line
   end), +0x20 head, +0x40 body, +0x60 inside body, +0x80/+0xa0 hands, +0xc0/+0xe0 feet (.gem
   model lists), +0x200 build (shoulders, hips) copied to the participant, +0x300 seven models
   loaded for each race (0x44f3f0, given back by 0x44f520), +0x33c/+0x340 arm and leg length.
   Names, looks and builds only: the AI's skill is the race's difficulty, and no driver has a
   voice. 0x45e500 deals the AI their drivers from a shuffle of all of them (participant i gets
   shuffled[i % count]), so 15 opponents met the 8 names twice. At 15 eight more follow, each
   with the look and build of the original in the same place (Carlo Bob's, Rose Frank's, Nate
   Joe's, Zoe Luke's, Jon Max's, Marty Nick's, Tommy Liz's, Emma Sue's); the player's CHANGE
   MODEL keeps to the original 8 (the same looks), so the player file never names a new one; a
   replay that does (bytes of the driver numbers) gets them back at 8 too. */
#define DRIVERS         GAME_DRIVERS
#define DRIVER_COUNT    GAME_DRIVER_COUNT
#define DRIVER_SIZE     0x344u
#define DRIVER_MODELS   0x300u          /* 7 model pointers */
#define PART_DRIVER     0x5cu           /* a participant's driver */
#define SHIPPED_DRIVERS 8u
static const char *const more_drivers[SHIPPED_DRIVERS] = {
    "Carlo", "Rose", "Nate", "Zoe", "Jon", "Marty", "Tommy", "Emma"};
static int roster_big;      /* the 8 more drivers are in the table */

static void roster_apply(int on)
{
    if (on && !roster_big && R32(DRIVER_COUNT) == SHIPPED_DRIVERS) {
        for (uint32_t k = 0; k < SHIPPED_DRIVERS; k++) {
            uint32_t src = DRIVERS + DRIVER_SIZE * k, dst = DRIVERS + DRIVER_SIZE * (SHIPPED_DRIVERS + k);
            memcpy(gptr(dst), gptr(src), DRIVER_SIZE);
            memset(gptr(dst), 0, 32);
            snprintf(gptr(dst), 32, "%s\r\n", more_drivers[k]);  /* as the file's names are read */
            memset(gptr(dst + DRIVER_MODELS), 0, 7 * 4);       /* loaded with the others' */
        }
        W32(DRIVER_COUNT, 2 * SHIPPED_DRIVERS);
        roster_big = 1;
    } else if (!on && roster_big) {
        memset(gptr(DRIVERS + DRIVER_SIZE * SHIPPED_DRIVERS), 0, DRIVER_SIZE * SHIPPED_DRIVERS);
        if (R32(DRIVER_COUNT) == 2 * SHIPPED_DRIVERS)
            W32(DRIVER_COUNT, SHIPPED_DRIVERS);
        roster_big = 0;
    }
}

/* the race being set up has a driver past the 8 (a replay recorded at 15) */
static int roster_used(void)
{
    uint32_t n = R32(PART_COUNT);
    for (uint32_t i = 0; i < n && i < 16; i++)
        if (R32(PART_TABLE + PART_SIZE * i + PART_DRIVER) >= SHIPPED_DRIVERS)
            return 1;
    return 0;
}

/* 0x44ea50(file): reads the drivers (count back to 0, then the file's) */
static int drivers_load_hook(CPU *c)
{
    F_0044ea50_orig(c);
    if (roster_big) {                   /* ours are still behind the file's: start again */
        memset(gptr(DRIVERS + DRIVER_SIZE * SHIPPED_DRIVERS), 0, DRIVER_SIZE * SHIPPED_DRIVERS);
        roster_big = 0;
    }
    roster_apply(want > SHIPPED_MAX || roster_used());
    return 1;
}

/* 0x47a200 / 0x47a250: the player setup's CHANGE MODEL steps through the drivers' looks */
static int model_next_hook(CPU *c)
{
    if (!roster_big)
        return 0;
    W32(DRIVER_COUNT, SHIPPED_DRIVERS);
    F_0047a200_orig(c);
    W32(DRIVER_COUNT, 2 * SHIPPED_DRIVERS);
    return 1;
}

static int model_prev_hook(CPU *c)
{
    if (!roster_big)
        return 0;
    W32(DRIVER_COUNT, SHIPPED_DRIVERS);
    F_0047a250_orig(c);
    W32(DRIVER_COUNT, 2 * SHIPPED_DRIVERS);
    return 1;
}

static void points_apply(int on);
static void grid_log(void);

/* the bigger limits are needed: asked for, or a replay of a race with more than 9 cars */
static int needed(void)
{
    return want > SHIPPED_MAX || (int32_t)R32(PART_COUNT) > SHIPPED_MAX + 1;
}

/* the caches move only while they are empty */
static void caches_switch(int on)
{
    if (on != big && !R32(CACHE_TEXTURES) && !R32(CACHE_MODELS) && !R32(CACHE_MODEL_TEXTURES))
        tables_apply(on);
}

/* 0x449dc0: empties the model and texture caches (as races and menus start): a good time to
   move them */
static int caches_empty_hook(CPU *c)
{
    F_00449dc0_orig(c);
    caches_switch(needed());
    return 1;
}

/* 0x45e500(n, kind): picks the race's n AI opponents (kind: same car, class, free) into the
   participant table after the player; no more than the maximum in force (a registry value from a
   session at 15, or edited) */
static int opponents_pick_hook(CPU *c)
{
    int32_t n = (int32_t)R32(c->esp + 4), max = want > SHIPPED_MAX ? MORE_MAX : SHIPPED_MAX - 1;
    if (n > max)
        W32(c->esp + 4, (uint32_t)max);
    roster_apply(want > SHIPPED_MAX);   /* the drivers dealt out */
    if (classes_pick_prepare(c)) {      /* CAR CLASSES REORGANIZED: "class" and "free" from all */
        F_0045e500_orig(c);
        classes_pick_done(c);
        return 1;
    }
    return 0;
}

/* 0x484dd0: a race's scene starts (the participants are known): clears its pools and builds the
   link nodes' free list (400 at 0x21b4778, handed out by 0x4854d0 up to rc_link_limit) */
static int link_pool_hook(CPU *c)
{
    static uint32_t mem;
    F_00484dd0_orig(c);
    more = needed();
    caches_switch(more);
    vehicles_switch(more);
    data_apply(more);
    roster_apply(want > SHIPPED_MAX || roster_used());
    points_apply(want > SHIPPED_MAX);
    rc_link_limit = more ? LINK_NODES : 400;
    if (oplog)
        LOG(LOG_ALWAYS, "opponents: a race with %d cars starts (frame %lu): bigger limits %s, caches %s, vehicles %s",
            (int)R32(PART_COUNT), frame_count, more ? "on" : "off", big ? "moved" : "as shipped",
            vehicles_big ? "moved" : "as shipped");
    if (oplog && R32(PART_COUNT))
        grid_log();
    if (!more)
        return 1;
    if (!mem && !(mem = vm_alloc_table(LINK_NODES * LINK_ENTRY)))
        fatal("opponents: no memory for the link nodes");
    memset(gptr(mem), 0, LINK_NODES * LINK_ENTRY);
    for (uint32_t k = 0; k < LINK_NODES; k++) {
        uint32_t n = mem + k * LINK_ENTRY;
        W32(n + 0x70, k ? n - LINK_ENTRY : 0);
        W32(n + 0x74, k + 1 < LINK_NODES ? n + LINK_ENTRY : 0);
    }
    W32(LINK_FREE, mem);
    return 1;
}

/* ------------------------------------------------------------------ places */
/* 0x452d00 numbers the standings order (GAME_STANDINGS, sorted by each mode's score) but gives
   cars with the same score the same place (+0x378) and marks them tied (+0x37c): in a Jamboree
   every car without a gate showed the place after the last car with one ("7" from 7th to 16th).
   With the bigger limits every car shows its own place, in the order the lists already use;
   the places kept (the player's goes to the championship) and the points stay as they were:
   the HUD and the results lists are drawn with the places swapped in, then put back. */
#define PART_PLACE      0x378u
#define PART_TIED       0x37cu
#define STANDINGS       GAME_STANDINGS          /* n x {score, participant} */

typedef struct { uint32_t n; uint32_t place[16], tied[16]; } Places;

static void places_save(Places *s)
{
    s->n = R32(PART_COUNT);
    if (s->n > 16) s->n = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        s->place[i] = R32(PART_TABLE + PART_SIZE * i + PART_PLACE);
        s->tied[i] = R32(PART_TABLE + PART_SIZE * i + PART_TIED);
    }
}

static void places_restore(const Places *s)
{
    for (uint32_t i = 0; i < s->n; i++) {
        W32(PART_TABLE + PART_SIZE * i + PART_PLACE, s->place[i]);
        W32(PART_TABLE + PART_SIZE * i + PART_TIED, s->tied[i]);
    }
}

/* 0x463bd0: the race HUD (the big place top left with "=" when tied, the opponent table):
   chained after widescreen.c's hook, before prompts.c's */
static int (*prev_race_hud)(CPU *c);
static unsigned long win_at;    /* INSANE_OPP_WIN=<frame>, tests: the player wins then */

/* What the game's Ctrl+W cheat does (0x45f66a; championship races only): the player has finished
   with the winning score, the race is over. Lets a test reach the results of a race the
   scripted player cannot finish. */
static void player_wins(CPU *c)
{
    int32_t p = (int32_t)R32(PLAYER_INDEX);
    if (p < 0 || p >= 16)
        return;
    uint32_t rec = PART_TABLE + PART_SIZE * (uint32_t)p;
    W32(rec + 0x384, 1);
    W32(rec + 0x398, R32(GAME_RACE_CLOCK));
    W32(rec + 0x388, R32(GAME_RACE_TARGET));
    W32(rec + 0x154, R32(GAME_RACE_TARGET));
    W32(rec + 0x158, R32(GAME_RACE_GATES));
    W32(GAME_RACE_STATE, 3);
    uint32_t over = R32(R32(GAME_RACE_RULES) + 0x14);
    if (over)
        guest_call(c, over, 0);
    LOG(LOG_ALWAYS, "opponents: INSANE_OPP_WIN: the player wins (frame %lu)", frame_count);
}

static int race_hud_hook(CPU *c)
{
    if (win_at && frame_count >= win_at && R32(GAME_RACE_STATE) == 2) {
        win_at = 0;
        player_wins(c);
    }
    static uint32_t state_was;
    if (oplog && R32(GAME_RACE_STATE) != state_was) {
        state_was = R32(GAME_RACE_STATE);
        if (state_was == 3)
            LOG(LOG_ALWAYS, "opponents: the race is over (frame %lu)", frame_count);
    }
    Places s;
    places_save(&s);
    if (more)
        for (uint32_t i = 0; i < s.n; i++) {
            uint32_t r = R32(PART_TABLE + PART_SIZE * i + PART_ROW);
            if (r < s.n) {
                W32(PART_TABLE + PART_SIZE * i + PART_PLACE, r);
                W32(PART_TABLE + PART_SIZE * i + PART_TIED, 0);
            }
        }
    if (!prev_race_hud || !prev_race_hud(c))
        F_00463bd0_orig(c);
    if (more)
        places_restore(&s);
    return 1;
}

/* ------------------------------------------------------------------ the HUD's opponent table */
#define HUD_ROWS 8

/* Each row is "N." at x - 0.07, a colour dot from x - 0.04, the name at x (x = the column, 0.07
   or 0.37 from the table's left), the score right-aligned: "10." to "16." ran into the dot. On
   those rows the dot (and the highlight over it) and the name move right by what the second
   digit adds to "8.", in the font as drawn (0x41bbe0 and 0x481380 are watched while the table
   is drawn); the other rows are as shipped. */
static struct {
    int on;
    float left, top;
    char text[HUD_ROWS][8];             /* the place shown in each slot (column * 4 + row) */
    float shift[HUD_ROWS];              /* < 0: not measured yet */
} tbl;

static float text_width(const char *s)
{
    uint32_t glyphs = R32(GAME_FONT_GLYPHS);
    float size = RF32(GAME_FONT_SIZE), w = 0;
    if (!glyphs || size == 0)
        return 0;
    for (; *s; s++) {
        int32_t g = (int32_t)R32(glyphs + (uint8_t)*s * 0x14u + 8);
        if (g > 0) w += (float)g;
    }
    return w * (RF32(GAME_FONT_WIDTH) / size * 1.5258789e-05f) * RF32(GAME_TEXT_UNIT);
}

/* the slot an element at (x, y) belongs to, if it sits where a dot (dx -0.04) or a name (dx 0) does */
static int tbl_slot(float x, float y, float dx)
{
    int col = x >= tbl.left + 0.3f;
    float colx = tbl.left + (col ? 0.37f : 0.07f);
    if (fabsf(x - (colx + dx)) > 0.004f)
        return -1;
    float r = (y - tbl.top - 0.01f) / 0.07f;
    int row = (int)lroundf(r);
    if (row < 0 || row > 3 || fabsf(r - (float)row) > 0.1f)
        return -1;
    int k = col * 4 + row;
    if (!tbl.text[k][0] || !tbl.text[k][1] || tbl.text[k][1] == '.')
        return -1;                      /* empty or one digit: as shipped */
    if (tbl.shift[k] < 0) {
        float d = text_width(tbl.text[k]) - text_width("8.");
        tbl.shift[k] = d > 0 ? d : 0;
    }
    return k;
}

/* 0x41bbe0(x0, y0, x1, y1, u0, v0, u1, v1, texture, colour): a flat quad */
static int flat_quad_hook(CPU *c)
{
    if (!tbl.on)
        return 0;
    float x0 = RF32(c->esp + 4), y0 = RF32(c->esp + 8);
    int k = tbl_slot(x0, y0, -0.04f);
    if (k >= 0) {
        WF32(c->esp + 4, x0 + tbl.shift[k]);
        WF32(c->esp + 12, RF32(c->esp + 12) + tbl.shift[k]);
    }
    return 0;
}

/* 0x481380(x, y, text): a line of text */
static int text_draw_hook(CPU *c)
{
    classes_browse_sync();              /* Vehicle > Browse's position follows the chosen car */
    if (!tbl.on)
        return 0;
    float x = RF32(c->esp + 4), y = RF32(c->esp + 8);
    int k = tbl_slot(x, y, 0);
    if (k >= 0)
        WF32(c->esp + 4, x + tbl.shift[k]);
    return 0;
}

/* 0x4621c0: the opponent table, one row per participant at its place in the standings (+0x380:
   rows 0-3 in the left column, 4-7 in the right). With more than 8 it is handed a copy of 8
   records: the four leaders, then four rows round the player. */
static int hud_table_hook(CPU *c)
{
    static uint32_t copy;
    uint32_t n = R32(PART_COUNT);
    if (!more || n <= HUD_ROWS || n > 16)
        return 0;
    int by_row[16], player = (int32_t)R32(PLAYER_INDEX), p = -1;
    for (uint32_t i = 0; i < n; i++)
        by_row[i] = -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = R32(PART_TABLE + PART_SIZE * i + PART_ROW);
        if (r < n) by_row[r] = (int)i;
    }
    for (uint32_t r = 0; r < n; r++)
        if (by_row[r] < 0)
            return 0;                   /* not a full order yet */
    if (player >= 0 && player < (int)n)
        p = (int)R32(PART_TABLE + PART_SIZE * (uint32_t)player + PART_ROW);
    /* rows shown: 0-3, then 4-7 or the player's neighbours (one ahead, two behind) */
    int rows[HUD_ROWS], start = 4;
    if (p > 7) {
        start = p - 1;
        if (start > (int)n - 4) start = (int)n - 4;
    }
    for (int k = 0; k < 4; k++) {
        rows[k] = k;
        rows[4 + k] = start + k;
    }
    if (!copy)
        copy = galloc(HUD_ROWS * PART_SIZE);
    uint32_t player_new = 0xffffffffu, marked = R32(MARKED_INDEX), marked_new = 0xffffffffu;
    for (int k = 0; k < HUD_ROWS; k++) {
        int i = by_row[rows[k]];
        uint32_t rec = copy + PART_SIZE * (uint32_t)k;
        memcpy(gptr(rec), gptr(PART_TABLE + PART_SIZE * (uint32_t)i), PART_SIZE);
        W32(rec + PART_ROW, (uint32_t)k);
        if (i == player) player_new = (uint32_t)k;
        if ((uint32_t)i == marked) marked_new = (uint32_t)k;
        snprintf(tbl.text[k], sizeof tbl.text[k], "%d.", (int)R32(rec + PART_PLACE) + 1);
        tbl.shift[k] = -1;
    }
    tbl.left = RF32(GAME_HUD_LEFT);
    tbl.top = RF32(GAME_HUD_TOP);
    const TableEntry *t = table("hud_racers");
    *t->reloc = copy;
    W32(PART_COUNT, HUD_ROWS);
    W32(PLAYER_INDEX, player_new);
    W32(MARKED_INDEX, marked_new);
    tbl.on = 1;
    F_004621c0_orig(c);
    tbl.on = 0;
    W32(MARKED_INDEX, marked);
    W32(PLAYER_INDEX, (uint32_t)player);
    W32(PART_COUNT, n);
    *t->reloc = t->base;
    return 1;
}

/* ------------------------------------------------------------------ the results screen */
/* The results list (0x456da0 laps, 0x4585f0 Pathfinder, 0x459de0 the other modes) puts row k at
   y = 0x4c8360 + k * 0x4c8364 (-0.54 + k * 0.12) in text of 1.4 x 1.5: with more than 9 rows
   they run under the menu and off the screen. They close up to end where the 9th row does, and
   their text shrinks once they are closer than the HUD's opponent table (0.07 apart, 1.4 high). */
#define RESULTS_STEP    GAME_RESULTS_STEP
#define SHIPPED_ROWS    9

/* every row its own place (the standings order), its points looked up as before */
static void results_places(Places *s, uint32_t row_addr, uint32_t *points)
{
    for (uint32_t k = 0; k < 16; k++)
        points[k] = R32(row_addr + 4 * k);
    for (uint32_t k = 0; k < s->n; k++) {
        uint32_t i = R32(STANDINGS + 8 * k + 4);
        if (i >= s->n || s->place[i] >= 16)
            continue;
        W32(PART_TABLE + PART_SIZE * i + PART_PLACE, k);
        W32(row_addr + 4 * k, points[s->place[i]]);
    }
}

/* The list's glyphs go into the font batch, drawn when it fills (mid-list with 16 rows) or when
   the frame's 2D ends; in widescreen a batch drawn outside the menus is placed like a name label
   by its middle, so the end of a long list that came out in a batch of its own moved right. The
   list is drawn as a menu (its 4:3 layout, as the game's own results screen) and the batch
   emptied before it ends. */
#define F_GLYPHS_DRAW   0x42a020u       /* (buffer, 5): draws a batch's queued quads */
static void list_draw(CPU *c, void (*orig)(CPU *))
{
    ws_menu_depth++;
    orig(c);
    guest_call(c, F_GLYPHS_DRAW, 2, 0, 5);
    guest_call(c, F_GLYPHS_DRAW, 2, 1, 5);
    ws_menu_depth--;
}

static void results_call(CPU *c, void (*orig)(CPU *))
{
    uint32_t n = R32(PART_COUNT);
    if (!more || n > 16) {
        orig(c);
        return;
    }
    Places s;
    uint32_t mode = R32(GAME_GAME_MODE), points[16];
    uint32_t row_addr = GAME_POINTS + 4 * 16 * (16 * (mode & 7) + n);
    places_save(&s);
    results_places(&s, row_addr, points);
    if (n <= SHIPPED_ROWS) {
        list_draw(c, orig);
        places_restore(&s);
        for (uint32_t k = 0; k < 16; k++)
            W32(row_addr + 4 * k, points[k]);
        return;
    }
    float step0 = RF32(RESULTS_STEP), step = step0 * (SHIPPED_ROWS - 1) / (float)(n - 1);
    float h = step * 1.4f / 0.07f, w;
    if (h > 1.5f) h = 1.5f;
    w = 1.4f * h / 1.5f;
    uint32_t w0 = rc_results_row_width, h0 = rc_results_row_height;
    memcpy(&rc_results_row_width, &w, 4);
    memcpy(&rc_results_row_height, &h, 4);
    WF32(RESULTS_STEP, step);
    list_draw(c, orig);
    WF32(RESULTS_STEP, step0);
    rc_results_row_width = w0;
    rc_results_row_height = h0;
    places_restore(&s);
    for (uint32_t k = 0; k < 16; k++)
        W32(row_addr + 4 * k, points[k]);
}

static int results_race_hook(CPU *c) { results_call(c, F_00456da0_orig); return 1; }
static int results_path_hook(CPU *c) { results_call(c, F_004585f0_orig); return 1; }
static int results_gates_hook(CPU *c) { results_call(c, F_00459de0_orig); return 1; }

/* ------------------------------------------------------------------ points */
/* data\scores.cfg gives each game mode a points row for 1 to 8 cars (GAME_POINTS[mode * 16 +
   cars][place], 16 ints a row; the same rows for every mode): 2 cars 10 0, ..., 8 cars 10 8 6 4
   3 2 1 0, so with 9 to 16 cars every place scored 0. With the bigger limits the rows for 9 to
   16 cars follow the same shape: 10 for a win (a championship's maximum and targets assume 10 an
   event), the last place 0, every other place at least 1, and no place worth less than with one
   car fewer. Ties keep the game's rule (tied cars get the points of their shared place). */
static const uint8_t more_points[8][16] = {
    {10, 8, 6, 5, 4, 3, 2, 1, 0},                           /*  9 cars */
    {10, 8, 6, 5, 4, 3, 2, 1, 1, 0},                        /* 10 */
    {10, 8, 6, 5, 4, 3, 2, 2, 1, 1, 0},                     /* 11 */
    {10, 8, 6, 5, 4, 3, 3, 2, 2, 1, 1, 0},                  /* 12 */
    {10, 8, 6, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0},               /* 13 */
    {10, 8, 7, 6, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0},            /* 14 */
    {10, 8, 7, 6, 5, 4, 4, 3, 3, 2, 2, 1, 1, 1, 0},         /* 15 */
    {10, 8, 7, 6, 5, 4, 4, 3, 3, 2, 2, 2, 1, 1, 1, 0}};     /* 16 */

static void points_apply(int on)
{
    for (uint32_t mode = 0; mode < 8; mode++)
        for (uint32_t n = 9; n <= 16; n++)
            for (uint32_t k = 0; k < 16; k++)
                W32(GAME_POINTS + 4 * (16 * (16 * mode + n) + k), on ? more_points[n - 9][k] : 0);
}

/* ------------------------------------------------------------------ championships */
/* data\champdef.cfg gives each championship's events (game mode column x scene) their number of
   opponents, all 3 (GAME_CHAMP_OPPONENTS: [championship 0x7f0 bytes][column 8][scene 8] ints).
   0x47b020 sets an event up from them (opponents, length, location, difficulty) and 0x478310
   scores it from the same number (the points row for that many cars, the player's kept place).
   With the bigger limits every event gets the championship setup's OPPONENTS (3 to 15, at least
   the event's own), written into those entries as an event is set up; at 8 they are as read. The
   championship's points, records (Player00.chp) and unlocks are unchanged otherwise: a win is
   still worth 10, so targets and maximum scores mean the same. */
#define CHAMP_OPPONENTS GAME_CHAMP_OPPONENTS
#define CHAMPS          5u
static int32_t champ_read[CHAMPS][64];  /* the file's numbers */
static int champ_saved;
static uint32_t champ_value;            /* program memory: the setup's OPPONENTS (an int the menu steps) */
static uint32_t champ_on;               /* program memory: 1 while the line can be changed */
static int champ_last;                  /* the value last saved to options.txt */

static void champs_apply(void)
{
    if (!champ_saved) {
        for (uint32_t c = 0; c < CHAMPS; c++)
            for (uint32_t k = 0; k < 64; k++)
                champ_read[c][k] = (int32_t)R32(CHAMP_OPPONENTS + 0x7f0 * c + 4 * k);
        champ_saved = 1;
    }
    int32_t v = champ_value ? (int32_t)R32(champ_value) : MORE_MAX;
    if (v < 1 || v > MORE_MAX) v = MORE_MAX;
    if (champ_value && v != champ_last) {          /* keep the setup's choice */
        char s[8];
        snprintf(s, sizeof s, "%d", v);
        option_set("championship_opponents", s);
        options_save();
        champ_last = v;
    }
    for (uint32_t c = 0; c < CHAMPS; c++)
        for (uint32_t k = 0; k < 64; k++) {
            int32_t n = champ_read[c][k];
            if (want > SHIPPED_MAX && n > 0 && v > n)
                n = v;
            W32(CHAMP_OPPONENTS + 0x7f0 * c + 4 * k, (uint32_t)n);
        }
}

/* 0x47b020(menu, column, scene): a championship event is chosen and set up */
static int champ_event_hook(CPU *c)
{
    champs_apply();
    points_apply(want > SHIPPED_MAX);
    return 0;
}

/* ------------------------------------------------------------------ stock cars only */
/* With fan content the car list (TABLES cars, 64 bytes an entry: file name at +0x20, class letter
   +0x30, paint jobs +0x38, locked +0x3c, not available +0x3d) holds the game's own cars and the fan
   cars, sorted by class. "stock_cars_only = on" leaves only the game's own (the .idf files in the
   game's Data\Vehicles; a fan car of the same name is never listed): the fan cars are marked
   locked and not available, which every step of the car pickers passes over (each by its own
   rule; picker.c's jumps follow the same rules), and which the opponents' choice (0x45e500:
   "class" and "free" take unlocked, available cars) and the attract mode's (0x478750: available
   cars of a class whose paint jobs, GAME_CLASSES + 0x88, are enough) leave out. "Locked" is worked
   out again by 0x46e690 (start-up, unlock codes, after a championship race), so the marks are made
   after it; "not available" is set once at start-up, so it is put back as it was when the option
   goes off, and so are the classes' paint-job totals. A class with fewer stock cars than
   opponents repeats them, as the game does when it runs short. Turning the option on with a fan
   car chosen takes the first stock car of that class (else the first stock car), as the game's
   own step would (name, paint job 0, the preview). Only with fan content; off by default. */
#define CAR_SIZE        0x40u
#define CAR_FILE        0x20u
#define CAR_CLASS       0x30u
#define CAR_PAINTS      0x38u
#define CAR_LOCKED      0x3cu
#define CAR_UNAVAILABLE 0x3du
#define CLASS_SIZE      0x8cu
#define CLASS_PAINTS    0x88u
#define F_CAR_CHANGED   0x46ef70u       /* the menus' vehicle preview follows the chosen car */
#define F_UNLOCKS       0x46e690u       /* works out what is locked */
static int stock_want;                  /* the option */
static int stock_marked;                /* the marks are in the car list */
static uint8_t *stock_unavail;          /* "not available" as start-up set it */
static int32_t stock_totals[128];      /* a class's paint jobs (CAR CLASSES REORGANIZED has 19) */
static char stock_files[64][32];
static int nstock_files = -1;

static int stock_on(void) { return allowed && mods_active && stock_want; }
static int stock_car(uint32_t i);
static int stock_car_shown(uint32_t i) { return stock_car(i); }

static int stock_car(uint32_t i)
{
    if (nstock_files < 0) {
        nstock_files = 0;
        char p[1400];
        snprintf(p, sizeof p, "%s/Data/Vehicles", game_dir);
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d)) && nstock_files < 64) {
            size_t k = strlen(e->d_name);
            if (k > 4 && k - 4 < 32 && !strcasecmp(e->d_name + k - 4, ".idf"))
                snprintf(stock_files[nstock_files++], 32, "%.*s", (int)(k - 4), e->d_name);
        }
        if (d) closedir(d);
    }
    const char *f = (const char *)gptr(reloc_cars + CAR_SIZE * i + CAR_FILE);
    for (int k = 0; k < nstock_files; k++)
        if (!strcasecmp(f, stock_files[k]))
            return 1;
    return 0;
}

static void stock_apply(void)
{
    uint32_t n = R32(GAME_CAR_COUNT), classes = R32(GAME_CLASS_COUNT);
    if (classes > 128) classes = 128;
    if (stock_on()) {
        if (!stock_marked) {
            free(stock_unavail);
            stock_unavail = malloc(n ? n : 1);
            for (uint32_t i = 0; i < n; i++)
                stock_unavail[i] = (uint8_t)R8(reloc_cars + CAR_SIZE * i + CAR_UNAVAILABLE);
            for (uint32_t c = 0; c < classes; c++)
                stock_totals[c] = (int32_t)R32(GAME_CLASSES + CLASS_SIZE * c + CLASS_PAINTS);
        }
        for (uint32_t i = 0; i < n; i++)
            if (!stock_car(i)) {
                W8(reloc_cars + CAR_SIZE * i + CAR_LOCKED, 1);
                W8(reloc_cars + CAR_SIZE * i + CAR_UNAVAILABLE, 1);
            }
        for (uint32_t c = 0; c < classes; c++) {
            uint8_t letter = (uint8_t)R8(GAME_CLASSES + CLASS_SIZE * c);
            int32_t total = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t e = reloc_cars + CAR_SIZE * i;
                if (R8(e + CAR_CLASS) == letter && !R8(e + CAR_UNAVAILABLE))
                    total += (int32_t)R32(e + CAR_PAINTS);
            }
            W32(GAME_CLASSES + CLASS_SIZE * c + CLASS_PAINTS, (uint32_t)total);
        }
        stock_marked = 1;
        classes_browse_filter(stock_car_shown);     /* Browse lists the stock cars alone */
    } else if (stock_marked) {
        for (uint32_t i = 0; i < n; i++)
            W8(reloc_cars + CAR_SIZE * i + CAR_UNAVAILABLE, stock_unavail[i]);
        for (uint32_t c = 0; c < classes; c++)
            W32(GAME_CLASSES + CLASS_SIZE * c + CLASS_PAINTS, (uint32_t)stock_totals[c]);
        stock_marked = 0;               /* "locked" was just worked out again (0x46e690) */
        classes_browse_filter(NULL);
    }
}

/* the first stock car of class `cls` a step could land on, else the first such stock car */
static int stock_first(uint8_t cls)
{
    uint32_t n = R32(GAME_CAR_COUNT);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t k = 0; k < n; k++) {
            uint32_t e = reloc_cars + CAR_SIZE * k;
            if (stock_car(k) && !R8(e + CAR_LOCKED) && !R8(e + CAR_UNAVAILABLE) && (pass || R8(e + CAR_CLASS) == cls))
                return (int)k;
        }
    return -1;
}

/* 0x44ca10(settings): a game type's settings (car file name at +8, paint job at +4) are taken
   up; with stock cars only a fan car there becomes the first stock car of its class (the game
   would take the next car it could choose, in list order) */
static int settings_load_hook(CPU *c)
{
    uint32_t set = R32(c->esp + 4);
    if (!stock_on() || !set)
        return 0;
    const char *want_car = (const char *)gptr(set + 8);
    uint32_t n = R32(GAME_CAR_COUNT), i;
    for (i = 0; i < n; i++)
        if (!strcasecmp((const char *)gptr(reloc_cars + CAR_SIZE * i + CAR_FILE), want_car))
            break;
    if (i >= n || stock_car(i))
        return 0;
    int pick = stock_first(R8(reloc_cars + CAR_SIZE * i + CAR_CLASS));
    if (pick < 0)
        return 0;
    const char *f = (const char *)gptr(reloc_cars + CAR_SIZE * (uint32_t)pick + CAR_FILE);
    if (oplog)
        LOG(LOG_ALWAYS, "opponents: stock cars only: the saved car %s is now %s", want_car, f);
    gstrncpy(set + 8, f, 16);
    W32(set + 4, 0);
    return 0;
}

/* 0x46e690: works out what is locked */
static int unlocks_hook(CPU *c)
{
    classes_unlock_begin();             /* CAR CLASSES REORGANIZED: the bonus cars won as before */
    F_0046e690_orig(c);
    classes_unlock();                   /* ... and the race classes' cars */
    stock_apply();
    return 1;
}

/* a fan car chosen: the first stock car of its class that a step could land on */
static void player_car_stock(CPU *c)
{
    uint32_t n = R32(GAME_CAR_COUNT), i = R32(GAME_CAR_CHOSEN);
    if (i < n && stock_car(i))
        return;
    int pick = stock_first(i < n ? (uint8_t)R8(reloc_cars + CAR_SIZE * i + CAR_CLASS) : 0);
    if (pick < 0)
        return;
    const char *f = (const char *)gptr(reloc_cars + CAR_SIZE * (uint32_t)pick + CAR_FILE);
    W32(GAME_CAR_CHOSEN, (uint32_t)pick);
    memcpy(gptr(GAME_CAR_NAME), f, strlen(f) + 1);
    W32(GAME_CAR_PAINT, 0);
    guest_call(c, F_CAR_CHANGED, 0);
    if (oplog)
        LOG(LOG_ALWAYS, "opponents: stock cars only: the player's car is now %s", f);
}

static void stock_set(CPU *c, int on)
{
    stock_want = on;
    option_set("stock_cars_only", on ? "on" : "off");
    options_save();
    guest_call(c, F_UNLOCKS, 0);        /* the marks follow (unlocks_hook) */
    if (stock_on())
        player_car_stock(c);
    if (oplog)
        LOG(LOG_ALWAYS, "opponents: stock cars only %s", on ? "on" : "off");
}

/* MV_STOCK_ONLY: a text and Left/Right */
static uint32_t stock_text;
METHOD(stock_get, 0)
{
    gstrncpy(stock_text, stock_want ? "Stock cars only" : "Stock and fan cars", 32);
    return stock_text;
}
METHOD(stock_next, 0) { stock_set(c, !stock_want); return 0; }
METHOD(stock_prev, 0) { stock_set(c, !stock_want); return 0; }

/* the race grid, for INSANE_OPP_LOG */
static void grid_log(void)
{
    uint32_t n = R32(PART_COUNT), fan = 0;
    char line[1024] = "";
    for (uint32_t i = 0; i < n && i < 16; i++) {
        uint32_t car = R32(PART_TABLE + PART_SIZE * i + 0x54);
        int stock = car < R32(GAME_CAR_COUNT) && stock_car(car);
        fan += !stock;
        size_t k = strlen(line);
        snprintf(line + k, sizeof line - k, "%s%s%s", i ? " " : "", (const char *)gptr(PART_TABLE + PART_SIZE * i + 0x60),
                 stock ? "" : "(fan)");
    }
    LOG(LOG_ALWAYS, "opponents: grid: %s; %u fan car(s)", line, fan);
}

/* ------------------------------------------------------------------ the setup screens */
/* Lines added to the menu script as it is read (after the Enhanced options' own patches, before
   the button prompts'):
   - with the bigger limits, the championship event screen (START, VEHICLE, PAINT JOB, SETUP,
     BACK) an OPPONENTS line (MV_OPP_CHAMP: the game's own int stepping, 3 to 15) and under the
     car "N opponent(s)" (as the race setup's line reads), greyed out if the option goes back to
     8 later in the session;
   - with fan content, the championship event screen and Quick Race a STOCK CARS line
     (MV_STOCK_ONLY), its setting with the others ("Stock cars only" / "Stock and fan cars").
   Practice has no opponents and no line; the choice applies there too. */
static int (*prev_open)(CPU *c), (*prev_vars)(CPU *c);
static uint32_t menu_names[4];          /* "cache\opponents\menu-N.cfg" per variant, once written */
static int menu_tried[4];
static uint32_t vars_first, vars_last;

static const char *line_after(const char *text, const char *block, const char *what)
{
    const char *b = strstr(text, block);
    if (!b) return NULL;
    const char *end = strstr(b + 1, "\nblock ");        /* the next top-level block */
    const char *w = strstr(b, what);
    if (!w || (end && w > end)) return NULL;
    const char *nl = strchr(w, '\n');
    return nl ? nl + 1 : NULL;
}

/* the height in a block's first "size 200 H" line */
static const char *rect_height(const char *text, const char *block)
{
    const char *b = strstr(text, block);
    if (!b) return NULL;
    const char *end = strstr(b + 1, "\nblock menu");
    const char *s = strstr(b, "size 200 ");
    if (!s || (end && s > end)) return NULL;
    return s + 9;
}

/* `what` inside the top-level block starting with `block` */
static const char *in_block(const char *text, const char *block, const char *what)
{
    const char *b = strstr(text, block);
    if (!b) return NULL;
    const char *end = strstr(b + 1, "\nblock ");
    const char *w = strstr(b, what);
    return w && (!end || w < end) ? w : NULL;
}

typedef struct { const char *at, *text; int replace; } Ins;

static int ins_cmp(const void *a, const void *b)
{
    const Ins *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}

static char *menu_patch(const char *in, int opp, int stock)
{
    Ins e[12];
    int k = 0;
    const char *c1 = line_after(in, "block menu championsetmenu", "mp \" SETUP\"");
    const char *c2 = line_after(in, "block text championshiptextxxxx", "\"$MV_SETUPTYPE:carsetup setup\"");
    if (!c1 || !c2)
        return NULL;
    if (opp) {
        e[k++] = (Ins){c1, "\tmp \"OPPONENTS\" /select MV_OPP_CHAMP /enabled MV_OPP_CHAMP_ON\r\n", 0};
        e[k++] = (Ins){c2, "\t\ttext \"$MV_OPP_CHAMP opponent(s)\"\r\n", 0};
    }
    if (stock) {
        const char *q1 = line_after(in, "block menu quickracemenu", "mp \"OPPONENT NUMBER\"");
        const char *q2 = line_after(in, "block menu quickracemenu", "opponenttype opponent(s)\"");
        const char *q3 = rect_height(in, "block menu quickracemenu");
        if (!q1 || !q2 || !q3 || strncmp(q3, "125", 3))
            return NULL;
        e[k++] = (Ins){c1, "\tmp \"STOCK CARS\" /select MV_STOCK_ONLY\r\n", 0};
        e[k++] = (Ins){c2, "\t\ttext \"$MV_STOCK_ONLY\"\r\n", 0};
        e[k++] = (Ins){q1, "\tmp \"STOCK CARS\" /select MV_STOCK_ONLY\r\n", 0};
        e[k++] = (Ins){q2, "\t\t\ttext \" \"\r\n\t\t\ttext \"$MV_STOCK_ONLY\"\r\n", 0};
        e[k++] = (Ins){q3, "139", 3};             /* the panel one line longer */
    }
    if (opp && stock) {             /* seven lines: the car and its lines 8 lower, clear of BACK */
        const char *v = in_block(in, "block text championshiptextxxxx", "viewport 37 -25 97 35");
        const char *t1 = in_block(in, "block text championshiptextxxxx", "pos 67 24");
        const char *t2 = in_block(in, "block text championshiptextxxxx", "pos 102 24");
        const char *t3 = in_block(in, "block text championshiptextxxxx", "pos 122 24");
        if (v && t1 && t2 && t3) {
            e[k++] = (Ins){v + 12, "-17 97 43", 9};
            e[k++] = (Ins){t1 + 7, "32", 2};
            e[k++] = (Ins){t2 + 8, "32", 2};
            e[k++] = (Ins){t3 + 8, "32", 2};
        }
    }
    /* the same place twice (OPPONENTS, then STOCK CARS) stays in that order */
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && ins_cmp(&e[j - 1], &e[j]) > 0; j--) {
            Ins t = e[j]; e[j] = e[j - 1]; e[j - 1] = t;
        }
    size_t n = strlen(in), extra = 0;
    for (int i = 0; i < k; i++) extra += strlen(e[i].text);
    char *out = malloc(n + extra + 1), *o = out;
    const char *p = in;
    for (int i = 0; i < k; i++) {
        memcpy(o, p, (size_t)(e[i].at - p)); o += e[i].at - p;
        memcpy(o, e[i].text, strlen(e[i].text)); o += strlen(e[i].text);
        p = e[i].at + e[i].replace;
    }
    memcpy(o, p, strlen(p) + 1);
    return out;
}

/* 0x44c110(name, &size): the game's file opener */
static int menu_open_hook(CPU *c)
{
    if (prev_open && prev_open(c))
        return 1;
    uint32_t a = R32(c->esp + 4);
    if (!a)
        return 0;
    const char *name = (const char *)gptr(a), *base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    if (strcasecmp(base, "menu.cfg") || strncasecmp(name, "cache\\", 6) || !strncasecmp(name, "cache\\opponents", 15))
        return 0;
    int opp = want > SHIPPED_MAX, stock = mods_active != 0, v = opp | stock << 1;
    if (!v)
        return 0;
    if (!menu_tried[v]) {
        menu_tried[v] = 1;
        char host[1300], path[1300];
        char *in = NULL, *out = NULL;
        FILE *f = path_to_host(name, host, sizeof host, 0) ? fopen(host, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            in = malloc((size_t)(n > 0 ? n : 0) + 1);
            size_t got = n > 0 ? fread(in, 1, (size_t)n, f) : 0;
            in[got] = 0;
            fclose(f);
            out = menu_patch(in, opp, stock);
        }
        snprintf(path, sizeof path, "%s/cache/opponents", user_dir);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/cache/opponents/menu-%d.cfg", user_dir, v);
        FILE *w = out ? fopen(path, "wb") : NULL;
        if (w) {
            int ok = fwrite(out, 1, strlen(out), w) == strlen(out);
            if (fclose(w) == 0 && ok) {
                char win[64];
                snprintf(win, sizeof win, "cache\\opponents\\menu-%d.cfg", v);
                menu_names[v] = gstrdup(win);
            }
        }
        if (!menu_names[v])
            LOG(LOG_ALWAYS, "opponents: the setup screens are not as expected; no OPPONENTS / STOCK CARS lines");
        free(in);
        free(out);
    }
    if (menu_names[v])
        W32(c->esp + 4, menu_names[v]);
    return 0;
}

/* 0x47cff0(menu): the menu variables, ours in front */
static int menu_vars_hook(CPU *c)
{
    uint32_t menu = R32(c->esp + 4);
    if (prev_vars)
        prev_vars(c);
    else
        F_0047cff0_orig(c);
    if (!vars_first) {
        uint32_t lo = galloc(4), hi = galloc(4), zero = galloc(4);
        W32(lo, 3);
        W32(hi, MORE_MAX + 1);              /* (one past the most offered, as the game's own) */
        W32(zero, 0);
        stock_text = galloc(32);
        const uint32_t v[3][9] = {
            {0, gstrdup("MV_OPP_CHAMP"), champ_value, 1, lo, hi, 0, 0, 0},
            {0, gstrdup("MV_OPP_CHAMP_ON"), champ_on, 1, zero, zero, 0, 0, 0},
            {0, gstrdup("MV_STOCK_ONLY"), thunk_addr(stock_get, "stock cars text"), 0x100, zero, zero,
             thunk_addr(stock_next, "stock cars next"), thunk_addr(stock_prev, "stock cars prev"), 0}};
        for (int i = 0; i < 3; i++) {
            uint32_t e = galloc(36);
            for (int k = 0; k < 9; k++)
                W32(e + 4 * k, v[i][k]);
            if (vars_last) W32(vars_last, e); else vars_first = e;
            vars_last = e;
        }
    }
    W32(champ_on, want > SHIPPED_MAX);
    W32(vars_last, R32(menu + 0x10));
    W32(menu + 0x10, vars_first);
    return 1;
}

/* after enhanced_init (it sets the file and menu hooks without passing them on) */
void opponents_menus_init(void)
{
    if (!allowed)
        return;
    prev_open = hook_idf_open;
    hook_idf_open = menu_open_hook;
    prev_vars = hook_menu_vars;
    hook_menu_vars = menu_vars_hook;
}

/* ------------------------------------------------------------------ options */
static void parse(void)
{
    const char *v = option_str("max_opponents", "8");
    int n = atoi(v);
    want = n > SHIPPED_MAX ? MORE_MAX : SHIPPED_MAX;
    if (n != SHIPPED_MAX && n != MORE_MAX)
        LOG(LOG_ALWAYS, "options: max_opponents \"%s\" is not 8 or 15", v);
}

void opponents_init(void)
{
    allowed = !deterministic || getenv("INSANE_ENHANCED");
    oplog = getenv("INSANE_OPP_LOG") != NULL;
    if (getenv("INSANE_OPP_WIN"))
        win_at = strtoul(getenv("INSANE_OPP_WIN"), NULL, 10);
    if (!allowed)
        return;
    parse();
    hook_caches_empty = caches_empty_hook;
    hook_opponents_pick = opponents_pick_hook;
    hook_link_pool = link_pool_hook;
    hook_hud_table = hud_table_hook;
    hook_results_race = results_race_hook;
    hook_results_path = results_path_hook;
    hook_results_gates = results_gates_hook;
    hook_drivers_load = drivers_load_hook;
    hook_model_next = model_next_hook;
    hook_model_prev = model_prev_hook;
    prev_race_hud = hook_race_hud;      /* widescreen.c's, if any */
    hook_race_hud = race_hud_hook;
    hook_flat_quad = flat_quad_hook;
    hook_text_draw = text_draw_hook;
    hook_champ_event = champ_event_hook;
    hook_unlocks = unlocks_hook;
    hook_settings_load = settings_load_hook;
    stock_want = option_bool("stock_cars_only", 0);
    champ_value = galloc(4);
    champ_on = galloc(4);
    champ_last = atoi(option_str("championship_opponents", "15"));
    if (champ_last < 3 || champ_last > MORE_MAX)
        champ_last = MORE_MAX;
    W32(champ_value, (uint32_t)champ_last);
    W32(champ_on, want > SHIPPED_MAX);
    if (want > SHIPPED_MAX) {           /* nothing is cached or loaded yet */
        tables_apply(1);
        vehicles_switch(1);
    }
    menu_apply();
}

/* the Enhanced page changed the option: the menu follows now, the tables at the next race */
void opponents_options(void)
{
    if (!allowed)
        return;
    parse();
    menu_apply();
    roster_apply(want > SHIPPED_MAX);
    points_apply(want > SHIPPED_MAX);
    W32(champ_on, want > SHIPPED_MAX);
    if (champ_saved)
        champs_apply();
}
