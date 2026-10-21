/* Car names with fan content (design point 8, "car names").

   1. Fan cars that share a name. A car's name is its .vht's "name" line, read into the car list
   (+0, 32 bytes) at start-up; the menus, the loading and event screens and the results lists all
   show it from there (Browse rows 0x4721c0, MV_MYVEHICLENAME 0x479ba0, the championship's car
   0x47b4a0, the loading screen 0x46cd00, the results 0x456da0 / 0x4585f0 / 0x459de0 as "%.21s").
   Different downloads often carry the same name (Ant's folders: two "72 BLAZER", two "Jeep YJ",
   two "NemesiS", three "Suzuki Samurai", and a fan "Wrecker" beside the stock bonus car), so
   the lists can't tell them apart. Once the list is complete (mods.c's champdef hook) every fan
   car whose name another car in the list shares (letter case and spaces aside) gets a tag:
   "72 BLAZER (Topless)". Stock cars keep their names (the stock Wrecker stays "Wrecker"). The
   tag comes from the table below for the known ones (keyed to file and name: what tells them
   apart, from the real vehicle, the readme or the download), else from the download's name
   with the car's own name's words, version words and punctuation taken out ("Jeep YJ Wrangler -
   ES.zip" for "Jeep YJ": "Wrangler ES"), else the file name. A tagged name is kept to 21
   letters, what the results lists show: the base name's first words (the make) go first
   ("Suzuki Samurai (Hardtop)" -> "Samurai (Hardtop)"), then the tag is cut. The game's player
   file, registry, replays and championship records keep file names, so nothing saved changes.
   Without fan content nothing is touched.

   2. Long names in Vehicle > Browse (classes.c's row hook): the list's rows start at x 35 of
   640 and the menu beside it at x 352; a fan name wider than that ran into it ("ELGonzO Trophy
   truck F150" over BROWSE, "Series IIB Forward Control" over CHANGE). A row wider than the
   column is cut with "..." (in the menu font's own glyph widths, the game's text routine's
   measure), a tag in brackets kept whole. Only the list's rows: the car's full name stays
   everywhere else. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "funcs.h"
#include "image.h"
#include "mods.h"

#define CAR_SIZE        0x40u           /* car list entry: name +0 (32 bytes), file name +0x20 */
#define CAR_FILE        0x20u
#define NAME_ROOM       31              /* the name's 32 bytes */
#define NAME_SHOWN      21              /* the results lists print "%.21s" (0x4cc5fc) */
#define FONT_TABLE      0x4cdf24u       /* the menu font's glyph records, 20 bytes per code (w at +8, 16.16) */
/* Browse's column in the font's units: at 640 x 480 a row's text takes 0.83 px a unit (measured
   on 33 rows, within 1.5 px), the chosen row's 9% more, from x 35; the menu beside the list starts
   at x 352: 345 units end the chosen row at x 342 */
#define ROW_WIDTH       345.0f

/* the known ones: what tells them apart */
static const struct { const char *file, *name, *tag; } known[] = {
    {"72blaz", "72 BLAZER", "Hardtop"},         /* 72_blazer.zip: lifted, 44" tyres, white hardtop */
    {"72blz", "72 BLAZER", "Topless"},          /* 72blzr.zip: "72 Chevy Blazer Topless" (its readme) */
    {"jyj", "Jeep YJ", "Roof Rack"},            /* "Jeep YJ Wrangler - ES.zip": rack, jerry cans, snorkel, winch */
    {"jyjs", "Jeep YJ", "Basic"},               /* "Jeep YJ Wrangler.zip": the same Jeep without them */
    {"rgh3", "NemesiS", "H3"},                  /* "HUMMER H3.zip": a Hummer H3 prerunner */
    {"scribbit", "NemesiS", "Baja Ranger"},     /* Baja_Ranger_NemesiS.zip: a Ford Ranger prerunner */
    {"htsam", "Suzuki Samurai", "Hardtop"},     /* "Samurai Hardtop - fixed.zip" */
    {"tsam", "Suzuki Samurai", "Soft Top"},     /* "Suzuki Samurai (st).zip": the soft top */
    {"zukmtl", "Suzuki Samurai", "Trail Rig"},  /* "Zuk MT.zip": "Suzuki Samurai_mild trail rig" */
    {"wrek", "Wrecker", "Rollback"},            /* kcrollback.zip: a rollback tow truck */
};

