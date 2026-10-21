/* The letters' font (Enhanced option "font": original, 1nsanefont, mirabel).

   The game draws all its text, menus and races alike, from one 256 x 256 picture of letters
   (data\abc.pcx, its alpha data\abc.acx) and a table of their boxes in it (data\abc.pos, read into
   the program at FONT_TABLE: 20 bytes a code, {x, y, w, h, y offset} 16.16): each letter is a quad
   showing its box, and a box's width is the letter's advance. Stretched to today's screens the
   picture is soft.

   With a font chosen, the picture is drawn again from the font file, ATLAS_SCALE times larger: each
   letter the game has a box for, in that box, at the game's size (26 pixels per em at the picture's
   scale, its baseline 19 - y offset pixels down the box, the pen at the box's left edge), white with
   the coverage as alpha. The renderer (vk.c) uses it, with its own mipmaps, wherever the game uses
   the picture (the surface font_frame marks), so the game still measures, places, colours and fades
   every letter. The fonts are built into the program (assets/1nsanefont):
   - 1nsaneFont: Mirabel Gothish adjusted to the game's letters (its README), spaced by the game's
     boxes, so it is drawn as it is;
   - Mirabel Gothish as it was: each letter centred across where 1nsaneFont's is, on the same
     baseline; what it lacks (the arrows, the eject mark, the square, the Hungarian letters) from
     1nsaneFont.
   A few codes draw other characters than Windows-1252 says (SPECIAL). The game's Ê sits a pixel above
   the line in its picture; here it is on the line.

   Off in --test runs unless INSANE_ENHANCED is set, and without FreeType (HAVE_FREETYPE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "gfx.h"
#include "font.h"

#define FONT_TEX        0x1c54c90u      /* the letters' texture (+0x7c its surface), 0 when not loaded */
#define FONT_TABLE      0x4cdf24u       /* -> the letters' boxes, 20 bytes per code */
#define ATLAS_SCALE     8               /* the drawn picture's pixels per the game's */
#define PICTURE         256             /* the game's picture's size */
#define EM              26.0            /* the fonts' em in the picture's pixels */

static int allowed, choice;             /* choice: 0 original, 1 1nsaneFont, 2 Mirabel Gothish */
static uint8_t *atlas;                  /* the drawn picture: alpha, then shade (ATLAS_SCALE * PICTURE square each) */
static uint8_t *old_atlas;              /* the one before (the renderer may still be reading it) */
static uint32_t atlas_gen;
static int built;                       /* the choice the atlas was drawn for, -1 none */
static Surface *marked;

static const char *const choices[] = {"original", "1nsanefont", "mirabel"};

static void parse(void)
{
    const char *v = option_str("font", "original");
    choice = 0;
    for (int k = 1; k < 3; k++)
        if (!strcasecmp(v, choices[k]))
            choice = k;
}

#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include "font_1nsane_otf.h"            /* generated from assets/1nsanefont/1nsaneFont.otf */
#include "font_mirabel_otf.h"           /* ... and MirabelGothish.otf */

/* the codes that draw another character than Windows-1252 has there */
static const struct { uint8_t code; uint32_t ch; } SPECIAL[] = {
    {0x3c, 0x25C0}, {0x3e, 0x25B6}, {0x5e, 0x23CF}, {0x7f, 0x25A0}, {0x9c, 0x0214}, {0xf7, 0x0215},
    {0xf8, 0x0151}, {0xfd, 0x0171}, {0xfe, 0x0170}, {0xff, 0x0150}};
