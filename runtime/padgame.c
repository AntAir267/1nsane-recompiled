/* Controllers in the game (pad.h, design point 5): the ready-made "XBOX CONTROLLER" layout,
   choosing it when a controller is connected, and menu navigation by controller.

   Layouts. 1NSANE loads its controller layouts from Data\0.ctl, 1.ctl, ... at start-up until one
   is missing (at most 8; 0.ctl "CLASSIC 1" is in Data.idf, MODIFY CONFIG saves the others to
   the user folder). A .ctl is the layout (200 bytes) and its name (32 bytes). The layout:
     6 axis entries of 28 bytes: steer, accelerate, brake, handbrake, head turn, head rise
       +0 type: 0 keys, 1 joystick axis, 2 mouse axis (a dword)
       keys:     +4 code for the + side (left, up, pressed), +8 code for the - side, +16/+20/+24
                 speed per tick towards a pressed side / back from the other side / back to rest
                 (full deflection is 0x7f0000)
       joystick: +4 joystick, +8 axis (X Y Z Rx Ry Rz), +12 bit 0: map the whole range onto
                 0..max (the game's own define sets it for every axis but X and Y, made for
                 throttle levers resting at one end), +16 direction +1/-1, +20 1 = half axis
       mouse:    +4 axis, +16 speed (its sign the direction), +24 speed back to rest
     8 button codes: shift up, shift down, rescue, repair, light, horn, diff lock, rollover.
   Codes: 0-255 keys (DIK), 256-259 mouse buttons, 260 + 24 * j joystick j's 16 buttons, then its
   D-pad up, right, down, left (menu_text.cfg's key names call these L, R, U, D). The layout in
   use is a copy (GAME_LAYOUT, registry Control1); SELECT CONFIG copies the chosen one in and
   shows its index (registry MenuLastCtrlLayout). 24 more codes (GameKeys: pause, camera cycle,
   rear view, big map, ...) belong to no layout.

   With a controller connected at start-up (the game looks for joysticks once, then), the
   runtime adds the XBOX CONTROLLER layout after the player's own (hook on the loader; written
   to the user folder only once the player saves a layout after it, so the indices stay put)
   and keeps two sets of control settings in the registry store: one for sessions with a
   controller, one for sessions without. The first session with a controller selects XBOX
   CONTROLLER (with the game's own analog steering defaults and Start/Y/R3/Back/D-pad right on
   pause/camera/rear view/big map/gate); after that each set is whatever the player last chose
   in sessions of its kind. Without a controller and never having had one, nothing changes.

   Plugged in later. The game looks for joysticks once; a controller connected after that is
   handed to the game's own joystick set-up then (claim_joysticks), and the session becomes one
   with a controller (pad_activate): XBOX CONTROLLER joins the list without being chosen, the
   player's layout stays, and the merge below gives the controller its own controls beside it.
   Unplugged, the game's device reads at rest (the input poll zeroes what it cannot read) and
   the keyboard goes on; one plugged back in, or another swapped in, takes the same joystick.

   Menus. The game's menus already read joystick 1's X/Y (beyond half way), button 0 (A) as
   Enter and button 1 (B) as Escape, once per press. With a controller the runtime adds the
   D-pad, Start (Enter in the main menus, Escape = back/continue inside a race), and repeat
   while a direction is held, all in the game's menu input state. "PRESS ENTER FOR RESULTS"
   reads the Enter key itself: there A or Start set it in the game's pressed-codes array. No
   key presses are made, so nothing reaches a race or the "press the control for ..." prompts.

   Both at once. A race reads one layout and one set of GameKeys, so with XBOX CONTROLLER chosen
   the keyboard did nothing in a race (and with a keyboard layout chosen, the controller did
   nothing). With a controller connected, races also take the other device's own controls: the
   keyboard's (the Keyboard set's layout and keys if made for the keyboard, else CLASSIC 1 and
   the game's keys) beside a controller layout, the controller's (the Controller set's if made
   for one, else XBOX CONTROLLER) beside a keyboard layout. Each axis is worked out for each
   device on its own value and the one further from rest drives (keys keep their ramps, an
   idle stick inside the dead zones gives 0, a released key ramps back on its own); the
   steering's velocity factor follows the device that steers. The other device's buttons and
   GameKeys count as pressed with them. Menus already read both devices. The hooks are set in
   every session but do nothing until a controller has been connected (pad_active). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "funcs.h"
#include "image.h"
#include "input.h"
#include "pad.h"

#define LAYOUT_SIZE 200
#define NAME_SIZE 32
#define MAX_LAYOUTS 8
#define XBOX_NAME "XBOX CONTROLLER"
#define JOY_BUTTON(b) (260 + (b))          /* joystick 1's button b (PAD_A..PAD_RB) */
#define JOY_DPAD(d) (276 + (d))            /* its D-pad: 0 up, 1 right, 2 down, 3 left */

#define GAME_REG "HKLM\\Software\\Codemasters\\Insane"
#define OWN_REG "HKLM\\Software\\1NSANE native\\Controls"
#define OWN_PAD OWN_REG "\\Controller"
#define OWN_KBD OWN_REG "\\Keyboard"