/* words a download's name carries that tell nothing about the car */
static const char *const noise[] = {"beta", "final", "fixed", "fix", "new", "version", "release", "insane", "1nsane",
                                    "car", "by", "the", "and", "for", "zip", "rar", "idf", "v", NULL};

static void key_of(const char *s, char *out, size_t n)  /* lower case, single spaces, trimmed */
{
    size_t k = 0;
    int space = 0;
    for (; *s && k + 1 < n; s++) {
        if (isspace((unsigned char)*s)) {
            space = k > 0;
            continue;
        }
        if (space && k + 2 < n) out[k++] = ' ';
        space = 0;
        out[k++] = (char)tolower((unsigned char)*s);
    }
    out[k] = 0;
}

static int has_word(const char *text, const char *w)    /* `w` a word of `text`, any case */
{
    size_t n = strlen(w);
    for (const char *p = text; *p; p++)
        if (!strncasecmp(p, w, n) && (p == text || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[n]))
            return 1;
    return 0;
}

/* a tag from the download's name: its words less the car's name's, versions and noise */
static void tag_from_download(const char *src, const char *name, const char *file, char *out, size_t n)
{
    const char *b = src ? strrchr(src, '/') : NULL;
    char base[256];
    snprintf(base, sizeof base, "%s", b ? b + 1 : src ? src : "");
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    out[0] = 0;
    size_t k = 0;
    for (char *p = base; *p;) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        char w[64];
        size_t m = 0;
        while (*p && isalnum((unsigned char)*p)) {
            if (m + 1 < sizeof w) w[m++] = *p;
            p++;
        }
        w[m] = 0;
        if (!m || has_word(name, w))
            continue;
        int skip = 0;
        for (int i = 0; noise[i] && !skip; i++)
            skip = !strcasecmp(w, noise[i]);
        if ((w[0] == 'v' || w[0] == 'V') && isdigit((unsigned char)w[1]))
            skip = 1;                                   /* v1, v2b */
        if (skip || k + m + 2 > n || k + m + 1 > 14)    /* (a short tag: the words that fit 14 letters) */
            continue;
        k += (size_t)snprintf(out + k, n - k, "%s%s", k ? " " : "", w);
    }
    if (!out[0])
        snprintf(out, n, "%s", file);
}

/* "base (tag)" in at most NAME_SHOWN letters: the base's first words go, then the tag is cut */
static void tagged(const char *base, const char *tag, char *out, size_t n)
{
    const char *b = base;
    while (*b == ' ') b++;
    for (;;) {
        snprintf(out, n, "%s (%s)", b, tag);
        const char *sp = strchr(b, ' ');
        if (strlen(out) <= NAME_SHOWN || !sp)
            break;
        b = sp + 1;
    }
    if (strlen(out) > NAME_SHOWN) {
        int room = NAME_SHOWN - (int)strlen(b) - 3;
        if (room < 3) {
            snprintf(out, n, "%.*s (%.3s)", NAME_SHOWN - 6, b, tag);
        } else {
            snprintf(out, n, "%s (%.*s)", b, room, tag);
        }
    }
}

