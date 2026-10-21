/* Car classes and the car menus (design point 8, "car classes"): with fan content, the class
   names, Vehicle > Browse in classes, the measured performance on the vehicle screen, and the
   Enhanced option CAR CLASSES: ORIGINAL / REORGANIZED (options.txt car_classes, read at
   start-up).

   A car's class is the letter of its .vht's "group" line; the five championships own x 4X4,
   c SPORTS, p PICKUPS, t TRUCKS and w EXTREME, and every other letter is a class of its own (see
   mods.c). With Ant's folders 270 cars of every kind share 4X4. The table (assets/car_classes.txt,
   made by tools/car_classes.py from the 2026-10-18 analysis Ant approved: real-world type, and
   speed measured in the game) gives each car a class and its measured numbers.

   Always (with fan content, ORIGINAL or REORGANIZED):
   - class names: REORGANIZED the table's; ORIGINAL the game's (4X4, SPORTS, PICKUPS, TRUCKS,
     EXTREME), BONUS for 'b' (the bonus cars), OTHER for 'o' (always unlocked: the stock Gnu and
     Aeroplane and fan cars of every kind) and "CLASS <letter>" for any other letter;
   - Vehicle > Browse in classes: each class starts with a heading row, "-- 4X4 OPEN -----", a
     rule to the list's edge (a heading that would end a page goes to the next one). The menu
     variable ML_VEHICLES gets a position and a length of ours and the row text, 0x4721c0, a
     row's car or heading; the position follows the chosen car, also right after each step so the
     list's own wrap test sees it; a name too wide for the list's column is cut to it with "..."
     (carnames.c). With STOCK CARS (opponents.c) the stock cars alone are rows;
   - the vehicle screen: the class's name heads the car's panel (instead of "Vehicle:"); the
     FACTORY STATISTICS (the game's words, in a smaller font so more fits) add the measured top
     speed and 0-100 km/h, the engine's power against the stock cars', the gears, and where the
     car ranks in its class (top speed and 0-100 among the class's measured cars); the
     championship event screen gets "Class: <name>" under the car; picker.c's hint line starts
     with the class. A car without numbers in the table (a user's own line) shows the words alone.
   REORGANIZED also:
   - letters and order: each car gets the table's class, and the list is put in the table's class
     order before the game numbers the classes (0x47d8a0 numbers the championships' letters 0-4
     and the others by first appearance, then sorts the list by class, which keeps an already
     sorted list as it is), so Browse shows 4X4, SPORTS, PICKUPS, TRUCKS, EXTREME, then the race
     classes, each by file name. The seven bonus cars are in their type's classes (Ant, 2026-10-19:
     no BONUS class): the ATV in QUADS & BIKES, the Raider in 4X4, the Wrecker and Police in MUSCLE &
     CLASSICS, the Speeder in ODDITIES, the Rally 2.0i and Rallycroxx in RALLY;
   - unlocks: after 0x46e690 works out what is locked, the race classes' cars ("always" in the
     table) are unlocked; the championship classes and ODDITIES stay as the game does it. Each
     bonus car stays locked until it is won, as the game decides it (see "unlocks" below);
   - opponents (0x45e500, through opponents.c's hook): "class" took the first 100 car/paint pairs
     of the player's class in list order (of 270 4x4s only the first 20 by file name ever
     raced) and "free" the same of the five championship classes. Now both draw from the whole
     pool: "free" from every class but ODDITIES and UNLIMITED. The game's own pick runs on a
     random sample of at most 100 pairs (shuffled with the game's random numbers): for the call
     the sampled cars wear the player's letter, the others with it are marked locked, and
     "free" runs as "class", so each opponent gets a car of its own while there are enough (the
     game's "free" drew pairs and repeated cars with many paint jobs); then all is put back. An
     ODDITIES car racing "class" opponents still races copies of itself, as shipped.
   A car the table doesn't name keeps its own letter; a car_classes.txt in the user folder (the
   same lines) adds or overrides entries. Without fan content nothing changes. Off in --test runs
   without INSANE_ENHANCED, like the other Enhanced options. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include "runtime.h"
#include "funcs.h"
#include "image.h"
#include "mods.h"
#include "options.h"
#include "car_classes_txt.h"            /* generated from assets/car_classes.txt */

#define CAR_SIZE        0x40u           /* car list: file name +0x20, letter +0x30, class +0x34, */
#define CAR_FILE        0x20u           /* paint jobs +0x38, locked +0x3c, not available +0x3d */
#define CAR_LETTER      0x30u
#define CAR_PAINTS      0x38u
#define CAR_LOCKED      0x3cu
#define CAR_UNAVAILABLE 0x3du
#define CHAMPS          5
#define CHAMP_SIZE      0x7f0u          /* championship records: +0 their letter */
#define CHAMP_BONUS     0x778u          /* +0x778 the bonus car, +0x7c4 the 100% bonus car: car numbers, -1 none */
#define CHAMP_BONUS100  0x7c4u
#define PART_SIZE       0x4d8u          /* participants: +0x54 the car */
#define F_RANDOM        0x480170u       /* (n) -> 0..n-1, the game's random numbers */
#define PICK_PAIRS      100             /* 0x45e500's room for car/paint pairs */
#define ML_VEHICLES     0x4c9080u       /* {link, name, value, type, min, max, next, prev, row text} */
#define ROW_TEXT_FLAGS  1u              /* a list row's flags: 1 shown (2 the chosen one, 4 greyed) */
#define HEADING_WIDTH   52              /* a heading row's width in half dashes, to the edge of the list */

