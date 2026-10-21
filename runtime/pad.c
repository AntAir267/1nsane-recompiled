/* Controllers as DirectInput joysticks (pad.h): the mapping SDL's gamepad events go through
   (win.c), and INSANE_PAD, a scripted controller for headless tests that goes through the
   same calls.

   Axis order: the game names joystick axes from a table of five (X Y Z U V); the sixth reads
   past it (a stray pointer: blank here, likely a crash on Windows), so the right stick's
   vertical, the least used (head rise), goes there and the triggers come before the right
   stick. Dead zone: the game means to set its own (10 %) on every axis at start-up, but that
   call names no axis and fails (DIERR_NOTFOUND, on Windows too); later it sets its ANALOGUE
   DEADZONE (5 %) on axis X only, entering a race. So both sticks get a round dead zone here
   against resting drift (controller_deadzone in options.txt, percent, default 6: steering then
   ends up near the game's intended 10 %), the rest of the travel stretched over the range. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "options.h"
#include "pad.h"

typedef struct {
    int connected;
    uint8_t button[PAD_BUTTONS];
    int axis[PAD_AXES];
} Pad;

static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static Pad pads[INPUT_MAX_JOYS];
static double stick_dead = 0.06 * 32767;

/* SDL axis -> joystick axis (see above) */
static const int joy_axis[PAD_AXES] = {INPUT_AXIS_X, INPUT_AXIS_Y, INPUT_AXIS_RY, INPUT_AXIS_RZ,
                                       INPUT_AXIS_Z, INPUT_AXIS_RX};

static int clamp16(double v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int)lround(v); }

static int last_controller;             /* the device used last was a controller */
void input_used(int controller) { last_controller = controller != 0; }
int input_controller_last(void) { return last_controller; }

/* Plugging a controller in counts as using it (the prompts switch to it); unplugging the last
   one hands them back to the keyboard. */
int pad_attach(const char *id, const char *name)
{
    int slot = input_joy_attach(id, name, 6, 16, 1);
    if (slot < 0)
        return -1;
    input_joy_set_ff(slot, ifc_rumble_wanted());    /* the game plays force feedback as rumble */
    pthread_mutex_lock(&pad_lock);
    memset(&pads[slot], 0, sizeof pads[slot]);
    pads[slot].connected = 1;
    pthread_mutex_unlock(&pad_lock);
    input_used(1);
    return slot;
}

void pad_detach(int slot)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS)
        return;
    input_joy_disconnect(slot);
    pthread_mutex_lock(&pad_lock);
    memset(&pads[slot], 0, sizeof pads[slot]);
    pthread_mutex_unlock(&pad_lock);
    if (!pad_count())
        input_used(0);
}

void pad_axis(int slot, int axis, int value)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || axis < 0 || axis >= PAD_AXES)
        return;
    pthread_mutex_lock(&pad_lock);
    pads[slot].axis[axis] = value;
    int x = pads[slot].axis[axis & ~1], y = pads[slot].axis[axis | 1];
    pthread_mutex_unlock(&pad_lock);
    if (abs(value) > 12000)
        input_used(1);
    if (axis >= PAD_LT) {
        input_joy_axis(slot, joy_axis[axis], value);
        return;
    }
    /* a stick: round dead zone, the rest of the travel stretched over the full range */
    double r = hypot(x, y), k = r > stick_dead ? (r - stick_dead) / (32767 - stick_dead) * 32767 / r : 0;
    input_joy_axis(slot, joy_axis[axis & ~1], clamp16(x * k));
    input_joy_axis(slot, joy_axis[axis | 1], clamp16(y * k));
}

void pad_button(int slot, int button, int down)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || button < 0 || button >= PAD_BUTTONS)
        return;
    if (down) {
        input_used(1);
        video_skip_press();             /* (any button skips an intro video: video.c) */
    }
    pthread_mutex_lock(&pad_lock);
    Pad *p = &pads[slot];
    p->button[button] = down != 0;
    int up = p->button[PAD_DUP], dn = p->button[PAD_DDOWN], l = p->button[PAD_DLEFT], r = p->button[PAD_DRIGHT];
    pthread_mutex_unlock(&pad_lock);
    if (button >= PAD_DUP && button <= PAD_DRIGHT) {
        int v = INPUT_POV_CENTERED;
        if (up && r) v = 4500; else if (r && dn) v = 13500;
        else if (dn && l) v = 22500; else if (l && up) v = 31500;
        else if (up) v = 0; else if (r) v = 9000; else if (dn) v = 18000; else if (l) v = 27000;
        input_joy_pov(slot, 0, v);
    } else {
        input_joy_button(slot, button < PAD_DUP ? button : button - 4, down);
    }
}

int pad_held(int button)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        r |= pads[s].connected && pads[s].button[button];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

int pad_axis_value(int axis)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        if (pads[s].connected && abs(pads[s].axis[axis]) > abs(r))
            r = pads[s].axis[axis];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

int pad_count(void)
{
    int n = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        n += pads[s].connected;
    pthread_mutex_unlock(&pad_lock);
    return n;
}

/* ------------------------------------------------------------------ INSANE_PAD */
/* axis: 0 a button, 1 an axis, 2 plugging (index 0 unplug, 1 the controller, 2 another one) */
typedef struct { unsigned long at, release; int axis, index, value, hold, state; } PadPress;
static PadPress *script;
static int nscript, script_slot = -1;
static const char *const plug_names[3] = {"UNPLUG", "PLUG", "PLUG2"};

