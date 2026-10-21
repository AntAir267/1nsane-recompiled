/* Button prompts and control hints (design point 5): Xbox button icons wherever the game names
   a key or says what to press while a controller is the device in use, the game's own key
   names otherwise; and, in races, reminders of the controls besides driving.

   Icons are extra characters of the menu font. The font is one 256 x 256 picture (data\abc.pcx)
   and a table of glyph records {x, y, w, h, y offset} (16.16, data\abc.pos) that every text
   routine reads; each glyph goes to the renderer through one routine (0x429d80) into a batch of
   quads whose render states name the font's texture, drawn when a 2D section ends. The picture
   is full and its texture coordinates are fixed to its 256 pixels, so the icons
   (assets/kenney-input-prompts/xbox_prompts.png, written to <user>\cache\prompts.png) are a
   texture of their own: codes 0x80..0x9b (0x8c is French Œ) get the records of a glyph a little
   taller than the capitals, placed far outside the font picture; those quads go to a second
   batch with the icon texture, drawn right after the font's. So the game itself measures,
   centres and places icons like letters, and they take the text's fade, its order and the
   widescreen HUD placement. An icon in black text (the HUD's drop shadows) stays a silhouette.

   Which: the device used last (a key or the mouse, or a controller's button or stick), or the
   player's choice (option "prompts": auto, keyboard, xbox). Texts follow it every frame:
   - the menus' hint line ("Up/Down to select menu, ...", menu.cfg), the credits' "press ENTER"
     and lines we add (the same hint line over the in-race menu, "Press any key to start" on the
     instruction screens) become menu variables (type 0x100: a function giving the text);
   - texts the game looks up by name (press_results, pressanykey, the demo's line) are swapped
     as it looks them up (0x4708e0);
   each in the game's language: its own words for the keys become icons ("Up/Down", "Enter",
   "any key", "Haut/bas", "EINGABETASTE", "INTRO", ...). Binding lists show what a control is
   bound to: a controller's button or axis as its icon, always (0x470cb0 names a code, 0x47d010
   an axis entry, 0x470d60 quotes them), which also fixes the game's rotated D-pad names.

   Control hints (option "control_hints": useful, always, off) are drawn after the race HUD
   (0x463bd0, inside widescreen.c's wrap so they go to the screen edge) in the HUD's own panel
   picture and text size, at the right above the dashboard: ROLLOVER while the car lies on its
   side or roof (the game's own test, 0x463b00), RESCUE when it is stuck (that test's "hanging"
   answer, flooded, or no progress under throttle), REPAIR for a while after a hard crash
   (sudden loss of speed; the game keeps no damage figure, it dents the model), or, with
   "always", a list of the controls besides driving. Each shows the key or button it is bound
   to. Off where the Enhanced options are (not in --test runs without INSANE_ENHANCED). */
#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "runtime.h"
#include "funcs.h"
#include "image.h"
#include "input.h"
#include "mods.h"
#include "options.h"
#include "pad.h"
#include "widescreen.h"
#include "xbox_prompts_png.h"           /* generated from assets/kenney-input-prompts/xbox_prompts.png */

/* game addresses (GOG build) */
#define RENDERER        0xa3ca88u       /* the renderer object (a pointer) */
#define R_BATCH         0x7b24u         /* ... its 2D quad batch (the font's) */
#define R_FUNCS         0x6c64u         /* ... its table of drawing functions */
#define B_N1            0x04u           /* batch: quads queued for the screen-space buffer */
#define B_N0            0x0cu           /* ... for the other buffer */
#define B_COLOR         0x10u
#define B_NSTATES       0x1cu           /* ... render state pairs follow at +0x20 */
#define FONT_TEX        0x1c54c90u      /* the menu font's texture, 0 when not loaded */
#define FONT_TABLE      0x4cdf24u       /* the current font's glyph records, 20 bytes per code */
#define FONT_SX         0x4cdf1cu       /* text width scale (0x481300's second argument, float) */
#define FONT_H          0x1ca5e18u      /* the font's height (float) */
#define TEXT_SCALE      0x4cdf0cu       /* screen units per text unit (float) */
#define IDF_COUNT       0x1b84b90u      /* mounted archive directory entries */
#define PLAYER          0x1c54d7cu      /* the local player */
#define PLAYER_BLOCK    0x1c4d988u      /* 0x4d8 per player: +0 record, +0x60 repair, +0x64 rescue countdown */
#define HUD_TEX         0x1c54c80u      /* the HUD's picture */
#define HUD_PANEL_UV    0x4c5d80u       /* the panel in it (the opponent table's), 4 floats */
#define FLOODED         0x1c54e24u      /* the HUD's flooded message is up */
#define ROLL_HELP       0x1c54e20u      /* the game's own rollover help is up */
#define PLAYER_FINISHED 0x2ccu          /* player block: over the line */
#define F_MENU_ACTION   0x473150u       /* (menu, "block:in" ...): runs a menu script action */
#define F_TEX_LOAD      0x4487b0u       /* (flags, name, 1, 1) -> texture; tries name.png first */
#define F_TEX_FREE      0x4491e0u
#define F_BATCH_NEW     0x429bf0u       /* (quads, state pairs, pairs) -> the current batch */
#define F_BATCH_FREE    0x429d20u       /* frees the current batch */
#define F_FLUSH         0x42a020u       /* (buffer, 5): draws a batch's queued quads */
#define F_2D_BEGIN      0x41fb20u
#define F_2D_END        0x41fb80u
#define F_PANEL         0x41bbe0u       /* (x0, y0, x1, y1, u0, v0, u1, v1, texture, colour) */
#define F_TEXT_STYLE    0x481300u       /* (colour, width scale, height scale, font) */
#define F_TEXT          0x481380u       /* (x, y, text) */
#define F_KEY_NAME      0x470cb0u       /* (menu, code, out) */
#define F_TEXT_LOOKUP   0x4708e0u       /* (array name, index) -> text */
#define F_UPSIDE_DOWN   0x463b00u       /* (player record) -> 1 on its side or roof, 2 hanging */