static int enabled, active, oplog;
typedef struct { uint8_t letter; int always; char name[32]; } Class;
static Class classes[64];
static int nclasses;
typedef struct {
    char file[16];
    uint8_t letter;                     /* 0: no class in the table (numbers only) */
    float top, t100, t50, power;        /* measured km/h, s, s; the engine's power; < 0 unknown */
} Entry;
static Entry *entries;
static int nentries, capentries;

int classes_on(void) { return active; }

/* ------------------------------------------------------------------ the table */
static Class *class_of_letter(uint8_t l)
{
    for (int i = 0; i < nclasses; i++)
        if (classes[i].letter == l) return &classes[i];
    return NULL;
}

static float field(const char *w)
{
    if (!w || !strcmp(w, "-")) return -1;
    char *end;
    float v = strtof(w, &end);
    return end != w ? v : -1;
}

static void table_line(char *s, const char *src, int lineno)
{
    for (char *h = s; *h; h++)          /* a comment: '#' at the start or after a space ("bf#14rf" is a name) */
        if (*h == '#' && (h == s || h[-1] == ' ' || h[-1] == '\t')) {
            *h = 0;
            break;
        }
    char *w[8] = {0};
    int n = 0;
    for (char *p = s; *p && n < 8;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        w[n++] = p;
        if (n == 4 && !strcmp(w[0], "class")) break;   /* the name: the rest of the line */
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
        if (*p) *p++ = 0;
    }
    if (!n) return;
    if (!strcmp(w[0], "class") && n == 4 && strlen(w[1]) == 1) {
        char *e = w[3] + strlen(w[3]);
        while (e > w[3] && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
        Class *c = class_of_letter((uint8_t)w[1][0]);
        if (!c && nclasses < 64) c = &classes[nclasses++];
        if (!c) return;
        c->letter = (uint8_t)w[1][0];
        c->always = !strcmp(w[2], "always");
        snprintf(c->name, sizeof c->name, "%s", w[3]);
    } else if (!strcmp(w[0], "car") && n >= 3 && strlen(w[2]) == 1 && strlen(w[1]) < 16) {
        /* car <file> <letter> [top km/h, 0-100 s, 0-50 s, power]: '-' for a letter or a number unknown */
        Entry *e = NULL;
        for (int i = 0; i < nentries && !e; i++)
            if (!strcasecmp(entries[i].file, w[1])) e = &entries[i];
        if (!e) {
            if (nentries == capentries) {
                capentries = capentries ? capentries * 2 : 512;
                entries = realloc(entries, capentries * sizeof *entries);
            }
            e = &entries[nentries++];
            snprintf(e->file, sizeof e->file, "%s", w[1]);
        }
        e->letter = w[2][0] == '-' ? 0 : (uint8_t)w[2][0];
        e->top = field(w[3]);
        e->t100 = field(w[4]);
        e->t50 = field(w[5]);
        e->power = field(w[6]);
    } else {
        LOG(LOG_ALWAYS, "classes: %s line %d: \"%s\" is not \"class <letter> <unlock> <name>\" or \"car <file> "
            "<letter> [<top km/h> <0-100 s> <0-50 s> <power>]\"", src, lineno, w[0]);
    }
}

static void table_parse(const char *text, const char *src)
{
    int lineno = 0;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        snprintf(line, sizeof line, "%.*s", (int)(k < sizeof line ? k : sizeof line - 1), p);
        table_line(line, src, ++lineno);
        p += k + (e != NULL);
    }
}

static Entry *table_entry(const char *file)
{
    for (int i = 0; i < nentries; i++)
        if (!strcasecmp(entries[i].file, file)) return &entries[i];
    return NULL;
}

/* the stock cars' power, for the vehicle screen: their range */
static float power_lo = -1, power_hi = -1;

static void stock_power(void)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/Data/Vehicles", game_dir);
    DIR *d = opendir(p);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        size_t k = strlen(de->d_name);
        if (k < 5 || k - 4 >= 16 || strcasecmp(de->d_name + k - 4, ".idf")) continue;
        char f[16];
        snprintf(f, sizeof f, "%.*s", (int)(k - 4), de->d_name);
        Entry *e = table_entry(f);
        if (!e || e->power < 0) continue;
        if (power_lo < 0 || e->power < power_lo) power_lo = e->power;
        if (e->power > power_hi) power_hi = e->power;
    }
    if (d) closedir(d);
}

void classes_init(void)
{
    oplog = getenv("INSANE_OPP_LOG") != NULL;
    if ((deterministic && !getenv("INSANE_ENHANCED")) || !mods_active)
        return;
    enabled = 1;
    const char *v = option_str("car_classes", "original");
    active = !strcasecmp(v, "reorganized");
    if (!active && strcasecmp(v, "original"))
        LOG(LOG_ALWAYS, "options: car_classes \"%s\" is not original or reorganized", v);
    table_parse(car_classes_txt, "assets/car_classes.txt");
    char path[1300];
    snprintf(path, sizeof path, "%s/car_classes.txt", user_dir);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *text = malloc((size_t)(n > 0 ? n : 0) + 1);
        size_t got = n > 0 ? fread(text, 1, (size_t)n, f) : 0;
        text[got] = 0;
        fclose(f);
        table_parse(text, "car_classes.txt");
        free(text);
    }
    stock_power();
    logmsg(LOG_ALWAYS, "classes: CAR CLASSES %s: %d classes, %d cars in the table%s", active ? "REORGANIZED" : "ORIGINAL",
           nclasses, nentries, f ? " (with the user folder's car_classes.txt)" : "");
}