/* Windows-1252's 0x80-0x9f */
static const uint16_t CP1252_HI[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

static uint32_t char_of(int code)
{
    for (size_t k = 0; k < sizeof SPECIAL / sizeof SPECIAL[0]; k++)
        if (SPECIAL[k].code == code)
            return SPECIAL[k].ch;
    if (code >= 0x80 && code < 0xa0)
        return CP1252_HI[code - 0x80];
    return (uint32_t)code;
}

static FT_Library ft;
static FT_Face face_1nsane, face_mirabel;

static int faces(void)
{
    static int tried, ok;
    if (tried)
        return ok;
    tried = 1;
    ok = !FT_Init_FreeType(&ft) &&
         !FT_New_Memory_Face(ft, font_1nsane_otf, (FT_Long)sizeof font_1nsane_otf, 0, &face_1nsane) &&
         !FT_New_Memory_Face(ft, font_mirabel_otf, (FT_Long)sizeof font_mirabel_otf, 0, &face_mirabel) &&
         !FT_Set_Char_Size(face_1nsane, 0, (FT_F26Dot6)(EM * ATLAS_SCALE * 64), 72, 72) &&
         !FT_Set_Char_Size(face_mirabel, 0, (FT_F26Dot6)(EM * ATLAS_SCALE * 64), 72, 72);
    if (!ok)
        LOG(LOG_ALWAYS, "font: FreeType could not open the built-in fonts; the game's letters stay");
    return ok;
}

/* a glyph's ink, in pixels from its pen on the baseline (up positive): x0 x1 y0 y1 (all 0 if none) */
static void ink(FT_Face f, FT_UInt g, double *box)
{
    box[0] = box[1] = box[2] = box[3] = 0;
    if (FT_Load_Glyph(f, g, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) || f->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        return;
    FT_BBox b;
    FT_Outline_Get_CBox(&f->glyph->outline, &b);
    box[0] = b.xMin / 64.0, box[1] = b.xMax / 64.0, box[2] = b.yMin / 64.0, box[3] = b.yMax / 64.0;
}

/* one letter into the atlas: its glyph with the pen at (px, py) atlas pixels (sub-pixel), clipped to
   its box [bx0, bx1) x [by0, by1) */
static void draw_glyph(FT_Face f, FT_UInt g, double px, double py, int bx0, int by0, int bx1, int by1, int n)
{
    FT_Vector d = {(FT_Pos)((px - (int)px) * 64), (FT_Pos)(-(py - (int)py) * 64)};
    FT_Set_Transform(f, NULL, &d);
    int err = FT_Load_Glyph(f, g, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_RENDER);
    FT_Set_Transform(f, NULL, NULL);
    if (err)
        return;
    FT_Bitmap *bm = &f->glyph->bitmap;
    int x0 = (int)px + f->glyph->bitmap_left, y0 = (int)py - f->glyph->bitmap_top;
    for (unsigned r = 0; r < bm->rows; r++) {
        int y = y0 + (int)r;
        if (y < by0 || y >= by1 || y < 0 || y >= n)
            continue;
        for (unsigned k = 0; k < bm->width; k++) {
            int x = x0 + (int)k;
            if (x < bx0 || x >= bx1 || x < 0 || x >= n)
                continue;
            uint8_t a = bm->buffer[r * (unsigned)bm->pitch + k];
            uint8_t *o = &atlas[(size_t)y * n + x];
            if (a > *o)
                *o = a;
        }
    }
}

/* The letters' shade: the game's picture has a black outline round each white letter, under its soft
   edge, so its letters darken toward their edges over about a picture pixel (most on the races' big
   lap numbers). Here the same: the coverage averaged over a picture pixel's width (separable box). */
static void shade(const uint8_t *cov, uint8_t *out, int n)
{
    const int r = ATLAS_SCALE / 2;
    uint16_t *row = malloc(sizeof *row * (size_t)n * n);
    for (int y = 0; y < n; y++) {
        int sum = 0;
        for (int x = -r; x < n + r; x++) {
            if (x + r < n) sum += cov[(size_t)y * n + x + r];
            if (x - r - 1 >= 0) sum -= cov[(size_t)y * n + x - r - 1];
            if (x >= 0 && x < n) row[(size_t)y * n + x] = (uint16_t)sum;
        }
    }
    const int w = (2 * r + 1) * (2 * r + 1);
    for (int x = 0; x < n; x++) {
        int sum = 0;
        for (int y = -r; y < n + r; y++) {
            if (y + r < n) sum += row[(size_t)(y + r) * n + x];
            if (y - r - 1 >= 0) sum -= row[(size_t)(y - r - 1) * n + x];
            if (y >= 0 && y < n) out[(size_t)y * n + x] = (uint8_t)((sum + w / 2) / w);
        }
    }
    free(row);
}

/* the picture drawn from the chosen font, each letter in the game's box for it */
static void build(void)
{
    uint32_t tab = R32(FONT_TABLE);
    if (!tab || !faces())
        return;
    const int n = PICTURE * ATLAS_SCALE;
    uint8_t *a = calloc((size_t)n * n, 2);
    if (!a)
        return;
    free(old_atlas);
    old_atlas = atlas;
    atlas = a;
    int drawn = 0, lent = 0;
    for (int code = 0; code < 256; code++) {
        const uint32_t e = tab + 20 * (uint32_t)code;
        double x = (int32_t)R32(e) / 65536.0, y = (int32_t)R32(e + 4) / 65536.0;
        double w = (int32_t)R32(e + 8) / 65536.0, h = (int32_t)R32(e + 12) / 65536.0;
        double yoff = (int32_t)R32(e + 16) / 65536.0;
        if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > PICTURE || y + h > PICTURE)
            continue;                   /* (no letter, or a button icon's record: prompts.c) */
        uint32_t ch = char_of(code);
        FT_UInt g1 = FT_Get_Char_Index(face_1nsane, ch);
        if (!g1)
            continue;
        FT_Face f = face_1nsane;
        FT_UInt g = g1;
        double pen = x * ATLAS_SCALE, base = (y + 19.0 - yoff) * ATLAS_SCALE;
        if (choice == 2) {
            FT_UInt gm = FT_Get_Char_Index(face_mirabel, ch);
            if (gm) {
                /* centred across on 1nsaneFont's letter; and up or down on it where the box would cut it
                   (the game's underscore and ¿ sit higher than Mirabel's) */
                double b1[4], bm[4];
                ink(face_1nsane, g1, b1);
                ink(face_mirabel, gm, bm);
                pen += (b1[0] + b1[1]) / 2 - (bm[0] + bm[1]) / 2;
                if (base - bm[3] < y * ATLAS_SCALE - 0.5 || base - bm[2] > (y + h) * ATLAS_SCALE + 0.5)
                    base += (bm[2] + bm[3]) / 2 - (b1[2] + b1[3]) / 2;
                f = face_mirabel;
                g = gm;
            } else
                lent++;
        }
        draw_glyph(f, g, pen, base, (int)(x * ATLAS_SCALE), (int)(y * ATLAS_SCALE),
                   (int)((x + w) * ATLAS_SCALE + 0.5), (int)((y + h) * ATLAS_SCALE + 0.5), n);
        drawn++;
    }
    shade(atlas, atlas + (size_t)n * n, n);
    const char *dump = getenv("INSANE_FONT_DUMP");     /* (a .pgm of the letters' coverage, to look at) */
    FILE *df = dump ? fopen(dump, "wb") : NULL;
    if (df) {
        fprintf(df, "P5\n%d %d\n255\n", n, n);
        fwrite(atlas, 1, (size_t)n * n, df);
        fclose(df);
    }
    __atomic_add_fetch(&atlas_gen, 1, __ATOMIC_RELEASE);
    built = choice;
    LOG(LOG_ALWAYS, "font: the letters drawn from %s: %d of them%s, %d x %d", choice == 1 ? "1nsaneFont" : "Mirabel Gothish",
        drawn, lent ? " (some from 1nsaneFont)" : "", n, n);
}
#else
static void build(void) { built = choice; }
#endif

const uint8_t *font_atlas(int *size, uint32_t *gen)
{
    if (!allowed || !choice || built != choice || !atlas)
        return NULL;
    *size = PICTURE * ATLAS_SCALE;
    *gen = __atomic_load_n(&atlas_gen, __ATOMIC_ACQUIRE);
    return atlas;
}

void font_frame(void)
{
    if (!allowed)
        return;
    uint32_t h = R32(FONT_TEX), obj = h ? R32(h + 0x7c) : 0;
    Surface *s = obj ? surface_get(obj) : NULL;
    if (s && s->width != PICTURE)
        s = NULL;
    if (s != marked) {
        if (marked)
            __atomic_store_n(&marked->font_letters, 0, __ATOMIC_RELEASE);
        if (s)
            __atomic_store_n(&s->font_letters, 1, __ATOMIC_RELEASE);
        marked = s;
    }
    if (s && choice && built != choice)
        build();
}

void font_options(void)
{
    if (allowed)
        parse();
}

void font_init(void)
{
    allowed = !deterministic || getenv("INSANE_ENHANCED");
    built = -1;
    if (allowed)
        parse();
#ifndef HAVE_FREETYPE
    allowed = 0;
#endif
}