static int enabled;
static uint32_t scratch;                /* program memory for our strings and arguments */

/* ------------------------------------------------------------------ which device */
static int xbox(void)
{
    const char *m = option_str("prompts", "auto");
    if (!strcasecmp(m, "xbox")) return 1;
    if (!strcasecmp(m, "keyboard")) return 0;
    return input_controller_last();
}

/* ------------------------------------------------------------------ icon glyphs */
enum { IC_A, IC_B, IC_X, IC_Y, IC_LB, IC_RB, IC_LT, IC_RT, IC_VIEW, IC_MENU, IC_LS, IC_RS,
       IC_DUP, IC_DDOWN, IC_DLEFT, IC_DRIGHT, IC_DPAD, IC_DPAD_V, IC_DPAD_H, IC_LSTICK, IC_LSTICK_H,
       IC_LSTICK_V, IC_RSTICK, IC_RSTICK_H, IC_RSTICK_V, IC_GUIDE, IC_SHARE, NICONS };
#define ICON_COLS 8
#define ICON_ROWS 4
#define ICON_X0 4096                    /* where icon k's record says it is: x = ICON_X0 + 64 k */
#define ICON_STEP 64

static char icon_char(int k) { return (char)(k < 12 ? 0x80 + k : 0x81 + k); }

/* a joystick code (260 + button, 276.. D-pad) -> icon, -1 if none */
static int code_icon(uint32_t code)
{
    static const int8_t buttons[16] = {IC_A, IC_B, IC_X, IC_Y, IC_VIEW, IC_GUIDE, IC_MENU, IC_LS, IC_RS,
                                       IC_LB, IC_RB, IC_SHARE, -1, -1, -1, -1};
    static const int8_t dpad[4] = {IC_DUP, IC_DRIGHT, IC_DDOWN, IC_DLEFT};    /* names L R U D in the game */
    if (code >= 260 && code < 276) return buttons[code - 260];
    if (code >= 276 && code < 280) return dpad[code - 276];
    return -1;
}

static uint32_t ours, ours_font;        /* our batch (icon texture) and the font batch it goes with */
static uint32_t icon_tex, icon_name;
static int in_ours;

static uint32_t slot_addr(void) { return R32(RENDERER) + R_BATCH; }

static void drop_ours(CPU *c)
{
    if (ours) {
        uint32_t slot = slot_addr(), keep = R32(slot);
        W32(slot, ours);
        guest_call(c, F_BATCH_FREE, 0);
        W32(slot, keep);
        ours = ours_font = 0;
    }
    if (icon_tex) {
        guest_call(c, F_TEX_FREE, 1, icon_tex);
        icon_tex = 0;
    }
}

/* our batch: the font batch's render states with the icon texture */
static int ensure_ours(CPU *c, uint32_t font)
{
    if (ours && ours_font == font)
        return 1;
    drop_ours(c);
    icon_tex = guest_call(c, F_TEX_LOAD, 4, 1, icon_name, 1, 1);
    uint32_t n = R32(font + B_NSTATES), tex = R32(FONT_TEX);
    if (!icon_tex || n > 16)
        return 0;
    uint32_t pairs = scratch;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t st = R32(font + 0x20 + 8 * k), v = R32(font + 0x24 + 8 * k);
        W32(pairs + 8 * k, st);
        W32(pairs + 8 * k + 4, v == tex ? icon_tex : v);
    }
    uint32_t slot = slot_addr();
    guest_call(c, F_BATCH_NEW, 3, 0x80, n, pairs);
    ours = R32(slot);
    W32(slot, font);
    ours_font = ours ? font : 0;
    LOG(LOG_DRAW, "prompts: icon texture %08x, batch %08x", icon_tex, ours);
    return ours != 0;
}

/* 0x429d80(screen quad, texture quad, buffer): a glyph quad. Icons go to our batch. */
static int quad_hook(CPU *c)
{
    uint32_t uv = R32(c->esp + 8);
    if (in_ours || !uv)
        return 0;
    uint32_t px = R32(uv) >> 16;
    if (px < ICON_X0 || px >= ICON_X0 + ICON_STEP * NICONS)
        return 0;
    int k = (int)(px - ICON_X0) / ICON_STEP;
    uint32_t slot = slot_addr(), font = R32(slot);
    if (!font || !ensure_ours(c, font)) {
        c->ret = R32(c->esp);                   /* no icon texture: leave the quad out */
        c->esp += 4;
        return 1;
    }
    uint32_t col = R32(font + B_COLOR);
    unsigned lum = ((col >> 16) & 255) + ((col >> 8) & 255) + (col & 255);
    W32(ours + B_COLOR, lum < 96 ? col : (col & 0xff000000u) | 0xffffffu);
    double cw = 1.0 / ICON_COLS, ch = 1.0 / ICON_ROWS, in = 0.5 / 512;   /* half a texel of the 512 x 256 picture */
    uint32_t u0 = (uint32_t)(((k % ICON_COLS) * cw + in) * 16777216.0), u1 = (uint32_t)(((k % ICON_COLS + 1) * cw - in) * 16777216.0);
    uint32_t v0 = (uint32_t)(((k / ICON_COLS) * ch + in) * 16777216.0), v1 = (uint32_t)(((k / ICON_COLS + 1) * ch - in) * 16777216.0);
    const uint32_t q[8] = {u0, v0, u0, v1, u1, v0, u1, v1};    /* the text routines' vertex order */
    for (int i = 0; i < 8; i++)
        W32(uv + 4 * i, q[i]);
    W32(slot, ours);
    in_ours = 1;
    F_00429d80_orig(c);
    in_ours = 0;
    W32(slot, font);
    return 1;
}