/* ------------------------------------------------------------------ letters and order */
/* From mods.c's champdef hook: the car list is complete, the classes not built yet. */
void classes_letters(void)
{
    if (!active)
        return;
    uint32_t n = R32(GAME_CAR_COUNT), moved = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = reloc_cars + i * CAR_SIZE;
        Entry *t = table_entry((const char *)gptr(e + CAR_FILE));
        if (t && t->letter && t->letter != (uint8_t)R8(e + CAR_LETTER)) {
            W8(e + CAR_LETTER, t->letter);
            moved++;
        }
    }
    /* the order: the championships' letters as the game numbers them, the table's other classes,
       then letters the table doesn't know, in order of first appearance */
    uint8_t champ[CHAMPS];
    for (int k = 0; k < CHAMPS; k++)
        champ[k] = (uint8_t)R8(GAME_CHAMPS + k * CHAMP_SIZE);
    int key_of[256];
    for (int l = 0; l < 256; l++) key_of[l] = -1;
    for (int k = 0; k < CHAMPS; k++) key_of[champ[k]] = k;
    int next = CHAMPS;
    for (int c = 0; c < nclasses; c++)
        if (key_of[classes[c].letter] < 0) key_of[classes[c].letter] = next++;
    uint8_t *buf = malloc(n * CAR_SIZE + 1);
    int *key = malloc((n + 1) * sizeof *key);
    uint32_t *idx = malloc((n + 1) * sizeof *idx);
    for (uint32_t i = 0; i < n; i++) {
        memcpy(buf + i * CAR_SIZE, gptr(reloc_cars + i * CAR_SIZE), CAR_SIZE);
        uint8_t l = buf[i * CAR_SIZE + CAR_LETTER];
        if (key_of[l] < 0) key_of[l] = next++;
        key[i] = key_of[l];
        idx[i] = i;
    }
    for (uint32_t i = 1; i < n; i++) {         /* stable: each class keeps the file name order */
        uint32_t v = idx[i];
        int j = (int)i - 1;
        while (j >= 0 && key[idx[j]] > key[v]) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    for (uint32_t i = 0; i < n; i++)
        memcpy(gptr(reloc_cars + i * CAR_SIZE), buf + idx[i] * CAR_SIZE, CAR_SIZE);
    free(buf);
    free(key);
    free(idx);
    LOG(LOG_ALWAYS, "classes: %u of %u cars change class; the list is in the table's class order", moved, n);
}

/* ------------------------------------------------------------------ names */
const char *classes_name(uint8_t letter)
{
    if (!enabled)
        return NULL;
    Class *c = active ? class_of_letter(letter) : NULL;
    if (c)
        return c->name;
    static const struct { uint8_t letter; const char *name; } shipped[] = {
        {'x', "4X4"}, {'c', "SPORTS"}, {'p', "PICKUPS"}, {'t', "TRUCKS"}, {'w', "EXTREME"},
        {'b', "BONUS"}, {'o', "OTHER"}};
    for (size_t k = 0; k < sizeof shipped / sizeof *shipped; k++)
        if (shipped[k].letter == letter)
            return shipped[k].name;
    static char other[256][12];
    if (!other[letter][0])
        snprintf(other[letter], sizeof other[letter], "CLASS %c", letter >= 32 && letter < 127 ? letter : '?');
    return other[letter];
}

const char *classes_car_name(uint32_t car)
{
    if (!enabled || car >= R32(GAME_CAR_COUNT))
        return NULL;
    return classes_name((uint8_t)R8(reloc_cars + car * CAR_SIZE + CAR_LETTER));
}

/* ------------------------------------------------------------------ unlocks */
/* 0x46e690 works out what is locked (start-up, after a championship race, the unlock codes):
   every car locked, then the cars of each open championship's letter and every 'o' car unlocked,
   and each championship's bonus cars once the player's points in it reach their targets
   (champdef.cfg's "bonus car" and "100% bonus car", which start-up turns into car numbers after
   sorting the list: +0x778 / +0x7c4 of the championship's record, -1 for none); ineedmorecars
   unlocks everything. With REORGANIZED the bonus cars are in their type's classes, so by letter
   the Raider would open with the 4X4 championship, the Speeder with every 'o' car, and the others
   with the race classes below. So for the game's pass each bonus car wears the letter it has in
   its .vht, 'b' (no championship's, not 'o'): only the game's own bonus rule and the code open it,
   exactly as with the BONUS class; then its class letter is put back and the race classes' unlock
   passes over it. Once won it is a car of its class like the others: chosen in Browse, a
   championship's VEHICLE (the Raider in 4X4) and the opponents. */
static void view_build(void);
static int view_built, hooked;
static struct { uint32_t car; uint8_t letter; } bonus[CHAMPS * 2];
static int nbonus;

static int is_bonus(uint32_t car)
{
    for (int b = 0; b < nbonus; b++)
        if (bonus[b].car == car)
            return 1;
    return 0;
}

/* Before 0x46e690 (opponents.c's hook); the bonus cars are listed with fan content (for
   INSANE_OPP_LOG under both options), their letters changed only with REORGANIZED */