static const struct { const char *name; int button; } button_names[] = {
    {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"BACK", PAD_BACK}, {"VIEW", PAD_BACK},
    {"SELECT", PAD_BACK}, {"GUIDE", PAD_GUIDE}, {"START", PAD_START}, {"MENU", PAD_START},
    {"LS", PAD_LSTICK}, {"L3", PAD_LSTICK}, {"RS", PAD_RSTICK}, {"R3", PAD_RSTICK},
    {"LB", PAD_LB}, {"RB", PAD_RB}, {"UP", PAD_DUP}, {"DOWN", PAD_DDOWN}, {"LEFT", PAD_DLEFT},
    {"RIGHT", PAD_DRIGHT}, {"MISC", PAD_MISC1},
};
static const char *const axis_names[PAD_AXES] = {"LX", "LY", "RX", "RY", "LT", "RT"};

void pad_init(void)
{
    const char *dz = option_str("controller_deadzone", NULL);
    if (dz) {
        double v = atof(dz);
        stick_dead = (v < 0 ? 0 : v > 90 ? 90 : v) / 100 * 32767;
    }
    const char *env = getenv("INSANE_PAD");
    if (!env || !*env)
        return;
    char *copy = strdup(env), *save = NULL;
    for (char *tok = strtok_r(copy, ", \t\n", &save); tok; tok = strtok_r(NULL, ", \t\n", &save)) {
        char *colon = strchr(tok, ':'), *end;
        unsigned long at = colon ? strtoul(tok, &end, 10) : 0;
        if (!colon || end != colon) {
            LOG(LOG_ALWAYS, "INSANE_PAD: \"%s\" is not frame:BUTTON or frame:AXIS=value", tok);
            continue;
        }
        char *name = colon + 1, *star = strchr(name, '*'), *eq = strchr(name, '=');
        int hold = 3;
        if (star) {
            *star = 0;
            hold = atoi(star + 1) > 0 ? atoi(star + 1) : 1;
        }
        PadPress p = {at, 0, 0, -1, 1, hold, 0};
        for (int k = 0; k < 3 && !eq; k++)
            if (!strcasecmp(name, plug_names[k])) {
                p.axis = 2;
                p.index = k;
            }
        if (p.axis == 2) {
        } else if (eq) {
            *eq = 0;
            p.axis = 1;
            p.value = atoi(eq + 1);
            for (int a = 0; a < PAD_AXES; a++)
                if (!strcasecmp(name, axis_names[a]))
                    p.index = a;
        } else {
            for (size_t k = 0; k < sizeof button_names / sizeof *button_names; k++)
                if (!strcasecmp(name, button_names[k].name))
                    p.index = button_names[k].button;
        }
        if (p.index < 0) {
            LOG(LOG_ALWAYS, "INSANE_PAD: unknown %s \"%s\"", eq ? "axis" : "button", name);
            continue;
        }
        script = realloc(script, (nscript + 1) * sizeof *script);
        script[nscript++] = p;
    }
    free(copy);
    int first = -1;                 /* the first plugging event: if it plugs in, none at start-up */
    for (int k = 0; k < nscript; k++)
        if (script[k].axis == 2 && (first < 0 || script[k].at < script[first].at))
            first = k;
    if (first >= 0 && script[first].index) {
        LOG(LOG_ALWAYS, "INSANE_PAD: %d scripted inputs, the controller not connected yet", nscript);
        return;
    }
    script_slot = pad_attach("insane-pad-script", "Scripted controller");
    LOG(LOG_ALWAYS, "INSANE_PAD: controller connected as joystick %d, %d scripted inputs", script_slot, nscript);
}

static void script_plug(unsigned long frame, int which)
{
    if (script_slot >= 0) {
        LOG(LOG_ALWAYS, "INSANE_PAD: frame %lu: joystick %d unplugged", frame, script_slot);
        pad_detach(script_slot);
        script_slot = -1;
    }
    if (which) {
        script_slot = which == 1 ? pad_attach("insane-pad-script", "Scripted controller")
                                 : pad_attach("insane-pad-script-2", "Another scripted controller");
        LOG(LOG_ALWAYS, "INSANE_PAD: frame %lu: %s connected as joystick %d", frame,
            which == 1 ? "controller" : "another controller", script_slot);
    }
}

void pad_frame(unsigned long frame)
{
    for (int k = 0; k < nscript; k++) {
        PadPress *s = &script[k];
        int down = -1;
        if (s->state == 0 && frame >= s->at) {
            s->state = 1;
            s->release = frame + (unsigned long)s->hold;
            down = 1;
        } else if (s->state == 1 && frame >= s->release) {
            s->state = 2;
            down = 0;
        }
        if (down < 0)
            continue;
        if (s->axis == 2) {
            if (down)
                script_plug(frame, s->index);
        } else if (s->axis) {
            LOG(LOG_ALWAYS, "INSANE_PAD: frame %lu: %s = %d", frame, axis_names[s->index], down ? s->value : 0);
            pad_axis(script_slot, s->index, down ? s->value : 0);
        } else {
            const char *n = "?";
            for (size_t b = 0; b < sizeof button_names / sizeof *button_names; b++)
                if (button_names[b].button == s->index) {
                    n = button_names[b].name;
                    break;
                }
            LOG(LOG_ALWAYS, "INSANE_PAD: frame %lu: %s %s", frame, n, down ? "down" : "up");
            pad_button(script_slot, s->index, down);
        }
    }
    padgame_frame(frame);
}