static int pad_mode;            /* a controller was connected at start-up (or since: see pad_activate) */
static int pad_active;          /* a controller has been connected this session: the hooks act */
static int had_pad;             /* the store holds settings from a session with a controller */
static int xbox_index = -1;     /* XBOX CONTROLLER's place in the list, -1 = none */
static int xbox_virtual;        /* ... provided by the runtime, not (yet) a file */

/* ------------------------------------------------------------------ the layout */
typedef struct { uint32_t type, a, b, flags; int32_t s1, s2, s3; } AxisEntry;

static void xbox_layout(uint8_t out[LAYOUT_SIZE])
{
    static const AxisEntry axes[6] = {
        {1, 0, 0, 0, -1, 0, 0},                 /* steer: left stick X (left = +) */
        {1, 0, 3, 0, 1, 1, 0},                  /* accelerate: right trigger (Rx), 0..full */
        {1, 0, 2, 0, 1, 1, 0},                  /* brake / reverse: left trigger (Z) */
        {0, JOY_BUTTON(PAD_A), 0, 0, 0x100000, 0x100000, 0x200000},  /* handbrake: A, as CLASSIC's space */
        {1, 0, 4, 0, -1, 0, 0},                 /* head turn: right stick X (Ry; left = +) */
        {1, 0, 5, 0, -1, 0, 0},                 /* head rise: right stick Y (Rz; up = +) */
    };
    static const uint32_t buttons[8] = {
        JOY_BUTTON(PAD_RB), JOY_BUTTON(PAD_LB),  /* shift up, shift down */
        JOY_DPAD(3), JOY_DPAD(2), JOY_DPAD(0),   /* rescue: left, repair: down, light: up */
        JOY_BUTTON(PAD_X), JOY_BUTTON(PAD_LSTICK), JOY_BUTTON(PAD_B),   /* horn, diff lock, rollover */
    };
    _Static_assert(sizeof axes + sizeof buttons == LAYOUT_SIZE, "layout size");
    memcpy(out, axes, sizeof axes);
    memcpy(out + sizeof axes, buttons, sizeof buttons);
}

/* GameKeys entries a controller gets (the rest stay on the keyboard; free look is not needed
   with head turn/rise on the right stick) */
static const struct { int index; uint32_t code; } pad_game_keys[] = {
    {16, JOY_DPAD(1)},              /* cycle gate: D-pad right */
    {17, JOY_BUTTON(PAD_Y)},        /* camera cycle */
    {18, JOY_BUTTON(PAD_RSTICK)},   /* rear view (held) */
    {19, JOY_BUTTON(PAD_START)},    /* pause (Escape still pauses too) */
    {22, JOY_BUTTON(PAD_BACK)},     /* big map (held) */
};

/* the steering settings the game itself picks for an analog steer axis (ADVANCED CONTROLS,
   RESET TO DEFAULTS), as registry values */
static const struct { const char *name; uint32_t value; } pad_steering[] = {
    {"Steer_PowBase", 0x3f8ccccd},      /* non-linear steering 1.1 (keyboard: 1.46) */
    {"Steer_LockPowBase", 0x3f800000},  /* velocity-steering adjuster 1.0 (keyboard: 6.5) */
    {"Steer_LockMaxSpeed", 0x43af0000}, /* velocity-steering maximum 350 */
    {"DeadZone", 500},                  /* analogue dead zone 5 % */
};

/* Data\N.ctl on disk (user or game folder): 1 and its name if present */
static int disk_layout(int n, char name[NAME_SIZE])
{
    char win[64], host[2048];
    snprintf(win, sizeof win, "%s\\Data\\%d.ctl", virtual_game_root(), n);
    if (!path_to_host(win, host, sizeof host, 0))
        return 0;
    FILE *f = fopen(host, "rb");
    if (!f)
        return 0;
    uint8_t buf[LAYOUT_SIZE + NAME_SIZE] = {0};
    size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    memcpy(name, buf + LAYOUT_SIZE, NAME_SIZE);
    name[NAME_SIZE - 1] = 0;
    return got >= LAYOUT_SIZE;
}

/* The list the game will load: 0 from Data.idf, then files while they exist. Returns the
   first free index (MAX_LAYOUTS if full); sets xbox_index if one of them is XBOX CONTROLLER. */
static int scan_layouts(void)
{
    char name[NAME_SIZE];
    int n = 1;
    for (; n < MAX_LAYOUTS && disk_layout(n, name); n++)
        if (!strcmp(name, XBOX_NAME) && xbox_index < 0)
            xbox_index = n;
    return n;
}

static int layout_by_name(const char *want)
{
    char name[NAME_SIZE];
    if (xbox_index >= 0 && !strcmp(want, XBOX_NAME))
        return xbox_index;
    for (int n = 1; n < MAX_LAYOUTS && disk_layout(n, name); n++)
        if (!strcmp(name, want))
            return n;
    return -1;
}