void classes_unlock_begin(void)
{
    nbonus = 0;
    if (!enabled)
        return;
    uint8_t wear = 'b';
    for (int k = 0; k < CHAMPS; k++)    /* (a championship of 'b' would open it: a letter of none) */
        if (R8(GAME_CHAMPS + k * CHAMP_SIZE) == wear)
            wear = 1;
    uint32_t n = R32(GAME_CAR_COUNT);
    for (int k = 0; k < CHAMPS; k++)
        for (int j = 0; j < 2; j++) {
            uint32_t car = R32(GAME_CHAMPS + k * CHAMP_SIZE + (j ? CHAMP_BONUS100 : CHAMP_BONUS));
            if (car >= n || is_bonus(car))
                continue;
            uint32_t e = reloc_cars + car * CAR_SIZE;
            bonus[nbonus].car = car;
            bonus[nbonus].letter = (uint8_t)R8(e + CAR_LETTER);
            nbonus++;
            if (active)
                W8(e + CAR_LETTER, wear);
        }
}

/* After 0x46e690 (opponents.c's hook, before its stock-cars marks) */
void classes_unlock(void)
{
    if (active)                         /* the bonus cars' classes back (before Browse's headings) */
        for (int b = 0; b < nbonus; b++)
            W8(reloc_cars + bonus[b].car * CAR_SIZE + CAR_LETTER, bonus[b].letter);
    if (hooked && !view_built)
        view_build();                   /* the car list is complete: Browse in classes from now on */
    if (active) {
        uint32_t n = R32(GAME_CAR_COUNT);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t e = reloc_cars + i * CAR_SIZE;
            Class *c = class_of_letter((uint8_t)R8(e + CAR_LETTER));
            if (c && c->always && !is_bonus(i))
                W8(e + CAR_LOCKED, 0);
        }
    }
    if (oplog)
        for (int b = 0; b < nbonus; b++) {
            uint32_t e = reloc_cars + bonus[b].car * CAR_SIZE;
            LOG(LOG_ALWAYS, "classes: bonus car %s (%s) %s", (const char *)gptr(e + CAR_FILE),
                classes_name(bonus[b].letter), R8(e + CAR_LOCKED) ? "locked" : "unlocked");
        }
}

/* ------------------------------------------------------------------ Vehicle > Browse */
/* rows: a car's index, or a heading (HEADING + its letter) */
#define HEADING 0x80000000u
#define BLANK   0x40000000u             /* an empty row */
#define VIEW_LINES 12                   /* Browse's rows a page (menu.cfg vhcbrowse: maxlines 12) */
static uint32_t view_pos, view_len;     /* program ints: the position and the count shown */
static uint32_t *view_rows, *view_at;   /* row -> car or heading; car -> row */
static uint32_t view_n, view_cars;
static int view_on;
static int (*view_keep)(uint32_t car);  /* STOCK CARS: the cars shown, NULL all */
static uint32_t heading_text, row_text;

static void view_build(void)
{
    view_built = 1;
    if (!hooked || (!enabled && !view_keep)) {     /* (no row hook: the variable as shipped) */
        if (view_on) {
            W32(ML_VEHICLES + 8, GAME_CAR_CHOSEN);
            W32(ML_VEHICLES + 20, GAME_CAR_COUNT);
            view_on = 0;
        }
        return;
    }
    if (!view_pos) {
        view_pos = galloc(4);
        view_len = galloc(4);
        heading_text = galloc(64);
    }
    uint32_t n = R32(GAME_CAR_COUNT);
    free(view_rows);
    free(view_at);
    view_rows = malloc(sizeof *view_rows * (2 * n + 1));
    view_at = malloc(sizeof *view_at * (n + 1));
    view_n = 0;
    view_cars = n;
    int last = -1;
    for (uint32_t i = 0; i < n; i++) {
        view_at[i] = 0xffffffffu;
        if (view_keep && !view_keep(i))
            continue;
        int l = R8(reloc_cars + i * CAR_SIZE + CAR_LETTER);
        if (enabled && l != last) {
            if (view_n % VIEW_LINES == VIEW_LINES - 1)
                view_rows[view_n++] = BLANK;    /* a heading at a page's foot goes to the next page */
            view_rows[view_n++] = HEADING | (uint32_t)l;
        }
        last = l;
        view_at[i] = view_n;
        view_rows[view_n++] = i;
    }
    W32(view_len, view_n);
    W32(ML_VEHICLES + 8, view_pos);
    W32(ML_VEHICLES + 20, view_len);
    view_on = 1;
    classes_browse_sync();
}

void classes_browse_sync(void)
{
    uint32_t i = R32(GAME_CAR_CHOSEN);
    if (view_on && i < view_cars && view_at[i] != 0xffffffffu)
        W32(view_pos, view_at[i]);
}

/* opponents.c: STOCK CARS on (the stock cars alone) or off */
void classes_browse_filter(int (*keep)(uint32_t car))
{
    view_keep = keep;
    view_build();
}