void carnames_tag(void)
{
    if (!mods_active)
        return;
    uint32_t n = R32(GAME_CAR_COUNT);
    char (*keys)[40] = calloc(n + 1, sizeof *keys);
    for (uint32_t i = 0; i < n; i++)
        key_of((const char *)gptr(reloc_cars + i * CAR_SIZE), keys[i], sizeof keys[i]);
    int tagged_n = 0;
    char *done = calloc(n + 1, 1);
    for (uint32_t i = 0; i < n; i++) {
        if (done[i] || !keys[i][0])
            continue;
        uint32_t same = 0;
        for (uint32_t j = i; j < n; j++)
            same += !strcmp(keys[i], keys[j]);
        if (same < 2)
            continue;
        /* the group: tags for its fan cars, then made unique */
        char names[16][NAME_ROOM + 1];
        uint32_t who[16];
        int m = 0;
        for (uint32_t j = i; j < n && m < 16; j++) {
            if (strcmp(keys[i], keys[j]))
                continue;
            done[j] = 1;
            uint32_t e = reloc_cars + j * CAR_SIZE;
            char name[NAME_ROOM + 1], file[16];
            snprintf(name, sizeof name, "%s", (const char *)gptr(e));
            for (size_t l = strlen(name); l && isspace((unsigned char)name[l - 1]);)
                name[--l] = 0;                          /* (the .vht's line end: "72 BLAZER\r") */
            snprintf(file, sizeof file, "%s", (const char *)gptr(e + CAR_FILE));
            const char *src = mods_download(0, file);
            if (!src)
                continue;                               /* a stock car keeps its name */
            char tag[NAME_ROOM + 1] = "", kk[40];
            for (size_t k = 0; k < sizeof known / sizeof *known && !tag[0]; k++) {
                key_of(known[k].name, kk, sizeof kk);
                if (!strcasecmp(known[k].file, file) && !strcmp(kk, keys[j]))
                    snprintf(tag, sizeof tag, "%s", known[k].tag);
            }
            if (!tag[0])
                tag_from_download(src, name, file, tag, sizeof tag);
            tagged(name, tag, names[m], sizeof names[m]);
            for (int k = 0; k < m; k++)
                if (!strcasecmp(names[k], names[m]))     /* the same tag twice: the file names */
                    tagged(name, file, names[m], sizeof names[m]);
            who[m++] = j;
        }
        for (int k = 0; k < m; k++) {
            uint32_t e = reloc_cars + who[k] * CAR_SIZE;
            LOG(LOG_ALWAYS, "carnames: %s shares its name with %u other car(s): shown as \"%s\"",
                (const char *)gptr(e + CAR_FILE), same - 1, names[k]);
            gstrncpy(e, names[k], NAME_ROOM + 1);
            tagged_n++;
        }
    }
    free(keys);
    free(done);
    if (tagged_n)
        logmsg(LOG_ALWAYS, "carnames: %d fan cars that share a name with another car tagged", tagged_n);
}

/* ------------------------------------------------------------------ Browse's rows */
static float glyphs(const char *s)
{
    uint32_t tab = R32(FONT_TABLE);
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (tab) {
            int32_t gw = (int32_t)R32(tab + 20u * *p + 8);
            if (gw > 0) w += gw / 65536.0f;
        } else {
            w += isupper(*p) || isdigit(*p) ? 21.0f : *p == ' ' ? 17.5f : 15.5f;   /* (no font: about) */
        }
    }
    return w;
}

/* `text` (room for 64) cut to Browse's column; 1 if it was */
int carnames_fit(char *text)
{
    if (!mods_active || glyphs(text) <= ROW_WIDTH)
        return 0;
    /* a tag in brackets at the end stays whole */
    char head[64], tail[64] = "";
    snprintf(head, sizeof head, "%s", text);
    size_t k = strlen(head);
    while (k && isspace((unsigned char)head[k - 1]))
        head[--k] = 0;
    char *open = k && head[k - 1] == ')' ? strrchr(head, '(') : NULL;
    if (open && open > head && open[-1] == ' ') {
        snprintf(tail, sizeof tail, " %s", open);
        open[-1] = 0;
    }
    char out[160];
    for (k = strlen(head); k > 0; k--) {
        snprintf(out, sizeof out, "%.*s...%s", (int)k, head, tail);
        if (glyphs(out) <= ROW_WIDTH)
            break;
    }
    /* whole words before the dots ("Alfa Romeo GTA...", not "GTA 130..."), unless that leaves
       less than 6 letters; nor a space or a dash */
    size_t w = k;
    while (w > 0 && head[w - 1] != ' ') w--;
    if (head[k] && head[k] != ' ' && w >= 6)
        k = w;
    while (k > 0 && (head[k - 1] == ' ' || head[k - 1] == '-' || head[k - 1] == ','))
        k--;
    snprintf(text, 64, "%.*s...%s", (int)k, head, tail);
    return 1;
}
