/* The car and map pickers with fan content (design point 8, "picking from long lists").

   The game picks a car or a map in three places, all stepping one entry per press and wrapping
   round: Vehicle > Browse and Location > Browse, list menus ("mp /list ML_VEHICLES" and
   ML_MAPS in menu.cfg, 12 and 10 lines a page) walked with Up and Down, and the VEHICLE line of
   a championship's event screen (CHANGE in Quick Race's vehicle screen), MV_MYVEHICLENAME,
   walked with Left and Right. The game's controller handling (padgame.c) repeats while a
   direction is held; the keyboard does not repeat. The car list is sorted by class (a class's
   cars together, by file name), the map list by file name; a championship walks its class's
   cars only. With 430 cars (270 of them 4x4s) and 300 maps that is a long walk, so while one of
   those has the focus, and only with fan content:
   - PgUp/PgDn, or LB/RB on a controller: the first car of the previous/next class; where there
     is one class (maps, a championship), the same line of the previous/next page, or 10 on;
   - Home/End: the first/last entry;
   - a letter or digit key: the next entry whose name starts with it (again: the one after);
   - holding the direction repeats on the keyboard too, and speeds up the longer it is held.
   The jumps follow the game's own rules for which entries a step passes over (locked or not
   available ones, by game type) and land through the game's own step function, so the choice
   is made exactly as one more step would make it (name, paint job, preview). The menus' hint
   line (prompts.c) names the jumps while one of these has the focus. Without fan content none
   of this is hooked: the pickers behave as they always did, and so do they whenever the jumps
   are not used (a single press still moves one entry). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "funcs.h"
#include "image.h"
#include "input.h"
#include "mods.h"
#include "pad.h"

/* game addresses (GOG build) */
#define GAME_TYPE       0x4c7f18u       /* the steps' rules: 0 championship, 4 (only not-available
                                           entries are passed over), others */
#define CAR_CHOSEN      0x1bf394cu      /* the car list index chosen */
#define MAP_CHOSEN      0x1c54bd8u      /* the map list index chosen */
#define CAR_SIZE        0x40u           /* car list: name at +0, class at +0x34, locked +0x3c,
                                           not available +0x3d */
#define MAP_SIZE        0x60u           /* map list: name at +0x20, locked +0x5c, not available +0x5d */
#define CLASS_COUNTS    0x1c0dd1cu      /* + class * 0x8c: a class list's length (the lists
                                           themselves are TABLES class_lists, 35 slots a class) */
#define CLASS_STRIDE    0x8cu
#define CLASS_SLOTS     35u
#define CHAMP_SIZE      0x7f0u          /* championship records: +4 the class */
#define F_CURRENT_ITEM  0x472f10u       /* (menu) -> the item with the focus: +8 its kind (2 a
                                           Left/Right choice, its variable at +0xc), +0x10 a
                                           list's variable */
#define MENU_BLOCK_LINES 0x5cu          /* a menu block's maxlines (menu + 8 -> +8 -> the block) */

/* the pickers: the menu variable {link, name, value, ...}, its step forward */
enum { P_BROWSE_CARS, P_BROWSE_MAPS, P_CHANGE_CAR, NPICKERS };
static const struct {
    uint32_t var, step_fn;
    int cars, sideways;                 /* car list (else maps); stepped with Left/Right */
} pickers[NPICKERS] = {
    {0x4c9080u, 0x479180u, 1, 0},       /* ML_VEHICLES (Vehicle > Browse) */
    {0x4c9038u, 0x479880u, 0, 0},       /* ML_MAPS (Location > Browse) */
    {0x4c92c0u, 0x4794b0u, 1, 1},       /* MV_MYVEHICLENAME (championship VEHICLE, Quick Race CHANGE) */
};
static int focus = -1;                  /* the picker with the focus, as of the last menu frame */
static unsigned long focus_frame;

/* ------------------------------------------------------------------ the lists */
typedef struct {
    int picker;
    uint32_t base, size, chosen;
    uint32_t *order;                    /* indices in the order a step walks them */
    int n;
} List;

static int is_cars(const List *l) { return pickers[l->picker].cars; }

/* passed over by a step (each step function's own rule) */
static int hidden(const List *l, uint32_t i)
{
    uint32_t e = l->base + i * l->size, type = R32(GAME_TYPE);
    int locked = R8(e + (is_cars(l) ? 0x3c : 0x5c)) != 0, unavailable = R8(e + (is_cars(l) ? 0x3d : 0x5d)) != 0;
    if (type == 4) return unavailable;
    if (is_cars(l) && (type == 0 || pickers[l->picker].sideways)) return locked || unavailable;
    return locked;
}

static int one_class(const List *l) { return !is_cars(l) || R32(GAME_TYPE) == 0; }