/* 0x4721c0(menu, value, row, &text): a row of Vehicle > Browse */
static int car_row_hook(CPU *c)
{
    if (!view_built)
        view_build();
    if (!view_on || R32(ML_VEHICLES + 8) != view_pos)
        return 0;
    uint32_t row = R32(c->esp + 12);
    classes_browse_sync();
    uint32_t what = row < view_n ? view_rows[row] : 0;
    if (what & (HEADING | BLANK)) {
        char t[64] = "";
        if (what & BLANK) {
            gstrncpy(heading_text, t, 64);
            W32(R32(c->esp + 16), heading_text);
            c->eax = ROW_TEXT_FLAGS;
            c->ret = R32(c->esp);
            c->esp += 4;
            return 1;
        }
        /* a rule across the list: "-- ROCK CRAWLERS ----------" (the menus' sliders are dashes too) */
        snprintf(t, sizeof t, "-- %s ", classes_name((uint8_t)(what & 0xff)));
        /* to the list's right edge, before the menu beside it: in the menu font a letter is about
           1.5 dashes wide, a space one */
        float w = 0;
        for (const char *p = t; *p; p++) w += *p == '-' || *p == ' ' ? 2 : 3;
        for (int k = 0; (w < HEADING_WIDTH || k < 1) && strlen(t) < sizeof t - 1; k++, w += 2)
            strcat(t, "-");
        gstrncpy(heading_text, t, 64);
        W32(R32(c->esp + 16), heading_text);
        c->eax = ROW_TEXT_FLAGS;
        c->ret = R32(c->esp);
        c->esp += 4;
        return 1;
    }
    W32(c->esp + 8, R32(GAME_CAR_CHOSEN));
    W32(c->esp + 12, what);
    /* the game's row, then a name too wide for the list's column cut to it (carnames.c) */
    uint32_t outp = R32(c->esp + 16);
    F_004721c0_orig(c);
    uint32_t t = outp ? R32(outp) : 0;
    if (t && what < R32(GAME_CAR_COUNT) && !R8(reloc_cars + what * CAR_SIZE + CAR_LOCKED)) {
        char s[64];
        snprintf(s, sizeof s, "%.63s", (const char *)gptr(t));
        if (carnames_fit(s)) {
            if (!row_text)
                row_text = galloc(64);
            gstrncpy(row_text, s, 64);
            W32(outp, row_text);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ opponents */
static struct { uint32_t car; uint8_t letter, locked; } *kept;
static int nkept, pick_first, pick_kind;
static uint32_t pick_cars, pick_arg;

static void keep(uint32_t car)
{
    uint32_t e = reloc_cars + car * CAR_SIZE;
    kept[nkept].car = car;
    kept[nkept].letter = (uint8_t)R8(e + CAR_LETTER);
    kept[nkept].locked = (uint8_t)R8(e + CAR_LOCKED);
    nkept++;
}

/* Before 0x45e500(n, kind) runs (opponents.c's hook): 1 if the car list was changed for the
   call (classes_pick_done puts it back). The pool ("class": the player's class; "free": every
   class but ODDITIES and UNLIMITED) is shuffled with the game's own random numbers and cars are
   taken while their paint jobs fit the game's 100 pairs. For the call the cars taken wear the
   player's letter and every other car with it is marked locked; "free" then runs as "class"
   (kind 1), which also gives each opponent a car of its own while there are enough (the game's
   "free" took pairs at random and repeated cars with many paint jobs). */
int classes_pick_prepare(CPU *c)
{
    int kind = (int)R32(c->esp + 8);
    if (!active || (kind != 1 && kind != 2))
        return 0;
    uint32_t n = R32(GAME_CAR_COUNT), me = R32(GAME_CAR_CHOSEN);
    uint8_t mine = me < n ? (uint8_t)R8(reloc_cars + me * CAR_SIZE + CAR_LETTER) : 0;
    if (kind == 1 && mine == 'o')
        return 0;                       /* copies of the player's car, as shipped */
    uint8_t champ[CHAMPS + 1] = {0};
    for (int k = 0; k < CHAMPS; k++)
        champ[k] = (uint8_t)R8(GAME_CHAMPS + k * CHAMP_SIZE);
    uint32_t *pool = malloc((n + 1) * sizeof *pool), npool = 0, pairs = 0, listed = 0, lpairs = 0;
    uint8_t *in = calloc(n + 1, 1);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = reloc_cars + i * CAR_SIZE;
        if (R8(e + CAR_LOCKED) || R8(e + CAR_UNAVAILABLE))
            continue;
        uint8_t l = (uint8_t)R8(e + CAR_LETTER);
        int ok = kind == 1 ? l == mine : l != 'o' && l != 'z' && l != 0;
        int shipped = kind == 1 ? ok : l && memchr(champ, l, CHAMPS) != NULL;
        if (shipped && lpairs < PICK_PAIRS) {       /* what the game alone would have taken */
            listed++;
            lpairs += R32(e + CAR_PAINTS);
        }
        if (ok && R32(e + CAR_PAINTS) > 0) {
            pool[npool++] = i;
            pairs += R32(e + CAR_PAINTS);
        }
    }
    if (kind == 1 && pairs <= PICK_PAIRS) {         /* the game takes them all */
        free(pool);
        free(in);
        return 0;
    }
    for (uint32_t i = npool; i > 1; i--) {          /* a random order */
        uint32_t j = guest_call(c, F_RANDOM, 1, i), t = pool[i - 1];
        pool[i - 1] = pool[j];
        pool[j] = t;
    }
    uint32_t taken = 0, used = 0;
    for (uint32_t k = 0; k < npool; k++) {
        uint32_t p = R32(reloc_cars + pool[k] * CAR_SIZE + CAR_PAINTS);
        if (used + p <= PICK_PAIRS) {
            used += p;
            taken++;
            in[pool[k]] = 1;
        }
    }
    free(pool);
    free(kept);
    kept = malloc((n + 2) * sizeof *kept);
    nkept = 0;
    uint8_t target = mine;
    if (target == 'o' || !target) {     /* "free" for an ODDITIES car: a letter of the race's own */
        target = champ[0];
        keep(me);
        W8(reloc_cars + me * CAR_SIZE + CAR_LETTER, target);
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = reloc_cars + i * CAR_SIZE;
        if (in[i] && R8(e + CAR_LETTER) != target) {
            keep(i);
            W8(e + CAR_LETTER, target);
        } else if (!in[i] && R8(e + CAR_LETTER) == target && !R8(e + CAR_LOCKED)) {
            keep(i);
            W8(e + CAR_LOCKED, 1);      /* left out of this pick */
        }
    }
    free(in);
    pick_first = (int)R32(GAME_PARTICIPANT_COUNT);
    pick_kind = kind;
    pick_cars = npool;
    pick_arg = c->esp + 8;
    W32(pick_arg, 1);                   /* "free" runs as "class" */
    if (oplog)
        LOG(LOG_ALWAYS, "classes: %s opponents from %u cars (%u car/paint pairs) of %s; the game alone took the first "
            "%u cars; this race draws from %u of them", kind == 1 ? "\"class\"" : "\"free\"", npool, pairs,
            kind == 1 ? (classes_name(mine) ? classes_name(mine) : "the player's class") : "every class but ODDITIES and UNLIMITED",
            listed, taken);
    return 1;
}

void classes_pick_done(CPU *c)
{
    (void)c;
    W32(pick_arg, (uint32_t)pick_kind);
    for (int k = nkept - 1; k >= 0; k--) {
        uint32_t e = reloc_cars + kept[k].car * CAR_SIZE;
        W8(e + CAR_LETTER, kept[k].letter);
        W8(e + CAR_LOCKED, kept[k].locked);
    }
    nkept = 0;
    if (!oplog)
        return;
    /* each opponent's place in its pool's list order (the game alone reached only the first ones) */
    uint32_t n = R32(GAME_CAR_COUNT), me = R32(GAME_CAR_CHOSEN), last = R32(GAME_PARTICIPANT_COUNT);
    uint8_t mine = me < n ? (uint8_t)R8(reloc_cars + me * CAR_SIZE + CAR_LETTER) : 0;
    char line[1024] = "";
    for (uint32_t p = (uint32_t)pick_first; p < last && p < 16; p++) {
        uint32_t car = R32(GAME_PARTICIPANTS + p * PART_SIZE + 0x54), rank = 0;
        for (uint32_t i = 0; i < n && i <= car; i++) {
            uint32_t e = reloc_cars + i * CAR_SIZE;
            uint8_t l = (uint8_t)R8(e + CAR_LETTER);
            if (!R8(e + CAR_LOCKED) && !R8(e + CAR_UNAVAILABLE) &&
                (pick_kind == 1 ? l == mine : l != 'o' && l != 'z' && l != 0))
                rank++;
        }
        size_t k = strlen(line);
        snprintf(line + k, sizeof line - k, "%s%s (#%u of %u)", k ? ", " : "",
                 car < n ? (const char *)gptr(reloc_cars + car * CAR_SIZE + CAR_FILE) : "?", rank, pick_cars);
    }
    LOG(LOG_ALWAYS, "classes: opponents: %s", line);
}

/* ------------------------------------------------------------------ the vehicle screen */
/* The menu script (data\menu.cfg, as the Enhanced options and opponents.c serve it from cache\)
   is served again from cache\classes\ with:
   - the vehicle screen's panel headed by the class's name instead of "Vehicle:" ("Class: "
     in that font would push the longer names into the menu beside it);
   - its FACTORY STATISTICS in the smaller font 18 (more lines fit beside the car): the game's
     words with the measured top speed and 0-100 km/h beside them, stability, the engine's power
     against the stock cars', mass, drive and gears (the game's own MV_VSTAT_GEARS, unused as
     shipped), and the car's ranks in its class;
   - the championship event screen "Class: <name>" under the car's name. */
static int (*prev_open)(CPU *c), (*prev_vars)(CPU *c);
static struct { char in[96]; uint32_t name; int tried; } served[4];

typedef struct { const char *at, *text; size_t cut; } Edit;

static int edit_cmp(const void *a, const void *b)
{
    const Edit *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}

/* the start of the line holding `what`, inside the block that starts with `block`, before the
   next top-level block */
static const char *find_in(const char *text, const char *block, const char *what)
{
    const char *b = strstr(text, block);
    if (!b) return NULL;
    const char *end = strstr(b + 1, "\nblock ");
    const char *w = strstr(b, what);
    if (!w || (end && w > end)) return NULL;
    while (w > text && w[-1] != '\n') w--;
    return w;
}

static const char *line_end(const char *p)
{
    const char *e = strchr(p, '\n');
    return e ? e + 1 : p + strlen(p);
}

static const char stats_block[] =
    "\t\tblock text carstats\r\n\t\t{\r\n"
    "\t\t\tpos -94 11\r\n\t\t\ttab 38 36\r\n\t\t\tfont text 18\r\n"
    "\t\t\ttext \"Top speed:\t$MV_VSTAT_TOPSPEED:statistics\t$MV_CS_TOP\"\r\n"
    "\t\t\ttext \"Acceleration:\t$MV_VSTAT_ACCELERATION:statistics\t$MV_CS_ACC\"\r\n"
    "\t\t\ttext \"Stability:\t$MV_VSTAT_STABILITY:statistics\"\r\n"
    "\t\t\ttext \"$MV_CS_POWER\"\r\n"
    "\t\t\ttext \"Mass:\t$MV_VSTAT_MASS KG\"\r\n"
    "\t\t\ttext \"Drive:\t$MV_VSTAT_DRIVE:wheeldrive\t$MV_VSTAT_GEARS GEARS\"\r\n"
    "\t\t\ttext \"$MV_CS_RANK\"\r\n"
    "\t\t\tblock state in\r\n\t\t\t\t{\r\n\t\t\t\tpos -120 11\r\n\t\t\t\t}\r\n"
    "\t\t\tblock state out\r\n\t\t\t\t{\r\n\t\t\t\tpos -140 11\r\n\t\t\t\t}\r\n"
    "\t\t}\r\n";

static char *patch_menu(const char *in)
{
    Edit e[8];
    int k = 0;
    /* FACTORY STATISTICS: the lines after the title go, the block of ours comes before the
       panel's own "Vehicle:" block (inside the same block, so it moves with it) */
    const char *first = find_in(in, "block menu vehiclesetmenu", "text \" \"");
    const char *last = find_in(in, "block menu vehiclesetmenu", "text \"Drive:\t$MV_VSTAT_DRIVE:wheeldrive\"");
    const char *inner = find_in(in, "block menu vehiclesetmenu", "block text vehisetting\r\n\t\t{");
    if (!inner) inner = find_in(in, "block menu vehiclesetmenu", "block text vehisetting\n\t\t{");
    const char *head = find_in(in, "block menu vehiclesetmenu", "text \"Vehicle:\"");
    if (!first || !last || !inner || !head || !(first < last && last < inner && inner < head)) {
        LOG(LOG_ALWAYS, "classes: the vehicle screen is not as expected; no class or measured lines");
        return NULL;
    }
    e[k++] = (Edit){first, "", (size_t)(line_end(last) - first)};
    e[k++] = (Edit){inner, stats_block, 0};
    const char *v = strstr(head, "Vehicle:");
    e[k++] = (Edit){v, "$MV_CS_CLASS", 8};          /* the class (upper case) over the car's name */
    /* the championship event screen: the class under the car's name */
    const char *name = find_in(in, "block text championshiptextxxxx", "text \"$MV_MYVEHICLENAME\"");
    if (name) {
        static char line[96];
        const char *t = strstr(name, "text");
        snprintf(line, sizeof line, "%.*stext \"Class: $MV_CS_CLASS\"\r\n", (int)(t - name), name);
        e[k++] = (Edit){line_end(name), line, 0};
    }
    qsort(e, (size_t)k, sizeof *e, edit_cmp);
    size_t n = strlen(in), extra = 0;
    for (int i = 0; i < k; i++) extra += strlen(e[i].text);
    char *out = malloc(n + extra + 1), *o = out;
    const char *p = in;
    for (int i = 0; i < k; i++) {
        memcpy(o, p, (size_t)(e[i].at - p)); o += e[i].at - p;
        memcpy(o, e[i].text, strlen(e[i].text)); o += strlen(e[i].text);
        p = e[i].at + e[i].cut;
    }
    memcpy(o, p, strlen(p) + 1);
    return out;
}

/* 0x44c110(name, &size): after the Enhanced options' and opponents.c's (they serve the menu
   script from cache\), before the button prompts' */
static int open_hook(CPU *c)
{
    if (prev_open && prev_open(c))
        return 1;
    uint32_t a = R32(c->esp + 4);
    if (!a)
        return 0;
    const char *name = (const char *)gptr(a), *base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    if (strncasecmp(name, "cache\\", 6) || !strncasecmp(name, "cache\\classes", 13) ||
        strncasecmp(base, "menu", 4) || strcasecmp(base + strlen(base) - 4, ".cfg") ||
        (strcasecmp(base, "menu.cfg") && strncasecmp(base, "menu-", 5)))
        return 0;
    int k = 0;
    while (k < 4 && served[k].tried && strcasecmp(served[k].in, name)) k++;
    if (k == 4)
        return 0;
    if (!served[k].tried) {
        served[k].tried = 1;
        snprintf(served[k].in, sizeof served[k].in, "%s", name);
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
            out = patch_menu(in);
        }
        snprintf(path, sizeof path, "%s/cache/classes", user_dir);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/cache/classes/menu-%d.cfg", user_dir, k);
        FILE *w = out ? fopen(path, "wb") : NULL;
        if (w) {
            int ok = fwrite(out, 1, strlen(out), w) == strlen(out);
            if (fclose(w) == 0 && ok) {
                char win[64];
                snprintf(win, sizeof win, "cache\\classes\\menu-%d.cfg", k);
                served[k].name = gstrdup(win);
            }
        }
        free(in);
        free(out);
    }
    if (served[k].name)
        W32(c->esp + 4, served[k].name);
    return 0;
}

/* ------------------------------------------------------------------ the numbers */
static const char *ordinal(int n, char *buf, size_t sz)
{
    const char *s = (n % 100 >= 11 && n % 100 <= 13) ? "TH" : n % 10 == 1 ? "ST" : n % 10 == 2 ? "ND" :
                    n % 10 == 3 ? "RD" : "TH";
    snprintf(buf, sz, "%d%s", n, s);
    return buf;
}

/* the chosen car (by its file name, as the screen shows it) and its table entry */
static uint32_t chosen_car(void)
{
    uint32_t n = R32(GAME_CAR_COUNT), i = R32(GAME_CAR_CHOSEN);
    const char *name = (const char *)gptr(GAME_CAR_NAME);
    if (i < n && !strcasecmp((const char *)gptr(reloc_cars + i * CAR_SIZE + CAR_FILE), name))
        return i;
    for (i = 0; i < n; i++)
        if (!strcasecmp((const char *)gptr(reloc_cars + i * CAR_SIZE + CAR_FILE), name))
            return i;
    return R32(GAME_CAR_CHOSEN);
}

static Entry *chosen_entry(void)
{
    uint32_t i = chosen_car();
    return i < R32(GAME_CAR_COUNT) ? table_entry((const char *)gptr(reloc_cars + i * CAR_SIZE + CAR_FILE)) : NULL;
}

enum { V_CLASS, V_TOP, V_ACC, V_POWER, V_RANK, NVARS };
static uint32_t var_text[NVARS];

static uint32_t var_now(int v)
{
    char t[128] = "", a[8], b[8];
    uint32_t car = chosen_car();
    Entry *e = chosen_entry();
    switch (v) {
    case V_CLASS: {
        const char *s = classes_car_name(car);
        snprintf(t, sizeof t, "%s", s ? s : "");
        break;
    }
    case V_TOP:
        if (e && e->top >= 0) snprintf(t, sizeof t, "%.0f KM/H", e->top);
        break;
    case V_ACC:
        if (e && e->t100 >= 0) snprintf(t, sizeof t, "0-100 IN %.1f S", e->t100);
        else if (e && e->t50 >= 0) snprintf(t, sizeof t, "0-50 IN %.1f S", e->t50);
        break;
    case V_POWER:
        if (e && e->power >= 0) {
            if (power_lo >= 0) snprintf(t, sizeof t, "Power:\t%.1f\tSTOCK %.1f-%.1f", e->power, power_lo, power_hi);
            else snprintf(t, sizeof t, "Power:\t%.1f", e->power);
        }
        break;
    case V_RANK: {
        if (!e || e->top < 0 || car >= R32(GAME_CAR_COUNT))
            break;
        uint8_t l = (uint8_t)R8(reloc_cars + car * CAR_SIZE + CAR_LETTER);
        uint32_t n = R32(GAME_CAR_COUNT);
        int of = 0, rtop = 1, racc = 1;
        for (uint32_t i = 0; i < n; i++) {
            if (R8(reloc_cars + i * CAR_SIZE + CAR_LETTER) != l) continue;
            Entry *o = table_entry((const char *)gptr(reloc_cars + i * CAR_SIZE + CAR_FILE));
            if (!o || o->top < 0) continue;
            of++;
            if (o == e) continue;
            rtop += o->top > e->top;
            /* 0-100: quicker times first; a car that never reached 100 km/h after all that did */
            if (o->t100 >= 0 && (e->t100 < 0 || o->t100 < e->t100)) racc++;
        }
        if (e->t100 >= 0)
            snprintf(t, sizeof t, "In class:\tTOP SPEED %s, 0-100 %s OF %d", ordinal(rtop, a, sizeof a),
                     ordinal(racc, b, sizeof b), of);
        else
            snprintf(t, sizeof t, "In class:\tTOP SPEED %s OF %d", ordinal(rtop, a, sizeof a), of);
        break;
    }
    }
    gstrncpy(var_text[v], t, 128);
    return var_text[v];
}

#define VSLOT(n) METHOD(cs_var_##n, 0) { return var_now(n); }
VSLOT(0) VSLOT(1) VSLOT(2) VSLOT(3) VSLOT(4)
static void (*const var_fns[NVARS])(CPU *) = {cs_var_0, cs_var_1, cs_var_2, cs_var_3, cs_var_4};
static const char *const var_names[NVARS] = {"MV_CS_CLASS", "MV_CS_TOP", "MV_CS_ACC", "MV_CS_POWER", "MV_CS_RANK"};
static uint32_t vars_first, vars_last;

/* 0x47cff0(menu): ours in front of the variables linked into the menu */
static int vars_hook(CPU *c)
{
    uint32_t menu = R32(c->esp + 4);
    if (prev_vars)
        prev_vars(c);
    else
        F_0047cff0_orig(c);
    if (!vars_first) {
        uint32_t zero = galloc(4), prev = 0;
        W32(zero, 0);
        for (int i = 0; i < NVARS; i++) {
            uint32_t e = galloc(36);
            var_text[i] = galloc(128);
            W8(var_text[i], 0);
            const uint32_t v[9] = {0, gstrdup(var_names[i]), thunk_addr(var_fns[i], var_names[i]), 0x100, zero, zero,
                                   0, 0, 0};
            for (int k = 0; k < 9; k++)
                W32(e + 4 * k, v[k]);
            if (prev) W32(prev, e); else vars_first = e;
            prev = e;
        }
        vars_last = prev;
    }
    W32(vars_last, R32(menu + 0x10));
    W32(menu + 0x10, vars_first);
    return 1;
}

/* after opponents_menus_init, before prompts_init (both add to the file and menu hooks) */
void classes_menus_init(void)
{
    if (!enabled)
        return;
    hook_car_row = car_row_hook;
    hooked = 1;
    prev_open = hook_idf_open;
    hook_idf_open = open_hook;
    prev_vars = hook_menu_vars;
    hook_menu_vars = vars_hook;
}