/* 0x42a020(buffer, 5): draws the font batch; our icons right after it */
static int flush_hook(CPU *c)
{
    if (in_ours || !ours)
        return 0;
    uint32_t buf = R32(c->esp + 4), arg = R32(c->esp + 8);
    F_0042a020_orig(c);
    uint32_t slot = slot_addr(), font = R32(slot);
    if (font && font == ours_font && R32(ours + (buf ? B_N1 : B_N0))) {
        W32(slot, ours);
        in_ours = 1;
        guest_call(c, F_FLUSH, 2, buf, arg);
        in_ours = 0;
        W32(slot, font);
    }
    return 1;
}

/* 0x4812d0: the font is unloaded (between menus and races): our texture and batch go too */
static int font_unload_hook(CPU *c)
{
    drop_ours(c);
    return 0;
}

/* data\abc.pos with icon records added */
static char *patch_pos(const char *in, uint32_t size, uint32_t *out_size)
{
    if (size < 0x1400)
        return NULL;
    char *out = malloc(size);
    memcpy(out, in, size);
    for (int k = 0; k < NICONS; k++) {
        int32_t r[5] = {(ICON_X0 + ICON_STEP * k) << 16, ICON_X0 << 16, 26 << 16, 26 << 16, -(7 << 15)};
        memcpy(out + 20 * (uint8_t)icon_char(k), r, sizeof r);
    }
    *out_size = size;
    return out;
}

/* ------------------------------------------------------------------ texts */
/* each language's words for keys -> icons (cp1252, as the game's files) */
static const struct { const char *words; int icon; } key_words[] = {
    {"Up/Down", IC_DPAD_V}, {"Left/Right", IC_DPAD_H}, {"any key", IC_A}, {"Enter", IC_A}, {"ENTER", IC_A},
    {"Haut/bas", IC_DPAD_V}, {"gauche/droite", IC_DPAD_H}, {"une touche", IC_A}, {"Entr\xe9" "e", IC_A},
    {"ENTREE", IC_A},
    {"Pfeil Obn/Unten", IC_DPAD_V}, {"Lks/Rchts", IC_DPAD_H}, {"beliebige Taste", IC_A},
    {"Beliebige Taste", IC_A}, {"Eingabetaste", IC_A}, {"EINGABETASTE", IC_A},
    {"Cursor arriba/abajo", IC_DPAD_V}, {"Izda/Dcha", IC_DPAD_H}, {"cualquier tecla", IC_A}, {"INTRO", IC_A},
    {"Su/gi\xf9", IC_DPAD_V}, {"sinistra/destra", IC_DPAD_H}, {"qualsiasi pulsante", IC_A}, {"un tasto", IC_A},
    {"Invio", IC_A}, {"INVIO", IC_A},
};

/* the text with its key words as icons; 0 if it names no key */
static int to_xbox(const char *in, char *out, size_t n)
{
    size_t k = 0;
    int hit = 0;
    while (*in && k + 2 < n) {
        size_t i, len = 0;
        for (i = 0; i < sizeof key_words / sizeof *key_words; i++) {
            len = strlen(key_words[i].words);
            if (!strncmp(in, key_words[i].words, len))
                break;
        }
        if (i < sizeof key_words / sizeof *key_words) {
            out[k++] = icon_char(key_words[i].icon);
            in += len;
            hit = 1;
        } else {
            out[k++] = *in++;
        }
    }
    out[k] = 0;
    return hit;
}

/* texts that become menu variables (MV_PRM_n) */
enum { T_HINT, T_CREDITS, T_START, NTEXTS };
static char *texts[NTEXTS];             /* the language's own (keyboard) text; T_START: looked up */
static uint32_t text_buf[NTEXTS];

static uint32_t text_now(CPU *c, int i)
{
    char kb[512], xb[512];
    if (i == T_START) {
        uint32_t name = scratch + 0x100;
        gstrncpy(name, "pressanykey", 32);
        snprintf(kb, sizeof kb, "%s", (const char *)gptr(guest_call(c, F_TEXT_LOOKUP, 2, name, 0)));
        gstrncpy(text_buf[i], kb, 512);                 /* the lookup already gave the device's text */
        return text_buf[i];
    }
    snprintf(kb, sizeof kb, "%s", texts[i] ? texts[i] : "");
    int pad = xbox();
    const char *browse = i == T_HINT ? picker_hint(pad) : NULL;    /* the long car and map lists */
    if (browse) {
        /* the controller's shoulder buttons as icons */
        size_t k = 0;
        for (const char *s = browse; *s && k + 2 < sizeof kb; s++) {
            if (!strncmp(s, "[LB]", 4) || !strncmp(s, "[RB]", 4)) {
                kb[k++] = icon_char(s[1] == 'L' ? IC_LB : IC_RB);
                s += 3;
            } else {
                kb[k++] = *s;
            }
        }
        kb[k] = 0;
    }
    gstrncpy(text_buf[i], pad && to_xbox(kb, xb, sizeof xb) ? xb : kb, 512);
    return text_buf[i];
}

#define TSLOT(n) METHOD(prm_##n, 0) { return text_now(c, n); }
TSLOT(0) TSLOT(1) TSLOT(2)
static void (*const text_fns[NTEXTS])(CPU *) = {prm_0, prm_1, prm_2};
static const char *const text_vars[NTEXTS] = {"MV_PRM_HINT", "MV_PRM_CREDITS", "MV_PRM_START"};

static uint32_t vars_first, vars_last;
static int (*prev_vars)(CPU *c);