/* the list as a step walks it: index order, or a championship's class list */
static int list_get(List *l, int picker)
{
    static uint32_t order[8192];
    memset(l, 0, sizeof *l);
    l->picker = picker;
    l->order = order;
    uint32_t n;
    if (is_cars(l)) {
        l->base = reloc_cars, l->size = CAR_SIZE, l->chosen = CAR_CHOSEN;
        if (R32(GAME_TYPE) == 0) {
            uint32_t champ = R32(GAME_CHAMP), cls = R32(GAME_CHAMPS + champ * CHAMP_SIZE + 4);
            n = champ < 5 ? R32(CLASS_COUNTS + cls * CLASS_STRIDE) : 0;
            for (uint32_t k = 0; k < n && k < 8192; k++)
                order[l->n++] = R32(reloc_class_lists + (cls * CLASS_SLOTS + k) * 4);
            return l->n > 0;
        }
        n = R32(GAME_CAR_COUNT);
    } else {
        l->base = reloc_maps, l->size = MAP_SIZE, l->chosen = MAP_CHOSEN;
        n = R32(GAME_MAP_COUNT);
    }
    for (uint32_t i = 0; i < n && i < 8192; i++)
        order[l->n++] = i;
    return l->n > 0;
}

static int pos_of(const List *l, uint32_t index)
{
    for (int p = 0; p < l->n; p++)
        if (l->order[p] == index) return p;
    return 0;                           /* as the game's step: from the start */
}

static uint32_t class_of(const List *l, int p) { return R32(l->base + l->order[p] * l->size + 0x34); }

static const char *name_of(const List *l, int p)
{
    return (const char *)gptr(l->base + l->order[p] * l->size + (is_cars(l) ? 0 : 0x20));
}

/* the nearest shown position from p in direction d (p itself if shown), -1 if none */
static int shown_from(const List *l, int p, int d)
{
    for (int k = 0; k < l->n; k++, p = (p + d + l->n) % l->n)
        if (!hidden(l, l->order[p])) return p;
    return -1;
}

/* Makes position p the choice through the game's own step: the choice goes to the shown entry
   before it and the step moves on to it. */
static void choose(CPU *c, uint32_t menu, const List *l, int p)
{
    p = p < 0 ? -1 : shown_from(l, p, 1);
    int cur = pos_of(l, R32(l->chosen));
    if (p < 0 || p == cur) return;
    int before = shown_from(l, (p - 1 + l->n) % l->n, -1);
    W32(l->chosen, l->order[before]);
    guest_call(c, pickers[l->picker].step_fn, 1, menu);
    LOG(LOG_INPUT, "picker: %s %u -> %u", is_cars(l) ? "car" : "map", l->order[cur], R32(l->chosen));
}

/* ------------------------------------------------------------------ the jumps */
enum { J_PREV, J_NEXT, J_FIRST, J_LAST };

static void jump(CPU *c, uint32_t menu, int picker, int how)
{
    List l;
    if (!list_get(&l, picker)) return;
    int cur = pos_of(&l, R32(l.chosen)), n = l.n, p = cur;
    if (how == J_FIRST || how == J_LAST) {
        choose(c, menu, &l, how == J_FIRST ? 0 : shown_from(&l, n - 1, -1));
        return;
    }
    if (!one_class(&l)) {
        /* a class's first car: next, the one after this class; back, the one before this
           class's first (wrapping round) */
        uint32_t cls = class_of(&l, cur);
        if (how == J_NEXT) {
            while (p < n && class_of(&l, p) == cls) p++;
            if (p == n) p = 0;
        } else {
            while (p > 0 && class_of(&l, p - 1) == cls) p--;
            p = (p - 1 + n) % n;
            uint32_t prev = class_of(&l, p);
            while (p > 0 && class_of(&l, p - 1) == prev) p--;
        }
        choose(c, menu, &l, p);
        return;
    }
    /* a page (the list menu's lines, 10 for the Left/Right choice): the same line of the
       next/previous page; past the end, round to the other end */
    int lines = 10;
    if (!pickers[picker].sideways) {
        lines = (int)R32(R32(R32(menu + 8) + 8) + MENU_BLOCK_LINES);
        if (lines < 1 || lines > 100) lines = 10;
    }
    if (how == J_NEXT)
        p = cur / lines == (n - 1) / lines ? 0 : cur + lines < n ? cur + lines : n - 1;
    else
        p = cur < lines ? n - 1 : cur - lines;
    int q = shown_from(&l, p, how == J_NEXT ? 1 : -1);
    if (q >= 0) choose(c, menu, &l, q);
}