/* ------------------------------------------------------------------ the two sets of settings */
static const char *const state_values[] = {"MenuLastCtrlLayout", "Control1", "GameKeys", "Steer_PowBase",
                                           "Steer_MaxChange", "Steer_LockPowBase", "Steer_LockMaxSpeed",
                                           "DeadZone", "ForceFeedBack"};
#define NSTATE (int)(sizeof state_values / sizeof *state_values)

static void copy_value(const char *from, const char *to, const char *name)
{
    uint8_t buf[512];
    uint32_t type = 0;
    if (!settings_has(from, name)) {
        settings_delete(to, name);
        return;
    }
    uint32_t n = settings_get(from, name, &type, buf, sizeof buf);
    settings_put(to, name, type, buf, n < sizeof buf ? n : sizeof buf);
}

/* the game's current control settings -> a set; name: the selected layout's, if known */
static void save_set(const char *set, const char *name)
{
    for (int k = 0; k < NSTATE; k++)
        copy_value(GAME_REG, set, state_values[k]);
    if (name)
        settings_put(set, "LayoutName", 1, name, (uint32_t)strlen(name) + 1);
    else
        settings_delete(set, "LayoutName");
    uint32_t one = 1;
    settings_put(set, "Saved", 4, &one, 4);
}

/* a set -> the game's settings, its layout found by name (indices move as layouts are added) */
static void restore_set(const char *set)
{
    for (int k = 0; k < NSTATE; k++)
        copy_value(set, GAME_REG, state_values[k]);
    char name[NAME_SIZE] = "";
    settings_get(set, "LayoutName", NULL, name, NAME_SIZE - 1);
    int idx = name[0] ? layout_by_name(name) : -1;
    if (idx >= 0)
        settings_put(GAME_REG, "MenuLastCtrlLayout", 4, &idx, 4);
}

static void select_xbox(void)
{
    uint8_t lay[LAYOUT_SIZE];
    xbox_layout(lay);
    settings_put(GAME_REG, "Control1", 3, lay, LAYOUT_SIZE);
    if (xbox_index >= 0)
        settings_put(GAME_REG, "MenuLastCtrlLayout", 4, &xbox_index, 4);
    uint32_t keys[24];
    if (settings_get(GAME_REG, "GameKeys", NULL, keys, sizeof keys) != sizeof keys)
        for (int k = 0; k < 24; k++)
            keys[k] = R32(GAME_GAME_KEYS + 4 * k);     /* the game's defaults */
    for (size_t k = 0; k < sizeof pad_game_keys / sizeof *pad_game_keys; k++)
        keys[pad_game_keys[k].index] = pad_game_keys[k].code;
    settings_put(GAME_REG, "GameKeys", 3, keys, sizeof keys);
    for (size_t k = 0; k < sizeof pad_steering / sizeof *pad_steering; k++)
        settings_put(GAME_REG, pad_steering[k].name, 4, &pad_steering[k].value, 4);
}

/* ------------------------------------------------------------------ hooks (with a controller) */
/* layout N (cdecl, [esp+4]) -> 1 if loaded: XBOX CONTROLLER in the first free place */
static int layout_load_hook(CPU *c)
{
    int n = (int)R32(c->esp + 4);
    if (!xbox_virtual || n != xbox_index)
        return 0;
    xbox_layout((uint8_t *)gptr(GAME_LAYOUTS + LAYOUT_SIZE * n));
    memset(gptr(GAME_LAYOUT_NAMES + NAME_SIZE * n), 0, NAME_SIZE);
    memcpy(gptr(GAME_LAYOUT_NAMES + NAME_SIZE * n), XBOX_NAME, sizeof XBOX_NAME);
    LOG(LOG_INPUT, "controller: layout %d is %s", n, XBOX_NAME);
    c->eax = 1;
    c->ret = R32(c->esp);
    c->esp += 4;
    return 1;
}

/* saving layout N: if it comes after the provided XBOX CONTROLLER, that becomes a file first,
   so the next session without a controller still finds layout N */
static int layout_save_hook(CPU *c)
{
    int n = (int)R32(c->esp + 4);
    if (!xbox_virtual || n < xbox_index)
        return 0;
    xbox_virtual = 0;
    if (n == xbox_index)
        return 0;
    char win[64], host[2048];
    snprintf(win, sizeof win, "%s\\Data\\%d.ctl", virtual_game_root(), xbox_index);
    path_to_host(win, host, sizeof host, 1);
    FILE *f = fopen(host, "wb");
    if (f) {
        fwrite(gptr(GAME_LAYOUTS + LAYOUT_SIZE * xbox_index), 1, LAYOUT_SIZE, f);
        fwrite(gptr(GAME_LAYOUT_NAMES + NAME_SIZE * xbox_index), 1, NAME_SIZE, f);
        fclose(f);
    }
    LOG(LOG_INPUT, "controller: layout %d saved, so %s is now %s", n, XBOX_NAME, host);
    return 0;
}