/* 0x47cff0(menu): our variables in front of the ones linked into it */
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
        for (int i = 0; i < NTEXTS; i++) {
            uint32_t e = galloc(36), name = galloc(32);
            gstrncpy(name, text_vars[i], 32);
            text_buf[i] = galloc(512);
            W8(text_buf[i], 0);
            const uint32_t v[9] = {0, name, thunk_addr(text_fns[i], text_vars[i]), 0x100, zero, zero, 0, 0, 0};
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

/* 0x4708e0(name, index): texts the game looks up while drawing */
static int lookup_hook(CPU *c)
{
    static const struct { const char *name; uint32_t index; } swap[] = {
        {"press_results", 0}, {"pressanykey", 0}, {"demotext", 2}};
    static uint32_t bufs[3];
    const char *name = gstr(R32(c->esp + 4));
    uint32_t index = R32(c->esp + 8);
    F_004708e0_orig(c);
    for (int k = 0; name && k < 3; k++)
        if (!strcmp(name, swap[k].name) && index == swap[k].index && xbox()) {
            char xb[512];
            if (!to_xbox((const char *)gptr(c->eax), xb, sizeof xb))
                break;
            if (!bufs[k]) bufs[k] = galloc(512);
            gstrncpy(bufs[k], xb, 512);
            c->eax = bufs[k];
        }
    return 1;
}

/* 0x470cb0(menu, code, out): a binding's name; a controller's buttons as icons */
static int key_name_hook(CPU *c)
{
    int k = code_icon(R32(c->esp + 8));
    if (k < 0)
        return 0;
    char s[2] = {icon_char(k), 0};
    gstrncpy(R32(c->esp + 12), s, 2);
    c->eax = 1;
    c->ret = R32(c->esp);
    c->esp += 4;
    return 1;
}

/* 0x47d010(menu, axis entry) -> its text; a controller's sticks and triggers as icons */
static int axis_name_hook(CPU *c)
{
    static const int8_t axes[6] = {IC_LSTICK_H, IC_LSTICK_V, IC_LT, IC_RT, IC_RSTICK_H, IC_RSTICK_V};
    uint32_t e = R32(c->esp + 8);
    F_0047d010_orig(c);
    if (!e || !c->eax)
        return 1;
    char s[8];
    uint32_t type = R32(e) & 0xff, a = R32(e + 4), b = R32(e + 8);
    if (type == 1 && a == 0 && b < 6) {
        snprintf(s, sizeof s, "%c", icon_char(axes[b]));
    } else if (type == 0 && code_icon(a) >= 0 && (!b || code_icon(b) >= 0)) {
        if (b) snprintf(s, sizeof s, "%c / %c", icon_char(code_icon(a)), icon_char(code_icon(b)));
        else snprintf(s, sizeof s, "%c", icon_char(code_icon(a)));
    } else {
        return 1;
    }
    gstrncpy(c->eax, s, sizeof s);
    return 1;
}

/* 0x470d60(menu, variable, out): a menu variable's text; the quotes the binding lists put
   around a name are left off an icon */
static int mv_text_hook(CPU *c)
{
    uint32_t out = R32(c->esp + 12);
    F_00470d60_orig(c);
    char *s = gptr(out);
    size_t n = out ? strlen(s) : 0;
    if (n < 3 || s[0] != '\'' || s[n - 1] != '\'')
        return 1;
    for (size_t i = 1; i + 1 < n; i++)
        if ((unsigned char)s[i] < 0x80 && s[i] != ' ' && s[i] != '/')
            return 1;
    memmove(s, s + 1, n - 2);
    s[n - 2] = 0;
    return 1;
}

/* ------------------------------------------------------------------ control hints */
enum { H_ROLLOVER, H_RESCUE, H_REPAIR, H_CAMERA, H_REARVIEW, H_MAP, NHINTS };
static const struct { int layout_button; int game_key; const char *label; } hints[NHINTS] = {
    {7, -1, "ROLLOVER"}, {2, -1, "RESCUE"}, {3, -1, "REPAIR"},
    {-1, 17, "CAMERA"}, {-1, 18, "REAR VIEW"}, {-1, 22, "BIG MAP"}};
static char hint_label[NHINTS][32];     /* from the language's controls menu where it has them */

/* ------------------------------------------------------------------ the scripts */
typedef struct { char *p; size_t n, cap; } Buf;
static void put(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

/* the text of a `text "..."` line (NULL if the line is not one) */
static char *line_text(const char *line, const char *end)
{
    const char *t = line;
    while (t < end && (*t == ' ' || *t == '\t')) t++;
    if (end - t < 6 || strncmp(t, "text \"", 6)) return NULL;
    const char *a = t + 6, *z = memchr(a, '"', (size_t)(end - a));
    return z ? strndup(a, (size_t)(z - a)) : NULL;
}

/* The in-race menu's hint line: centred along the top edge, above the lap counter, in the
   script's own hint font (set 11, used by nothing else there) at the main menus' size, so its
   drop shadow is the game's own (+0.01 units, set 11's shadowcolor). It sits on a dark band like
   the pause menu's own panel: over the bare sky that shadow, about a letter's stroke away at
   this small size, read as a second, doubled line of text. One rect (the menus' rects meet with
   a pixel's overlap or gap, depending on the resolution). */
static const char hint_font[] = "font set 11 size 1.3 1.7";
static const char hint_block[] =
    "\tblock rect prompthintcsik\r\n\t{\r\n\t\tpos -100 -100\r\n\t\tsize 200 8\r\n\t\tpri -30\r\n"
    "\t\tcolor 0 0 0 0\r\n\t\talpha 170 170 170 170\r\n\t}\r\n"
    "\tblock text prompthint\r\n\t{\r\n\t\tjustify center\r\n\t\tpos 0 -98\r\n\t\tfont text 11\r\n"
    "\t\ttext \"$MV_PRM_HINT\"\r\n\t}\r\n";
static const char start_block[] =
    "\tblock text promptstart\r\n\t{\r\n\t\tjustify center\r\n\t\tpos -5 77\r\n\t\tfont text 13\r\n"
    "\t\ttext \"$MV_PRM_START\"\r\n\t}\r\n";

/* Line by line: the hint line and the credits' lines naming a key become variables; the in-race
   menu gets the hint line, each instruction screen a "press any key" line. */
static char *patch_script(const char *file, const char *in)
{
    Buf b = {0};
    int is_menu = !strcasecmp(file, "menu.cfg"), is_ingame = !strcasecmp(file, "menu_ingame.cfg");
    int is_credits = !strcasecmp(file, "menu_credits.cfg");
    const char *block = "";             /* the top-level block we are in */
    char blockname[64] = "";
    int depth = 0, changed = 0;
    for (const char *p = in; *p;) {
        const char *e = strchr(p, '\n');
        e = e ? e + 1 : p + strlen(p);
        if (is_ingame && !strncmp(p, "font set 11 size ", 17)) {     /* the hint font, narrower */
            put(&b, hint_font, strlen(hint_font));
            const char *rest = p + 17;
            while (*rest && *rest != ' ') rest++;               /* past the width */
            while (*rest == ' ') rest++;
            while (*rest && *rest != ' ') rest++;               /* past the height */
            put(&b, rest, (size_t)(e - rest));
            p = e;
            continue;
        }
        if (!strncmp(p, "block ", 6)) {
            block = p;
            sscanf(p, "block %*s %63s", blockname);
        }
        for (const char *q = p; q < e && *q != ';' && *q != '"'; q++) {
            if (*q == '{') depth++;
            if (*q == '}' && --depth == 0) {
                /* closing a top-level block: add our lines */
                if (is_ingame && !strcmp(blockname, "mainmenu")) { put(&b, hint_block, strlen(hint_block)); changed = 1; }
                if (is_ingame && !strncmp(blockname, "instruct_", 9)) { put(&b, start_block, strlen(start_block)); changed = 1; }
            }
        }
        (void)block;
        if (is_menu) {
            static const struct { const char *var; int hint; } labels[] = {
                {"MV_CTRL_BUTTON_RESPAWN", H_ROLLOVER}, {"MV_CTRL_BUTTON_RESCUE", H_RESCUE},
                {"MV_CTRL_BUTTON_REPAIR", H_REPAIR}, {"MV_CTRL_BUTTON_CAMCYCLE", H_CAMERA},
                {"MV_CTRL_BUTTON_REARVIEW", H_REARVIEW}, {"MV_CTRL_BUTTON_BIGMAP", H_MAP}};
            const char *mp = p;
            while (*mp == ' ' || *mp == '\t') mp++;
            for (size_t k = 0; !strncmp(mp, "mp \"", 4) && k < sizeof labels / sizeof *labels; k++) {
                const char *d = strstr(mp, labels[k].var), *q = strchr(mp + 4, '"');
                if (d && d < e && q && q < d && !hint_label[labels[k].hint][0])
                    snprintf(hint_label[labels[k].hint], sizeof hint_label[0], "%.*s", (int)(q - mp - 4), mp + 4);
            }
        }
        char *t = line_text(p, e);
        int which = -1;
        if (t && is_menu && !strcmp(blockname, "hintsinmenu1") && !texts[T_HINT]) which = T_HINT;
        if (t && is_credits) {
            char xb[512];
            if (to_xbox(t, xb, sizeof xb) && !texts[T_CREDITS]) which = T_CREDITS;
        }
        if (which >= 0) {
            texts[which] = t;
            const char *ind = p;
            while (*ind == ' ' || *ind == '\t') ind++;
            put(&b, p, (size_t)(ind - p));
            char line[64];
            snprintf(line, sizeof line, "text \"$%s\"\r\n", text_vars[which]);
            put(&b, line, strlen(line));
            changed = 1;
        } else {
            free(t);
            put(&b, p, (size_t)(e - p));
        }
        p = e;
    }
    if (!changed) {
        free(b.p);
        return NULL;
    }
    return b.p;
}

/* START CAMERA (camera.c) on DISPLAY OPTIONS: in menu.cfg's displayopmenu and menu_ingame.cfg's
   displaymenu a line before BACK, its value after the last value, the panel a line taller (and
   room for the eleventh line in the pause menu's). Left as it was if anything is not found. */
static char *insert_at(char *s, const char *at, const char *text)
{
    size_t pre = (size_t)(at - s), n = strlen(text), len = strlen(s);
    char *r = malloc(len + n + 1);
    memcpy(r, s, pre);
    memcpy(r + pre, text, n);
    memcpy(r + pre + n, s + pre, len - pre + 1);
    free(s);
    return r;
}

static char *camera_lines(const char *file, char *s)
{
    int ingame = !strcasecmp(file, "menu_ingame.cfg");
    if (!s || (!ingame && strcasecmp(file, "menu.cfg")))
        return s;
    const char *block = ingame ? "block menu displaymenu" : "block menu displayopmenu";
    const char *last = ingame ? "\"$MV_DIAL_MAPSCALE:mapscale\"" : "\"$MV_DIAL_NAMES:onoff\"";
    const char *size = ingame ? "size  200 120" : "size 200 107", *bigger = ingame ? "size  200 132" : "size 200 119";
    char *b = strstr(s, block), *end = b ? strstr(b + 1, "\nblock menu ") : NULL;
    char *back = b ? strstr(b, "\tmp \"BACK\"") : NULL, *val = b ? strstr(b, last) : NULL;
    char *sz = b ? strstr(b, size) : NULL, *open = b ? strchr(b, '\n') : NULL;
    if (!end) end = s + strlen(s);
    if (!back || !val || !sz || !open || back > end || val > end || sz > end) {
        LOG(LOG_ALWAYS, "prompts: %s's display options are not as expected; no START CAMERA line", file);
        return s;
    }
    size_t at_back = (size_t)(back - s), at_sz = (size_t)(sz - s), at_open = (size_t)(open - s);
    char *val_end = strchr(val, '\n');
    size_t at_val = val_end ? (size_t)(val_end + 1 - s) : strlen(s);
    char indent[16] = "";                   /* the value lines' own indentation */
    const char *ls = val;
    while (ls > s && ls[-1] != '\n') ls--;
    snprintf(indent, sizeof indent, "%.*s", (int)(strchr(ls, 't') - ls) < 15 ? (int)(strchr(ls, 't') - ls) : 0, ls);
    char text[80];
    snprintf(text, sizeof text, "%stext \"$MV_CAM_START\"\r\n", indent);
    /* back to front, so the earlier offsets stay right */
    memcpy(s + at_sz, bigger, strlen(bigger));
    s = insert_at(s, s + at_val, text);
    s = insert_at(s, s + at_back, "\tmp \"START CAMERA\" /select MV_CAM_START\r\n");
    if (ingame)
        s = insert_at(s, strchr(s + at_open + 1, '\n') + 1, "\tmaxlines 12\r\n");
    return s;
}

/* an archive member as the game finds it (directory entries {name[52], archive, offset, size}) */
static char *idf_read(const char *want, uint32_t *size)
{
    uint32_t n = R32(IDF_COUNT);
    for (uint32_t i = 0; i < n && i < limit_idf_entries; i++) {
        uint32_t e = reloc_idf_entries + 64 * i;
        if (strcasecmp((const char *)gptr(e), want)) continue;
        char host[1200];
        if (!path_to_host((const char *)gptr(reloc_idf_archives + 100 * R32(e + 0x34)), host, sizeof host, 0))
            return NULL;
        FILE *f = fopen(host, "rb");
        if (!f) return NULL;
        uint32_t off = R32(e + 0x38), sz = R32(e + 0x3c);
        char *buf = malloc(sz + 1);
        int ok = fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, sz, f) == sz;
        fclose(f);
        if (!ok) { free(buf); return NULL; }
        buf[sz] = 0;
        *size = sz;
        return buf;
    }
    return NULL;
}

/* what the game would open under this name: a loose file (the user folder's cache\ copies of
   the Enhanced options' patches) or the archive member */
static char *game_file(const char *name, uint32_t *size)
{
    char host[1300];
    if (!strncasecmp(name, "cache\\", 6) && path_to_host(name, host, sizeof host, 0)) {
        FILE *f = fopen(host, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)n + 1);
        int ok = n >= 0 && fread(buf, 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        if (!ok) { free(buf); return NULL; }
        buf[n] = 0;
        *size = (uint32_t)n;
        return buf;
    }
    return idf_read(name, size);
}

static int write_cache(const char *file, const void *data, size_t n)
{
    char path[1300];
    snprintf(path, sizeof path, "%s/cache", user_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/cache/%s", user_dir, file);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static struct { const char *file, *ours; uint32_t name; int tried; } served[] = {
    {"abc.pos", "prompts-abc.pos", 0, 0}, {"menu.cfg", "prompts-menu.cfg", 0, 0},
    {"menu_ingame.cfg", "prompts-menu_ingame.cfg", 0, 0}, {"menu_credits.cfg", "prompts-menu_credits.cfg", 0, 0}};
static int (*prev_open)(CPU *c);

/* The menu script as other parts serve it in its place (cache\opponents\menu-N.cfg, the
   STOCK CARS and OPPONENTS lines; cache\classes\menu-N.cfg, the class line): patched the same
   way, once for each such name. */
static struct { char in[96]; uint32_t name; int tried; } variants[8];

static int menu_variant(CPU *c, const char *name, const char *base)
{
    size_t k = strlen(base);
    if (strncasecmp(name, "cache\\", 6) || strncasecmp(base, "menu-", 5) || k < 9 || strcasecmp(base + k - 4, ".cfg"))
        return 0;
    int v = 0;
    while (v < 8 && variants[v].tried && strcasecmp(variants[v].in, name)) v++;
    if (v == 8)
        return 1;
    if (!variants[v].tried) {
        variants[v].tried = 1;
        snprintf(variants[v].in, sizeof variants[v].in, "%s", name);
        uint32_t size = 0;
        char *orig = game_file(name, &size), *out = orig ? camera_lines("menu.cfg", patch_script("menu.cfg", orig)) : NULL;
        char ours[96];
        snprintf(ours, sizeof ours, "prompts-%s", name + 6);  /* "prompts-opponents-menu-2.cfg" */
        for (char *p = ours; *p; p++)
            if (*p == '\\') *p = '-';
        if (out && write_cache(ours, out, strlen(out))) {
            char win[112];
            snprintf(win, sizeof win, "cache\\%s", ours);
            variants[v].name = gstrdup(win);
            LOG(LOG_FILE, "prompts: %s served as %s", name, win);
        }
        free(orig);
        free(out);
    }
    if (variants[v].name)
        W32(c->esp + 4, variants[v].name);
    return 1;
}

/* 0x44c110(name, &size), after the Enhanced options' own patches and the parts that serve the
   menu script from cache\ (opponents.c, classes.c) */
static int open_hook(CPU *c)
{
    if (prev_open && prev_open(c))
        return 1;
    uint32_t a = R32(c->esp + 4);
    if (!a) return 0;
    const char *name = (const char *)gptr(a), *base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    if (!strncasecmp(base, "prompts-", 8))
        return 0;
    if (menu_variant(c, name, base))
        return 0;
    for (size_t k = 0; k < sizeof served / sizeof *served; k++) {
        if (strcasecmp(base, served[k].file)) continue;
        if (!served[k].tried) {
            served[k].tried = 1;
            uint32_t size = 0, out_size = 0;
            char *orig = game_file(name, &size), *out = NULL;
            if (orig && k == 0) out = patch_pos(orig, size, &out_size);
            else if (orig) {
                out = camera_lines(base, patch_script(base, orig));
                out_size = out ? (uint32_t)strlen(out) : 0;
            }
            if (out && write_cache(served[k].ours, out, out_size)) {
                char win[64];
                snprintf(win, sizeof win, "cache\\%s", served[k].ours);
                served[k].name = gstrdup(win);
                LOG(LOG_FILE, "prompts: %s served as %s", name, win);
            }
            free(orig);
            free(out);
        }
        if (served[k].name)
            W32(c->esp + 4, served[k].name);
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ control hints: in the race */
static int (*prev_hud)(CPU *c);

static double hint_clock(void)
{
    return deterministic ? frame_count * (1000.0 / 60.0) : now_ms();
}

/* the name of what a control is bound to, without the quotes the menus put around it */
static void binding_text(CPU *c, uint32_t code, char *out, size_t n)
{
    uint32_t buf = scratch + 0x200;
    W8(buf, 0);
    guest_call(c, F_KEY_NAME, 3, 0, code, buf);
    const char *s = (const char *)gptr(buf);
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '\'' && s[len - 1] == '\'')
        snprintf(out, n, "%.*s", (int)(len - 2), s + 1);
    else
        snprintf(out, n, "%s", s);
}

/* what a hint's control is bound to on the device the prompts show (padgame.c: with a
   controller connected each device keeps its own bindings) */
static uint32_t hint_code(int h)
{
    return padgame_binding(hints[h].layout_button, hints[h].game_key, xbox());
}

static float fl(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static uint32_t fu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* which hints matter now (bits) */
static unsigned hints_useful(CPU *c)
{
    static double up_since, stuck_since, crash_until, last_t;
    static float speed_hist[30];
    static int nhist;
    uint32_t p = R32(PLAYER), blk = PLAYER_BLOCK + 0x4d8 * p, rec = p < 8 ? R32(blk) : 0;
    double t = hint_clock();
    if (!rec || !R32(rec + 0x98) || R32(blk + 0x60) || R32(blk + 0x64)) {   /* repairing or rescuing */
        up_since = stuck_since = crash_until = 0;
        nhist = 0;
        return 0;
    }
    uint32_t phys = R32(rec + 0x98);
    float speed = fabsf((float)(int32_t)R32(phys + 0x1990) / 65536.0f);
    int state = (int)guest_call(c, F_UPSIDE_DOWN, 1, rec);
    unsigned bits = 0;
    if (state == 1) {
        if (!up_since) up_since = t;
        if (t - up_since > 1000 && !(R32(ROLL_HELP) & 1)) bits |= 1u << H_ROLLOVER;
    } else {
        up_since = 0;
    }
    /* stuck: hanging on something, flooded, or hardly moving under throttle */
    int throttle = (int32_t)R32(GAME_CONTROLS + 4) > 0x400000;
    if (state == 2 || R32(FLOODED) || (throttle && speed < 1.0f)) {
        if (!stuck_since) stuck_since = t;
        if (t - stuck_since > (R32(FLOODED) ? 500 : 4000)) bits |= 1u << H_RESCUE;
    } else {
        stuck_since = 0;
    }
    /* a hard crash: most of a good speed gone within half a second */
    if (t - last_t > 1000.0 / 60.0 - 1) {
        last_t = t;
        memmove(speed_hist + 1, speed_hist, sizeof speed_hist - sizeof *speed_hist);
        speed_hist[0] = speed;
        if (nhist < 30) nhist++;
        for (int i = 1; i < nhist; i++)
            if (speed_hist[i] > 15.0f && speed < speed_hist[i] * 0.35f) {
                crash_until = t + 6000;
                nhist = 1;
                break;
            }
    }
    if (t < crash_until) bits |= 1u << H_REPAIR;
    const char *force = getenv("INSANE_HINTS_FORCE");     /* tests: these states as if detected */
    if (force)
        bits |= (unsigned)strtoul(force, NULL, 0);
    if (getenv("INSANE_HINTS_LOG"))
        LOG(LOG_ALWAYS, "hints %lu: speed %.2f upside %d flooded %u throttle %d -> %x", frame_count, speed, state,
            R32(FLOODED), throttle, bits);
    return bits;
}

/* The player is racing: the controls drive the car (no menu or instruction screen up), in a
   race (race mode 1; a replay is 2, the attract-mode demo 0) after the countdown and before
   the end (state 2; 3 from the finish through "press Enter for results" and the results), and
   the player is not over the line. */
static int racing(void)
{
    uint32_t p = R32(PLAYER);
    return R32(GAME_DRIVING) == 1 && R32(GAME_RACE_MODE) == 1 && R32(GAME_RACE_STATE) == 2 && p < 16 &&
           !R32(PLAYER_BLOCK + 0x4d8 * p + PLAYER_FINISHED);
}

static void draw_hints(CPU *c)
{
    const char *mode = option_str("control_hints", "useful");
    if (!strcasecmp(mode, "off") || !racing())
        return;
    int always = !strcasecmp(mode, "always");
    unsigned useful = hints_useful(c);
    int rows[NHINTS], nrows = 0;
    for (int h = 0; h < NHINTS; h++)
        if (always ? 1 : (useful >> h) & 1)
            rows[nrows++] = h;
    if (!nrows || !R32(HUD_TEX))
        return;
    /* right, above the dashboard: the opponent table's panel and text size */
    const float row = 0.07f, x1 = 0.97f, x0 = 0.55f, y1 = 0.58f, y0 = y1 - row * nrows - 0.03f;
    guest_call(c, F_2D_BEGIN, 1, 0);
    uint32_t funcs = R32(R32(RENDERER) + R_FUNCS);
    if (funcs)
        guest_call(c, R32(funcs + 0x3b4), 2, 2, 1);        /* the blending the HUD panels set */
    guest_call(c, F_PANEL, 10, fu(x0), fu(y0), fu(x1), fu(y1), R32(HUD_PANEL_UV), R32(HUD_PANEL_UV + 4),
               R32(HUD_PANEL_UV + 8), R32(HUD_PANEL_UV + 12), R32(HUD_TEX), 0xa0ffffffu);
    uint32_t text = scratch + 0x300;
    for (int i = 0; i < nrows; i++) {
        int h = rows[i];
        char key[64], line[160];
        binding_text(c, hint_code(h), key, sizeof key);
        int hot = (useful >> h) & 1, icon = (unsigned char)key[0] >= 0x80;
        uint32_t colour = hot && always ? 0xffffff00u : 0xffffffffu;
        float y = y0 + 0.02f + row * i;
        /* an icon a little larger than the squeezed HUD letters (1.0 x 1.4) */
        guest_call(c, F_TEXT_STYLE, 4, colour, fu(icon ? 1.5f : 1.0f), fu(icon ? 1.5f : 1.4f), 0);
        gstrncpy(text, key, 64);
        guest_call(c, F_TEXT, 3, fu(x0 + 0.02f), fu(icon ? y - 0.005f : y), text);
        guest_call(c, F_TEXT_STYLE, 4, colour, fu(1.0f), fu(1.4f), 0);
        snprintf(line, sizeof line, "%s", hint_label[h][0] ? hint_label[h] : hints[h].label);
        /* the label right-aligned in the panel (text width as the game measures it) */
        float w = 0;
        uint32_t tab = R32(FONT_TABLE);
        for (const unsigned char *s = (const unsigned char *)line; *s; s++) {
            int32_t gw = (int32_t)R32(tab + 20u * *s + 8);
            if (gw > 0) w += gw * (fl(FONT_SX) / fl(FONT_H)) / 65536.0f;
        }
        gstrncpy(text, line, 160);
        guest_call(c, F_TEXT, 3, fu(x1 - 0.02f - w * fl(TEXT_SCALE)), fu(y), text);
    }
    guest_call(c, F_2D_END, 0);
}

/* after the race HUD, still counted as the HUD for the widescreen placement */
/* The game's own help texts ("TO ROLLOVER PRESS ...", the flooded one), menu blocks the HUD
   (0x463bd0) brings in while the car lies upside down or in water and takes out again when it
   is back. It only does that while the race runs: from the finish on it skips that part, so a
   car upside down at the end kept the help over "press Enter for results", the results and
   whatever came next; the pause menu and replays showed it too. Outside racing() we take them
   out and leave their flags set, so the HUD does not bring them back; racing again, the flags
   are cleared and the HUD shows them again if the car still needs it. */
static const struct { uint32_t flag; const char *out; } helps[2] = {
    {ROLL_HELP, "help_rollover:out"}, {FLOODED, "help_flooded:out"}};
static uint32_t help_out[2];
static unsigned help_down;              /* bit k: we took helps[k] out */

static void helps_racing(int racing_now)
{
    for (int k = 0; k < 2; k++) {
        unsigned bit = 1u << k;
        if (racing_now) {
            if (help_down & bit)
                W32(helps[k].flag, 0);
            help_down &= ~bit;
        } else if (R32(helps[k].flag) != 1) {
            help_down &= ~bit;              /* the HUD took it out itself */
        }
    }
}

static void helps_take_down(CPU *c, uint32_t menu)
{
    for (int k = 0; k < 2; k++)
        if (R32(helps[k].flag) == 1 && !(help_down >> k & 1)) {
            if (!help_out[k])
                help_out[k] = gstrdup(helps[k].out);
            guest_call(c, F_MENU_ACTION, 2, menu, help_out[k]);
            help_down |= 1u << k;
        }
}

static int hud_hook(CPU *c)
{
    uint32_t menu = R32(c->esp + 4);
    int live = racing();
    helps_racing(live);
    if (prev_hud) prev_hud(c);
    else F_00463bd0_orig(c);
    if (!live)
        helps_take_down(c, menu);
    ws_hud_depth++;
    draw_hints(c);
    ws_hud_depth--;
    return 1;
}

/* ------------------------------------------------------------------ start */
void prompts_init(void)
{
    enabled = !deterministic || getenv("INSANE_ENHANCED");
    if (!enabled)
        return;
    if (!write_cache("prompts.png", xbox_prompts_png, sizeof xbox_prompts_png))
        LOG(LOG_ALWAYS, "prompts: cannot write the icon picture to the user folder");
    scratch = galloc(0x400);
    icon_name = gstrdup("cache\\prompts.pcx");          /* the loader tries .png first */
    prev_open = hook_idf_open;
    hook_idf_open = open_hook;
    prev_vars = hook_menu_vars;
    hook_menu_vars = vars_hook;
    prev_hud = hook_race_hud;
    hook_race_hud = hud_hook;
    hook_text_quad = quad_hook;
    hook_quads_flush = flush_hook;
    hook_font_unload = font_unload_hook;
    hook_text_lookup = lookup_hook;
    hook_key_name = key_name_hook;
    hook_axis_name = axis_name_hook;
    hook_mv_text = mv_text_hook;
}