static void jump_letter(CPU *c, uint32_t menu, int picker, char ch)
{
    List l;
    if (!list_get(&l, picker)) return;
    int cur = pos_of(&l, R32(l.chosen));
    for (int k = 1; k < l.n; k++) {
        int p = (cur + k) % l.n;
        if (hidden(&l, l.order[p])) continue;
        const char *s = name_of(&l, p);
        while (*s && !isalnum((unsigned char)*s)) s++;
        if (toupper((unsigned char)*s) == ch) {
            choose(c, menu, &l, p);
            return;
        }
    }
}

/* ------------------------------------------------------------------ keys and buttons */
static double nav_now(void)
{
    if (deterministic)
        return frame_count * (1000.0 / 60.0);       /* test runs: frames, as padgame.c */
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int key(int dik) { return R8(GAME_PRESSED + (uint32_t)dik) != 0; }

static int stick(int axis, int sign) { return pad_axis_value(axis) * sign > 16384; }

/* letter and digit keys -> their character */
static const struct { uint8_t dik; char ch; } letters[] = {
    {DIK_A, 'A'}, {DIK_B, 'B'}, {DIK_C, 'C'}, {DIK_D, 'D'}, {DIK_E, 'E'}, {DIK_F, 'F'}, {DIK_G, 'G'},
    {DIK_H, 'H'}, {DIK_I, 'I'}, {DIK_J, 'J'}, {DIK_K, 'K'}, {DIK_L, 'L'}, {DIK_M, 'M'}, {DIK_N, 'N'},
    {DIK_O, 'O'}, {DIK_P, 'P'}, {DIK_Q, 'Q'}, {DIK_R, 'R'}, {DIK_S, 'S'}, {DIK_T, 'T'}, {DIK_U, 'U'},
    {DIK_V, 'V'}, {DIK_W, 'W'}, {DIK_X, 'X'}, {DIK_Y, 'Y'}, {DIK_Z, 'Z'}, {DIK_0, '0'}, {DIK_1, '1'},
    {DIK_2, '2'}, {DIK_3, '3'}, {DIK_4, '4'}, {DIK_5, '5'}, {DIK_6, '6'}, {DIK_7, '7'}, {DIK_8, '8'},
    {DIK_9, '9'},
};
#define NLETTERS (int)(sizeof letters / sizeof *letters)

/* held controls, with when each went down and when it next repeats */
enum { C_BACK_KEY, C_ON_KEY, C_BACK_PAD, C_ON_PAD, C_PREV, C_NEXT, C_FIRST, C_LAST, NCTRL };
static struct { int on; double since, next; } ctrl[NCTRL];
static uint8_t letter_was[NLETTERS];

/* a jump control this frame: 1 for a press (or a repeat while held), 0 otherwise */
static int edge(int k, int held, double now, double delay, double rate)
{
    if (!held) {
        ctrl[k].on = 0;
        return 0;
    }
    if (!ctrl[k].on) {
        ctrl[k].on = 1;
        ctrl[k].since = now;
        ctrl[k].next = now + delay;
        return 1;
    }
    if (rate > 0 && now >= ctrl[k].next) {
        ctrl[k].next = now + rate;
        return 1;
    }
    return 0;
}

/* A direction held: repeats that speed up. The menu takes the first press itself; a
   controller's repeats come from padgame.c every 120 ms, so for it only the faster ones are
   added. */
static double hold_rate(double held_for, int pad)
{
    if (held_for < 1200) return pad ? 0 : 120;
    if (held_for < 2400) return 60;
    return 1;                           /* every frame */
}

static void repeat_dir(int k, int held, double now, int nav, int pad)
{
    if (!held) {
        ctrl[k].on = 0;
        return;
    }
    if (!ctrl[k].on) {
        ctrl[k].on = 1;
        ctrl[k].since = now;
        ctrl[k].next = now + 400;
        return;
    }
    if (now < ctrl[k].next) return;
    double rate = hold_rate(now - ctrl[k].since, pad);
    if (rate > 0)
        W8(GAME_MENU_NAV + (uint32_t)nav, 0);  /* the menu's input then takes it as a new press */
    ctrl[k].next = now + (rate > 0 ? rate : 1);
}

static int focused(CPU *c, uint32_t menu)
{
    if (!menu || !R32(menu + 8)) return -1;
    uint32_t item = guest_call(c, F_CURRENT_ITEM, 1, menu);
    if (!item) return -1;
    for (int k = 0; k < NPICKERS; k++)
        if (pickers[k].sideways ? R32(item + 8) == 2 && R32(item + 0xc) == pickers[k].var
                                : R32(item + 0x10) == pickers[k].var)
            return k;
    return -1;
}

/* 0x4759f0(menu): a frame of the menus (input, then the menu's actions) */
static int menu_frame_hook(CPU *c)
{
    uint32_t menu = R32(c->esp + 4);
    int was = focus;
    focus = focused(c, menu);
    if (focus != was) {
        memset(ctrl, 0, sizeof ctrl);
        List l;
        if (focus >= 0 && list_get(&l, focus))
            LOG(LOG_INPUT, "picker: %s, %d entries, game type %u", focus == P_BROWSE_CARS ? "car list" :
                focus == P_BROWSE_MAPS ? "map list" : "vehicle choice", l.n, R32(GAME_TYPE));
    }
    if (focus < 0) {
        F_004759f0_orig(c);
        return 1;
    }
    focus_frame = frame_count;
    double now = nav_now();
    /* the direction held (as of the last frame's input: the menu reads this frame's itself) */
    int side = pickers[focus].sideways, nav = side ? 2 : 0;
    int back_pad = pad_held(side ? PAD_DLEFT : PAD_DUP) || stick(side ? PAD_LX : PAD_LY, -1);
    int on_pad = pad_held(side ? PAD_DRIGHT : PAD_DDOWN) || stick(side ? PAD_LX : PAD_LY, 1);
    int back_key = key(side ? DIK_LEFT : DIK_UP), on_key = key(side ? DIK_RIGHT : DIK_DOWN);
    int any = back_pad + on_pad + back_key + on_key;
    repeat_dir(C_BACK_KEY, back_key && any == 1, now, nav, 0);
    repeat_dir(C_ON_KEY, on_key && any == 1, now, nav + 1, 0);
    repeat_dir(C_BACK_PAD, back_pad && any == 1, now, nav, 1);
    repeat_dir(C_ON_PAD, on_pad && any == 1, now, nav + 1, 1);
    F_004759f0_orig(c);
    /* the jumps, on this frame's input */
    int k = focus;
    if (edge(C_PREV, key(DIK_PRIOR) || pad_held(PAD_LB), now, 400, 200)) jump(c, menu, k, J_PREV);
    if (edge(C_NEXT, key(DIK_NEXT) || pad_held(PAD_RB), now, 400, 200)) jump(c, menu, k, J_NEXT);
    if (edge(C_FIRST, key(DIK_HOME), now, 0, 0)) jump(c, menu, k, J_FIRST);
    if (edge(C_LAST, key(DIK_END), now, 0, 0)) jump(c, menu, k, J_LAST);
    for (int i = 0; i < NLETTERS; i++) {
        int down = key(letters[i].dik);
        if (down && !letter_was[i])
            jump_letter(c, menu, k, letters[i].ch);
        letter_was[i] = (uint8_t)down;
    }
    return 1;
}

/* ------------------------------------------------------------------ the hint line */
/* For prompts.c: the hint line while a picker has the focus, NULL otherwise. The controller's
   version has [LB] and [RB] for those buttons' icons ("Up/Down", "Left/Right" and "Enter" are
   the words prompts.c already turns into icons). */
const char *picker_hint(int controller)
{
    if (focus < 0 || frame_count > focus_frame + 2)
        return NULL;
    int classes = pickers[focus].cars && R32(GAME_TYPE) != 0;
    const char *cls = classes ? classes_car_name(R32(CAR_CHOSEN)) : NULL;
    if (cls) {                          /* CAR CLASSES REORGANIZED: the class's name first */
        static char line[160];
        const char *rest = pickers[focus].sideways
            ? (controller ? "Left/Right to change vehicle. [LB]/[RB] to change class"
                          : "Left/Right to change. PgUp/PgDn to change class. A-Z by name")
            : (controller ? "Up/Down to select. [LB]/[RB] to change class. Enter to change menu"
                          : "Up/Down to select. PgUp/PgDn to change class. A-Z by name");
        snprintf(line, sizeof line, "%s. %s", cls, rest);
        return line;
    }
    if (pickers[focus].sideways) {
        if (controller)
            return classes ? "Left/Right to change vehicle. [LB]/[RB] to change class"
                           : "Left/Right to change vehicle. [LB]/[RB] to skip 10";
        return classes ? "Left/Right to change vehicle. PgUp/PgDn to change class. A-Z by name"
                       : "Left/Right to change vehicle. PgUp/PgDn to skip 10. A-Z by name";
    }
    if (controller)
        return classes ? "Up/Down to select. [LB]/[RB] to change class. Enter to change menu"
                       : "Up/Down to select. [LB]/[RB] to turn the page. Enter to change menu";
    return classes ? "Up/Down to select. PgUp/PgDn to change class. A-Z by name. Enter to change menu"
                   : "Up/Down to select. PgUp/PgDn to turn the page. A-Z by name. Enter to change menu";
}

void picker_init(void)
{
    hook_menu_frame = menu_frame_hook;
}