/* After the game binds an axis to a joystick axis ((menu, entry, half, prompt, prompt)): no
   axis of our controllers rests at one end (sticks rest in the middle, triggers at 0), so the
   game's whole-range mapping would leave a stick or trigger half deflected at rest. */
static int define_axis_hook(CPU *c)
{
    uint32_t entry = R32(c->esp + 8);
    F_00477020_orig(c);
    if (R32(entry) == 1 && (R32(entry + 12) & 1)) {
        W32(entry + 12, R32(entry + 12) & ~1u);
        LOG(LOG_INPUT, "controller: axis %u bound without the whole-range mapping", R32(entry + 8));
    }
    return 1;
}

/* menu input: D-pad, Start, repeat */
#define REPEAT_DELAY 400.0
#define REPEAT_RATE 120.0
#define STICK_ON 16384
#define STICK_OFF 12000

static double nav_clock(void)
{
    if (deterministic)
        return frame_count * (1000.0 / 60.0);       /* test runs: frames, not the game's clock */
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int stick_dir(int k, int v)
{
    static int on[4];
    int beyond = k == 0 || k == 2 ? -v : v;        /* up, down, left, right */
    on[k] = on[k] ? beyond > STICK_OFF : beyond > STICK_ON;
    return on[k];
}

static int menu_input_hook(CPU *c)
{
    static struct { int on; double next; } rep[6];
    uint8_t before[6];
    if (!pad_active)
        return 0;
    for (int k = 0; k < 6; k++)
        before[k] = (uint8_t)R8(GAME_MENU_NAV + k);
    F_0044d660_orig(c);
    int lx = pad_axis_value(PAD_LX), ly = pad_axis_value(PAD_LY);
    int start = pad_held(PAD_START), in_race = R32(GAME_RACE_MODE) != 0;
    int held[6] = {
        pad_held(PAD_DUP) | stick_dir(0, ly), pad_held(PAD_DDOWN) | stick_dir(1, ly),
        pad_held(PAD_DLEFT) | stick_dir(2, lx), pad_held(PAD_DRIGHT) | stick_dir(3, lx),
        start && !in_race, start && in_race,
    };
    double now = nav_clock();
    for (int k = 0; k < 6; k++) {
        if (!held[k]) {
            rep[k].on = 0;
            continue;
        }
        uint8_t b = (before[k] & 0xfe) | 1;         /* held; bit 7 = the menu already took it */
        if (!rep[k].on) {
            rep[k].on = 1;
            rep[k].next = now + REPEAT_DELAY;
        } else if (k < 4 && now >= rep[k].next) {
            b = 1;                                  /* a new press for the menu */
            rep[k].next = now + REPEAT_RATE;
        }
        W8(GAME_MENU_NAV + k, b);
    }
    return 1;
}

/* ------------------------------------------------------------------ both devices at once */
#define STEER_FACTOR    0x4c5134u       /* velocity steering: 0x480060 works it out before the controls */
#define STEER_MAXCHANGE 0x4c513cu       /* ADVANCED CONTROLS' steering settings (0x4c5138..0x4c5144) */
#define LOCAL_PLAYER    0x4c502cu       /* the player the layout drives */
#define PLAYER_BLOCKS   0x1c4d988u      /* 0x4d8 per player, +0 its record (+0x98 physics, speed at +0x1990) */
#define F_CONTROL_AXIS  0x441cb0u       /* (ticks, axis entry, value, exponent, maximum change) */
#define PRESSED_END     (0x59u * 4u)    /* the pressed-codes arrays: keys, mouse buttons, 4 joysticks */

/* One device's own controls: its layout, GameKeys and steering settings. */
typedef struct {
    int ready;
    uint8_t layout[LAYOUT_SIZE];
    uint32_t keys[24];
    uint32_t pow, maxchange, lockpow, lockspeed;
} DeviceSet;
static DeviceSet kbd_set, joy_set;
static uint32_t default_keys[24];       /* the game's own GameKeys, before any settings */
static uint32_t default_maxchange;      /* ... and its steering rate limit */
static uint32_t other_layout;           /* guest copy of the other device's layout */
static const DeviceSet *other_now;      /* ... whose layout it holds */

static uint32_t u32_at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int pad_layout(const uint8_t *lay) { return u32_at(lay) == 1 || u32_at(lay + 28) == 1; }
static int pad_code(uint32_t code) { return code >= 260 && code < PRESSED_END; }
static int pad_keys(const uint32_t *keys)
{
    for (int k = 0; k < 24; k++)
        if (pad_code(keys[k]))
            return 1;
    return 0;
}

/* a stored set's steering settings, if it has them */
static void set_steering(const char *set, DeviceSet *d)
{
    settings_get(set, "Steer_PowBase", NULL, &d->pow, 4);
    settings_get(set, "Steer_MaxChange", NULL, &d->maxchange, 4);
    settings_get(set, "Steer_LockPowBase", NULL, &d->lockpow, 4);
    settings_get(set, "Steer_LockMaxSpeed", NULL, &d->lockspeed, 4);
}

/* Each device's own controls, from the set kept for sessions of its kind when that set is made
   for it, else the defaults: the keyboard's CLASSIC 1 (loaded by the game: filled in on first
   use) with the game's GameKeys and keyboard steering, the controller's XBOX CONTROLLER. */
static void sets_init(void)
{
    uint32_t maxchange = default_maxchange;
    other_now = NULL;
    kbd_set = (DeviceSet){0, {0}, {0}, 0x3fbae148, maxchange, 0x40d00000, 0x43af0000};   /* 1.46, 6.5, 350 */
    joy_set = (DeviceSet){1, {0}, {0}, 0x3f8ccccd, maxchange, 0x3f800000, 0x43af0000};   /* 1.1, 1.0, 350 */
    uint8_t lay[LAYOUT_SIZE];
    if (settings_get(OWN_KBD, "Control1", NULL, lay, LAYOUT_SIZE) == LAYOUT_SIZE && !pad_layout(lay)) {
        memcpy(kbd_set.layout, lay, LAYOUT_SIZE);
        kbd_set.ready = 1;
        set_steering(OWN_KBD, &kbd_set);
    }
    if (settings_get(OWN_KBD, "GameKeys", NULL, kbd_set.keys, sizeof kbd_set.keys) != sizeof kbd_set.keys ||
        pad_keys(kbd_set.keys))
        memcpy(kbd_set.keys, default_keys, sizeof default_keys);
    xbox_layout(joy_set.layout);
    if (settings_get(OWN_PAD, "Control1", NULL, lay, LAYOUT_SIZE) == LAYOUT_SIZE && pad_layout(lay)) {
        memcpy(joy_set.layout, lay, LAYOUT_SIZE);
        set_steering(OWN_PAD, &joy_set);
    }
    if (settings_get(OWN_PAD, "GameKeys", NULL, joy_set.keys, sizeof joy_set.keys) != sizeof joy_set.keys ||
        !pad_keys(joy_set.keys)) {
        memcpy(joy_set.keys, default_keys, sizeof default_keys);
        for (size_t k = 0; k < sizeof pad_game_keys / sizeof *pad_game_keys; k++)
            joy_set.keys[pad_game_keys[k].index] = pad_game_keys[k].code;
    }
}

/* the device the game's own layout is not for (NULL if not ready) */
static const DeviceSet *other_set(void)
{
    const DeviceSet *d = pad_layout((const uint8_t *)gptr(GAME_LAYOUT)) ? &kbd_set : &joy_set;
    if (!d->ready) {
        if (R32(GAME_LAYOUT_COUNT) < 1)
            return NULL;
        memcpy(kbd_set.layout, gptr(GAME_LAYOUTS), LAYOUT_SIZE);     /* CLASSIC 1 */
        kbd_set.ready = 1;
    }
    if (!other_layout)
        other_layout = galloc(LAYOUT_SIZE);
    if (other_now != d) {
        memcpy(gptr(other_layout), d->layout, LAYOUT_SIZE);
        other_now = d;
    }
    return d;
}

/* the other device's GameKeys */
static const uint32_t *other_keys(void)
{
    uint32_t keys[24];
    for (int k = 0; k < 24; k++)
        keys[k] = R32(GAME_GAME_KEYS + 4 * k);
    return pad_keys(keys) ? kbd_set.keys : joy_set.keys;
}

static int driving(void) { return R32(GAME_DRIVING) == 1 && R32(GAME_RACE_MODE) != 0; }

static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* 0x480060's velocity steering factor, with a device's own settings */
static uint32_t steer_factor(const DeviceSet *d)
{
    float f = 1.0f;
    int32_t p = (int32_t)R32(LOCAL_PLAYER);
    uint32_t rec = p >= 0 ? R32(PLAYER_BLOCKS + 0x4d8u * (uint32_t)p) : 0, phys = rec ? R32(rec + 0x98) : 0;
    if (phys) {
        double one = fbits(R32(0x4bf36c)), pw = fbits(d->lockpow);
        double v = one - (double)(int32_t)R32(phys + 0x1990) * fbits(R32(0x4bf380)) / fbits(d->lockspeed);
        f = (float)(pw != one ? pow(v, pw) : v);
    }
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* The six axes (0x442190 -> 0x441cb0 for each entry of the layout in use): each device's own
   entry worked out on its own value (keys ramp, sticks are rate-limited from where they were),
   the one further from rest drives. So the other device's idle stick (inside the dead zones)
   or released keys never hold anything, and the steering's velocity factor is the one of the
   device that steers. In races only; elsewhere the game's own layout alone, as shipped. */
static int control_axis_hook(CPU *c)
{
    static int busy;
    static int32_t own[6], other[6], shown[6];
    static unsigned follow;                 /* bit i: own/other[i] gave the game's value */
    static uint32_t factor_game, factor_shown;
    uint32_t entry = R32(c->esp + 8), val = R32(c->esp + 12), off = entry - GAME_LAYOUT;
    if (!pad_active || busy || off >= 6 * 28 || off % 28 || val != GAME_CONTROLS + off / 28 * 4)
        return 0;
    int i = (int)(off / 28);
    const DeviceSet *d = driving() ? other_set() : NULL;
    if (!d) {
        follow &= ~(1u << i);
        return 0;
    }
    if (!(follow >> i & 1) || (int32_t)R32(val) != shown[i]) {
        own[i] = (int32_t)R32(val);         /* the game set it (free look, a new race): start from there */
        other[i] = 0;
        follow |= 1u << i;
    }
    uint32_t ticks = R32(c->esp + 4), expo = R32(c->esp + 16), maxchange = R32(c->esp + 20);
    busy = 1;
    W32(val, (uint32_t)own[i]);
    guest_call(c, F_CONTROL_AXIS, 5, ticks, entry, val, expo, maxchange);
    own[i] = (int32_t)R32(val);
    W32(val, (uint32_t)other[i]);
    guest_call(c, F_CONTROL_AXIS, 5, ticks, other_layout + off, val, i ? expo : d->pow, i ? maxchange : d->maxchange);
    other[i] = (int32_t)R32(val);
    busy = 0;
    int theirs = abs(other[i]) > abs(own[i]);
    shown[i] = theirs ? other[i] : own[i];
    W32(val, (uint32_t)shown[i]);
    if (i == 0) {
        uint32_t now = R32(STEER_FACTOR);
        if (now != factor_shown)
            factor_game = now;              /* 0x480060's, for the game's own layout */
        factor_shown = theirs ? steer_factor(d) : factor_game;
        W32(STEER_FACTOR, factor_shown);
    }
    c->ret = R32(c->esp);
    c->esp += 4;
    return 1;
}

/* The other device's buttons and GameKeys count as the game's own: set after each read of the
   devices, which rewrites every key and button, so nothing is left held. The game finds presses
   against two earlier copies of the array (0x441f40 and the race frame make them), which may be
   from a menu where nothing was merged: they get the same, or a key held through the pause menu
   would press its other-device twin again on the way out. */
#define PRESSED_PREV1   0x1b195e8u
#define PRESSED_PREV2   0x1b19b84u

static void alias(uint32_t own, uint32_t other)
{
    static const uint32_t arrays[3] = {GAME_PRESSED, PRESSED_PREV1, PRESSED_PREV2};
    if (!own || !other || own == other || own >= PRESSED_END || other >= PRESSED_END || (own >= 256 && own < 260))
        return;
    for (int a = 0; a < 3; a++)
        if (R8(arrays[a] + other))
            W8(arrays[a] + own, 0x80);
}

static void merge_buttons(void)
{
    const DeviceSet *d = other_set();
    if (!d)
        return;
    for (int k = 0; k < 8; k++)
        alias(R32(GAME_LAYOUT + 168 + 4 * k), u32_at(d->layout + 168 + 4 * k));
    const uint32_t *keys = other_keys();
    for (int k = 0; k < 24; k++)
        alias(R32(GAME_GAME_KEYS + 4 * k), keys[k]);
}

/* The control a hint names on the device the prompts show (want_pad): the game's own binding
   or the other device's, whichever is that device's (the game's own if neither is). */
uint32_t padgame_binding(int layout_button, int game_key, int want_pad)
{
    uint32_t own = layout_button >= 0 ? R32(GAME_LAYOUT + 168 + 4 * (uint32_t)layout_button)
                                      : R32(GAME_GAME_KEYS + 4 * (uint32_t)game_key);
    const DeviceSet *d = pad_active ? other_set() : NULL;
    if (!d)
        return own;
    uint32_t other = layout_button >= 0 ? u32_at(d->layout + 168 + 4 * layout_button) : other_keys()[game_key];
    if (pad_code(own) != want_pad && other && pad_code(other) == want_pad)
        return other;
    return own;
}

/* Leaving an in-race menu (pause menu, instruction screen, ...): the buttons and keys pressed
   while it was up and still held when the race takes the controls again (the one that closed
   it: B, Start, A on CONTINUE GAME, Escape, Enter) count as released until they are let go.
   Before, B held out of the pause menu was the rollover (upright: no visible effect, but the
   game then retires its own rollover help for good, RolloverHelp; upside down: the car rolls
   back onto its wheels) and A the handbrake for as long as it was held. What was already held
   when the menu came up (a key held for the throttle) and the sticks and triggers (axes, not
   in the pressed-codes array) are left alone. */
static uint8_t held_before[PRESSED_END];   /* held since before the menu came up */
static uint8_t masked[PRESSED_END];

static void menu_mask(void)
{
    static int in_menu;
    static const uint32_t arrays[3] = {GAME_PRESSED, PRESSED_PREV1, PRESSED_PREV2};
    const uint8_t *now = (const uint8_t *)gptr(GAME_PRESSED);
    if (!R32(GAME_RACE_MODE)) {             /* the main menus, the demo: not a race */
        in_menu = 0;
        memset(masked, 0, sizeof masked);
        return;
    }
    if (R32(GAME_DRIVING) != 1) {
        for (uint32_t k = 0; k < PRESSED_END; k++)
            held_before[k] = in_menu ? held_before[k] && now[k] : now[k] != 0;
        in_menu = 1;
        return;
    }
    if (in_menu) {
        for (uint32_t k = 0; k < PRESSED_END; k++)
            masked[k] = now[k] && !held_before[k];
        in_menu = 0;
    }
    for (uint32_t k = 0; k < PRESSED_END; k++) {
        if (!masked[k])
            continue;
        if (!now[k]) {
            masked[k] = 0;
            continue;
        }
        for (int a = 0; a < 3; a++)
            W8(arrays[a] + k, 0);
    }
}

/* ------------------------------------------------------------------ controllers connected later */
/* The game looks for joysticks once, at start-up (0x44aab0: EnumDevices with the callback
   0x44a710, which makes the device and counts its objects, then 0x44a860 per joystick: data
   format, co-operative level, ranges, capabilities; the input poll acquires it). A controller
   connected later is handed to the same two routines, before the next input poll, so the game
   gets one more joystick exactly as if it had been there. One that comes back (or another one
   swapped in) takes the slot the game already has (input.h), whose device just acquires again. */
#define JOY_COUNT       0x1b1a764u      /* the game's joysticks (at most 4; devices at 0x1b1a774) */
#define GAME_DINPUT     0x1b19b78u      /* its IDirectInputA */
#define F_JOY_FOUND     0x44a710u       /* the EnumDevices callback (instance, ref), stdcall */
#define F_JOY_SETUP     0x44a860u       /* (joystick) */

static void claim_joysticks(CPU *c)
{
    static int refused[INPUT_MAX_JOYS];
    int s = dinput_joy_unclaimed();
    if (s < 0 || refused[s] || !R32(GAME_DINPUT))
        return;
    uint32_t n = R32(JOY_COUNT);
    if (n < 4) {
        uint32_t inst = dinput_joy_instance(s);
        guest_call(c, F_JOY_FOUND, 2, inst, 0);
        gfree(inst);
    }
    if (n >= 4 || R32(JOY_COUNT) != n + 1) {
        refused[s] = 1;
        LOG(LOG_ALWAYS, "controller: joystick %d connected after start-up, but the game took no more joysticks", s);
        return;
    }
    guest_call(c, F_JOY_SETUP, 1, n);
    LOG(LOG_ALWAYS, "controller: joystick %d connected after start-up is the game's joystick %u", s, n);
}

/* The name of the layout the settings store says is chosen (empty if none). */
static void chosen_layout(char name[NAME_SIZE])
{
    uint32_t idx = 0;
    name[0] = 0;
    if (settings_get(GAME_REG, "MenuLastCtrlLayout", NULL, &idx, 4) == 4 && idx < R32(GAME_LAYOUT_COUNT))
        memcpy(name, gptr(GAME_LAYOUT_NAMES + NAME_SIZE * idx), NAME_SIZE - 1);
}

/* The first controller of a session that started without one: from here on it is a session with
   a controller (the hooks act; at exit its settings go to the controller set and the next session
   without one gets the keyboard set back, the settings so far being that set). The chosen layout
   is kept: the keyboard's stays, and the controller drives with its own beside it. XBOX
   CONTROLLER joins the layout list (not chosen) if it is not there. */
static void pad_activate(void)
{
    pad_active = 1;
    if (!pad_mode) {
        char name[NAME_SIZE];
        chosen_layout(name);
        save_set(OWN_KBD, name[0] ? name : NULL);
        settings_put(OWN_REG, "Mode", 1, "controller", 11);
        pad_mode = 1;
    }
    sets_init();
    uint32_t n = R32(GAME_LAYOUT_COUNT);
    for (uint32_t k = 0; k < n && xbox_index < 0; k++)
        if (!strncmp((const char *)gptr(GAME_LAYOUT_NAMES + NAME_SIZE * k), XBOX_NAME, NAME_SIZE))
            xbox_index = (int)k;
    if (xbox_index < 0 && n < MAX_LAYOUTS) {
        xbox_layout((uint8_t *)gptr(GAME_LAYOUTS + LAYOUT_SIZE * n));
        memset(gptr(GAME_LAYOUT_NAMES + NAME_SIZE * n), 0, NAME_SIZE);
        memcpy(gptr(GAME_LAYOUT_NAMES + NAME_SIZE * n), XBOX_NAME, sizeof XBOX_NAME);
        W32(GAME_LAYOUT_COUNT, n + 1);
        xbox_index = (int)n;
        xbox_virtual = 1;
    }
    hook_layout_save = layout_save_hook;
    LOG(LOG_ALWAYS, "controls: controller connected after start-up (%s is layout %d%s)", XBOX_NAME, xbox_index,
        xbox_virtual ? ", provided" : "");
}

/* input poll: controllers connected since the last one; in races the other device's buttons; on
   "PRESS ENTER FOR RESULTS" (still driving, race over) A or Start is Enter, once pressed after
   the race ended (A is the handbrake until then), and no merging (Start would also be a keyboard
   layout's Escape there) */
static int input_poll_hook(CPU *c)
{
    static int armed;
    claim_joysticks(c);
    if (!pad_active && pad_count() > 0)
        pad_activate();
    if (!pad_active)
        return 0;
    ifc_poll(c);                                    /* force feedback as rumble (ifc21.c) */
    F_0044add0_orig(c);
    menu_mask();
    if (driving() && R32(GAME_RACE_STATE) != 3)
        merge_buttons();
    if (R32(GAME_DRIVING) == 1 && R32(GAME_RACE_STATE) == 3) {
        int b = pad_held(PAD_A) || pad_held(PAD_START);
        if (!b)
            armed = 1;
        else if (armed)
            W8(GAME_PRESSED + DIK_RETURN, 0x80);
    } else {
        armed = 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ start and end */
void padgame_init(void)
{
    pad_mode = pad_active = pad_count() > 0;
    input_used(pad_mode);                           /* prompts start with the controller if one is here */
    for (int k = 0; k < 24; k++)
        default_keys[k] = R32(GAME_GAME_KEYS + 4 * k);
    default_maxchange = R32(STEER_MAXCHANGE);
    /* in every session: until a controller is connected they do nothing (pad_active) */
    hook_define_axis = define_axis_hook;
    hook_menu_input = menu_input_hook;
    hook_input_poll = input_poll_hook;
    hook_control_axis = control_axis_hook;
    had_pad = settings_has(OWN_PAD, "Saved");
    if (!pad_mode && !had_pad)
        return;                                     /* the game as shipped */
    char last[16] = "";
    settings_get(OWN_REG, "Mode", NULL, last, sizeof last - 1);
    int free_slot = scan_layouts();
    if (pad_mode && xbox_index < 0 && free_slot < MAX_LAYOUTS) {
        xbox_index = free_slot;
        xbox_virtual = 1;
    }
    const char *what = "kept";
    if (pad_mode) {
        if (!had_pad) {
            if (!settings_has(OWN_KBD, "Saved"))
                save_set(OWN_KBD, NULL);            /* the player's settings so far */
            select_xbox();
            what = "chose " XBOX_NAME;
        } else if (strcmp(last, "controller")) {
            restore_set(OWN_PAD);
            what = "restored the controller settings";
        }
        hook_layout_load = layout_load_hook;
        hook_layout_save = layout_save_hook;
        sets_init();
    } else if (!strcmp(last, "controller") && settings_has(OWN_KBD, "Saved")) {
        restore_set(OWN_KBD);
        what = "restored the keyboard settings";
    }
    settings_put(OWN_REG, "Mode", 1, pad_mode ? "controller" : "keyboard", pad_mode ? 11 : 9);
    if (xbox_index < 0 && pad_mode)
        LOG(LOG_ALWAYS, "controller: no room for the %s layout (8 layouts already)", XBOX_NAME);
    LOG(LOG_ALWAYS, "controls: %s connected, %s (%s is layout %d%s)", pad_mode ? "controller" : "no controller",
        what, XBOX_NAME, xbox_index, xbox_virtual ? ", provided" : "");
}

void padgame_exit(void)
{
    if (!pad_mode && !had_pad)
        return;
    char name[NAME_SIZE];
    chosen_layout(name);
    save_set(pad_mode ? OWN_PAD : OWN_KBD, name[0] ? name : NULL);
}

void padgame_frame(unsigned long frame)
{
    static int log = -1;
    if (log < 0) {
        const char *v = getenv("INSANE_PAD_LOG");
        log = v && *v && strcmp(v, "0");
    }
    if (!log)
        return;
    int32_t v[6];
    for (int k = 0; k < 6; k++)
        v[k] = (int32_t)R32(GAME_CONTROLS + 4 * k);
    uint32_t p = R32(LOCAL_PLAYER) & 15, blk = PLAYER_BLOCKS + 0x4d8 * p;
    /* rollover help: the HUD's state (0 upright, 1 upside down a second, 2 retired: the player
       has used rollover, repair or rescue; saved as RolloverHelp) and whether its text is up */
    LOG(LOG_ALWAYS, "controls %lu: steer %.3f accel %.3f brake %.3f handbrake %.3f look %.3f %.3f  "
        "driving %u race %u/%u nav %02x %02x %02x %02x %02x %02x buttons %02x repair %u rescue %u factor %.3f "
        "rollover help %u/%u",
        frame, v[0] / 8323072.0, v[1] / 8323072.0, v[2] / 8323072.0, v[3] / 8323072.0, v[4] / 8323072.0,
        v[5] / 8323072.0, R32(GAME_DRIVING), R32(GAME_RACE_MODE), R32(GAME_RACE_STATE), R8(GAME_MENU_NAV),
        R8(GAME_MENU_NAV + 1), R8(GAME_MENU_NAV + 2), R8(GAME_MENU_NAV + 3), R8(GAME_MENU_NAV + 4),
        R8(GAME_MENU_NAV + 5), R8(0x155d3b6 + 8 * p), R32(blk + 0x60), R32(blk + 0x64), fbits(R32(STEER_FACTOR)),
        R32(0x1c54e1c), R32(0x1c54e20));
}
