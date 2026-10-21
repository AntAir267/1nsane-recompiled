/* IFC21.dll stand-in: Immersion's force-feedback classes (IFC 2.1, C++, __thiscall: `this` in
   ecx, the callee pops its stack arguments), played as controller rumble (design point 5).

   How 1NSANE uses force feedback (Game.exe 0x44a860-0x44bf00):
   - At start-up it asks every joystick for its caps; one with DIDC_FORCEFEEDBACK sets
     0x1b1a768 and 0x1b1a088[joystick], and 0x44b1e0 then makes five periodic effects (one per
     waveform, for the road surface). The FORCE FEEDBACK option (MV_FORCE, registry
     ForceFeedBack, 0x1c54cf4) defaults to "a device was found"; the stored value wins.
   - Each time the car starts being driven (race start, out of the pause menu: 0x466100), with
     FORCE FEEDBACK on and the layout steering with a joystick axis, 0x44b460 makes a CImmDXDevice
     on that joystick (Initialize failing turns force feedback off), loads Data\Effects.ifr (an
     Immersion Studio project) into a CImmProject, creates its compound effects "Left tyre lands",
     "Right tyre lands" and "Frontal crash" (and four "?" placeholders, empty), turns the
     joystick's autocentre off and starts three endless effects: a constant force (steering
     weight), a periodic (engine) and a friction. Leaving the car (pause, race end: 0x466170)
     stops everything (0x44b880) and turns autocentre back on.
   - Every race frame (0x4657b0 -> 0x44ba10), from the car's own force-feedback values (its
     physics file: maxrpm baserpm maxpow powloss freqdiv maxperiod periodinterval fricmul):
     constant force = steering force x 0.05 x fricmul x FFGain (sign = direction); engine
     periodic: magnitude maxpow - rpm/maxrpm x powloss (less below baserpm) x FFGain, period
     maxperiod - rpm/maxrpm x periodinterval ms (the stock cars: maxpow 900-1200; 0, no engine
     effect, when a car file has none); the surface under the wheels (its ground's type in the
     map's talaj.dat, 13 types, table 0x4c52b8) picks one of the five waveforms: on PEBBLE and
     PEBBLE2 (gravel, rock) 25 Hz, on DUST 40 Hz, magnitude growing with speed to 700-900 at the
     type's speed and falling past it, on SMOKE (Canyon's sand) a 1 Hz wave up to 2500, nothing
     on grass, mud, water; plus the friction's coefficient. A surface effect stopped by a pause
     starts again only when the surface type changes. FFGain is FORCE FEEDBACK INTENSITY
     (0x4c5574: 0.5 by default, set by the controls' RESET TO DEFAULTS that start-up runs).
   - Events (0x44b9e0(effect, magnitude 0..10000), only for the player's car): "Frontal crash"
     on a collision with a car or an object and on the body hitting the ground (rolling over,
     landing on the nose), magnitude = impact x 10000 (0x49f334, 0x4a18ea, 0x4a1e61), and "Left/
     Right tyre lands" (the car's effect for that wheel, +0x3b8) when a wheel takes a hard hit
     (0x4a21a0: an impact over 8 in the game's 16.16 units wears the wheel's 0x3d8 down;
     magnitude = impact / 1000, or 10000 when the wheel breaks off). The next race frame
     restarts that compound with each contained effect's gain = its own gain x magnitude x
     FFGain. In Effects.ifr a tyre landing is a 35 ms half period of a 70 ms sine at full
     magnitude (the right one 70 ms late); a frontal crash is both.
   - With FORCE FEEDBACK on and a keyboard or mouse steering layout, the game plays nothing and
     instead pushes the steering value itself by the car's steering force (0x441c30). With a
     controller and RUMBLE on, the hooks below take the joystick way there instead (no push).

   Here: the stand-in answers as a working IFC device on a joystick the input layer reports as
   force-feedback capable (input_joy_set_ff: a controller with RUMBLE on), keeps every effect's
   parameters, and mixes what plays into the two motors of SDL gamepad rumble once per frame
   (ifc_frame): events become short kicks (a landing on the side's motor, a crash on both,
   lengthened to what a pad motor can be felt doing), the surface a low continuous rumble, the
   engine a faint hum; the steering force and friction have no rumble equivalent. Only while
   the car is driven in a race (not in menus, the pause menu, replays, the attract-mode demo or
   once the race is over), with the window in front: anything else stops the motors at once.
   Sends always carry a duration (no motor can stay on if the game stops calling); exit stops
   them too. INSANE_RUMBLE_LOG=1 logs every rumble request with the effects behind it and the
   game's events.

   Constructors only write a vtable pointer (the game embeds and subclasses these objects, so
   they must not write past their first field); the state is kept here, by object address. */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "funcs.h"
#include "image.h"
#include "input.h"
#include "options.h"
#include "pad.h"

#define DONT_CHANGE     0x80000000u     /* IMM_EFFECT_DONT_CHANGE */
#define DONT_CHANGE_PTR 0xffffffffu     /* ... for an envelope pointer */
#define INF             0xffffffffu     /* INFINITE duration */

/* game addresses (GOG build) */
#define FF_ON           0x1c54cf4u      /* FORCE FEEDBACK (MV_FORCE, registry ForceFeedBack) */
#define FF_READY        0x1b1a748u      /* the force-feedback objects are made (0x44b1e0) */
#define FF_FOUND        0x1b1a768u      /* a force-feedback joystick was found */
#define FF_JOY          0x1b1a088u      /* per joystick (4): it can do force feedback */
#define JOY_COUNT       0x1b1a764u      /* the game's joysticks */
#define JOY_DEVS        0x1b1a774u      /* their IDirectInputDevice2A pointers */
#define F_FF_OBJECTS    0x44b1e0u       /* makes the surface waveforms, sets FF_READY */
#define FF_ENGINE       0x1b1a5c0u      /* CImmPeriodic: engine */
#define FF_STEER        0x1b1a688u      /* CImmConstant: steering force */
#define FF_FRICTION     0x1b198d8u      /* CImmFriction */
#define FF_SURFACES     0x1b1a750u      /* 5 pointers to CImmPeriodic subclasses, one per waveform */

#define LOGA(...) logmsg(LOG_ALWAYS, __VA_ARGS__)
#define LOGV(...) do { if (verbose && LOGON(LOG_INPUT)) logmsg(LOG_INPUT, __VA_ARGS__); } while (0)

static uint32_t base_vtbl;
static int verbose;                     /* INSANE_LOG_INPUT_VERBOSE: the per-frame calls too */
static pthread_mutex_t ifc_lock = PTHREAD_MUTEX_INITIALIZER;
static int feature = -1;                /* the Enhanced options are on (not a plain --test run) */
static int rlog;                        /* INSANE_RUMBLE_LOG */

/* the Enhanced options are on (as enhanced.c decides; pad.c asks before ifc_init) */
static int enhanced_on(void)
{
    if (feature < 0)
        feature = !deterministic || getenv("INSANE_ENHANCED");
    return feature;
}

static void bad_virtual(CPU *c)
{
    fatal("virtual call on an IFC21 base object (return address %08x)", R32(c->esp));
}

/* ------------------------------------------------------------------ the project file (.ifr) */
/* Immersion Studio's binary project: "ifpr", object count, then per object: size (with itself),
   name, type ("Compound", "Periodic", ...), an empty string, a block size, then key/value string
   pairs ("ID", "ContainedObjects" = GUIDs with ';' between, "Period" ...). Times in us. */
enum { P_PERIOD, P_MAG, P_WAVE, P_DUR, P_DELAY, P_PHASE, P_OFFSET, P_GAIN, P_INF, NPROP };
static const char *const prop_names[NPROP] = {"Period", "Magnitude", "Waveform", "Duration", "StartDelay",
                                              "Phase", "ValueOffset", "Gain", "InfiniteDuration"};
typedef struct {
    char name[48], type[16], id[40];
    char contained[8][40];
    int ncontained;
    int32_t prop[NPROP];
    unsigned has;
} IfrObj;
#define MAXIFR 32
static IfrObj ifr[MAXIFR];
static int nifr;

static const char *take_str(const uint8_t **p, const uint8_t *end)
{
    const uint8_t *s = *p;
    while (*p < end && **p)
        (*p)++;
    if (*p < end)
        (*p)++;
    return (const char *)s;
}

static int ifr_parse(uint32_t data)
{
    const uint8_t *b = (const uint8_t *)gptr(data);
    nifr = 0;
    if (!data || memcmp(b, "ifpr", 4))
        return 0;
    uint32_t n, off = 8;
    memcpy(&n, b + 4, 4);
    for (uint32_t i = 0; i < n && nifr < MAXIFR; i++) {
        uint32_t size;
        memcpy(&size, b + off, 4);
        if (size < 8 || size > 0x10000)
            break;
        const uint8_t *p = b + off + 4, *end = b + off + size;
        IfrObj *o = &ifr[nifr++];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", take_str(&p, end));
        snprintf(o->type, sizeof o->type, "%s", take_str(&p, end));
        take_str(&p, end);                       /* (empty) */
        p += 4;                                  /* the identity block's size */
        while (p < end) {
            const char *k = take_str(&p, end), *v = take_str(&p, end);
            if (!strcmp(k, "ID")) {
                snprintf(o->id, sizeof o->id, "%s", v);
            } else if (!strcmp(k, "ContainedObjects")) {
                for (const char *s = v; *s && o->ncontained < 8;) {
                    size_t l = strcspn(s, ";");
                    snprintf(o->contained[o->ncontained++], sizeof o->contained[0], "%.*s", (int)l, s);
                    s += l + (s[l] == ';');
                }
            } else {
                for (int q = 0; q < NPROP; q++)
                    if (!strcmp(k, prop_names[q])) {
                        o->prop[q] = (int32_t)strtol(v, NULL, 10);
                        o->has |= 1u << q;
                    }
            }
        }
        off += size;
    }
    return nifr > 0;
}

static const IfrObj *ifr_find(const char *name, const char *id)
{
    for (int i = 0; i < nifr; i++)
        if ((name && !strcasecmp(ifr[i].name, name)) || (id && !strcasecmp(ifr[i].id, id)))
            return &ifr[i];
    return NULL;
}

/* ------------------------------------------------------------------ effect records */
enum { K_FREE, K_DEVICE, K_PROJECT, K_COMPOUND, K_CONTAINED, K_CONSTANT, K_PERIODIC, K_FRICTION };
/* what an effect is for (how it becomes rumble) */
enum { R_NONE, R_ENGINE, R_STEER, R_FRICTION, R_SURFACE, R_LAND_L, R_LAND_R, R_CRASH, R_KICK };
static const char *const role_names[] = {"effect", "engine", "steering", "friction", "surface",
                                         "left tyre lands", "right tyre lands", "frontal crash", "event"};
static const char *const wave_names[] = {"square", "sine", "triangle", "sawtooth up", "sawtooth down"};

typedef struct {
    int kind, role, wave, playing, slot, parent, nchild, child[8];
    uint32_t obj, dev;                  /* the program's object; the device it plays on */
    int32_t mag, offset;                /* periodic 0..10000; constant signed */
    uint32_t period, phase, duration, delay, gain;   /* ms, degrees, ms or INF, ms, 0..10000 */
    double started;
    char name[48];
} Imm;
#define MAXIMM 128
static Imm imm[MAXIMM];

static Imm *imm_find(uint32_t obj)
{
    for (int i = 0; i < MAXIMM; i++)
        if (imm[i].kind && imm[i].obj == obj)
            return &imm[i];
    return NULL;
}

static Imm *imm_get(uint32_t obj, int kind)
{
    Imm *e = imm_find(obj);
    if (e)
        return e;
    for (int i = 0; i < MAXIMM; i++)
        if (!imm[i].kind) {
            e = &imm[i];
            memset(e, 0, sizeof *e);
            e->kind = kind;
            e->obj = obj;
            e->slot = -1;
            e->parent = -1;
            e->gain = 10000;
            e->duration = INF;
            e->wave = 1;
            return e;
        }
    LOGA("IFC21: more than %d effect objects", MAXIMM);
    return NULL;
}

static void imm_free(uint32_t obj)
{
    Imm *e = imm_find(obj);
    if (e)
        e->kind = K_FREE;
}

/* the role of an effect the game made itself (by its known objects) */
static int game_role(uint32_t obj)
{
    if (obj == FF_ENGINE) return R_ENGINE;
    if (obj == FF_STEER) return R_STEER;
    if (obj == FF_FRICTION) return R_FRICTION;
    for (int k = 0; k < 5; k++)
        if (R32(FF_SURFACES + 4 * k) == obj)
            return R_SURFACE;
    return R_NONE;
}

static double ifc_clock(void)
{
    /* --test: by frames (now_ms would move the virtual clock the game runs on) */
    return deterministic ? frame_count * (1000.0 / 60.0) : now_ms();
}

static uint32_t ms_arg(uint32_t v, uint32_t old)       /* a time argument in ms */
{
    return v == DONT_CHANGE ? old : v;
}

static void start(Imm *e, double now)
{
    e->playing = 1;
    e->started = now;
}

/* ------------------------------------------------------------------ device */
/* ?Initialize@CImmDXDevice@@QAEHPAX0PAUIDirectInputA@@PAUIDirectInputDevice2A@@@Z:
   (hinstance, hwnd, IDirectInputA*, IDirectInputDevice2A*) -> works on a force-feedback joystick */
HOSTFN(w_IFC21__Initialize_CImmDXDevice__QAEHPAX0PAUIDirectInputA__PAUIDirectInputDevice2A___Z, 4)
{
    uint32_t didev = ARG(3);
    int slot = didev ? dinput_joy_slot(didev) : -1;
    int ok = slot >= 0 && dinput_dev_ff(didev) && ifc_rumble_wanted();
    pthread_mutex_lock(&ifc_lock);
    if (ok) {
        Imm *d = imm_get(c->ecx, K_DEVICE);
        if (d) {
            d->kind = K_DEVICE;
            d->slot = slot;
        }
    }
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmDXDevice::Initialize(this %08x, device %08x = joy%d) -> %d", c->ecx, didev, slot, ok);
    if (rlog)
        LOGA("ff frame %lu: force feedback %s on joy%d", frame_count, ok ? "started" : "refused", slot);
    return (uint32_t)ok;
}

/* ?GetDevice@CImmDXDevice@@UAEPAUIFeelitDevice@@XZ */
HOSTFN(w_IFC21__GetDevice_CImmDXDevice__UAEPAUIFeelitDevice__XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmDXDevice::GetDevice this=%08x", c->ecx);
    return 0;
}

/* ?GetAPI@CImmDXDevice@@UAEPAUIFeelit@@XZ */
HOSTFN(w_IFC21__GetAPI_CImmDXDevice__UAEPAUIFeelit__XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmDXDevice::GetAPI this=%08x", c->ecx);
    return 0;
}

/* ??0CImmDXDevice@@QAE@XZ */
HOSTFN(w_IFC21___0CImmDXDevice__QAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmDXDevice::CImmDXDevice this=%08x", c->ecx);
    W32(c->ecx, base_vtbl);
    return c->ecx;
}

/* ??1CImmDXDevice@@UAE@XZ: its effects stop with it */
HOSTFN(w_IFC21___1CImmDXDevice__UAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmDXDevice::~CImmDXDevice this=%08x", c->ecx);
    pthread_mutex_lock(&ifc_lock);
    for (int i = 0; i < MAXIMM; i++)
        if (imm[i].kind && imm[i].dev == c->ecx)
            imm[i].playing = 0;
    imm_free(c->ecx);
    pthread_mutex_unlock(&ifc_lock);
    return 0;
}

/* ?SwitchToAbsoluteMode@CImmDevice@@UAEHH@Z */
HOSTFN(w_IFC21__SwitchToAbsoluteMode_CImmDevice__UAEHH_Z, 1)
{
    LOG(LOG_INPUT, "IFC21 CImmDevice::SwitchToAbsoluteMode this=%08x", c->ecx);
    return 1;
}

/* ?ChangeScreenResolution@CImmDevice@@UAEHHKK@Z */
HOSTFN(w_IFC21__ChangeScreenResolution_CImmDevice__UAEHHKK_Z, 3)
{
    LOG(LOG_INPUT, "IFC21 CImmDevice::ChangeScreenResolution this=%08x", c->ecx);
    return 1;
}

/* ?reset@CImmDXDevice@@MAEXXZ */
HOSTFN(w_IFC21__reset_CImmDXDevice__MAEXXZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmDXDevice::reset this=%08x", c->ecx);
    return 0;
}

/* ?prepare_device@CImmDevice@@MAEHXZ */
HOSTFN(w_IFC21__prepare_device_CImmDevice__MAEHXZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmDevice::prepare_device this=%08x", c->ecx);
    return 1;
}

/* ------------------------------------------------------------------ project */
/* ??0CImmProject@@QAE@XZ */
HOSTFN(w_IFC21___0CImmProject__QAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmProject::CImmProject this=%08x", c->ecx);
    W32(c->ecx, base_vtbl);
    return c->ecx;
}

/* ??1CImmProject@@QAE@XZ */
HOSTFN(w_IFC21___1CImmProject__QAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmProject::~CImmProject this=%08x", c->ecx);
    pthread_mutex_lock(&ifc_lock);
    for (int i = 0; i < MAXIMM; i++)
        if (imm[i].kind == K_COMPOUND || imm[i].kind == K_CONTAINED)
            imm[i].playing = 0;
    imm_free(c->ecx);
    pthread_mutex_unlock(&ifc_lock);
    return 0;
}

/* ?LoadProjectFromMemory@CImmProject@@QAEHPAXPAVCImmDevice@@@Z: (file in memory, device) */
HOSTFN(w_IFC21__LoadProjectFromMemory_CImmProject__QAEHPAXPAVCImmDevice___Z, 2)
{
    pthread_mutex_lock(&ifc_lock);
    int ok = ifr_parse(ARG(0));
    /* effects made from an earlier load are the game's no more (it creates them anew) */
    for (int i = 0; i < MAXIMM; i++)
        if (imm[i].kind == K_COMPOUND || imm[i].kind == K_CONTAINED)
            imm[i].playing = 0;
    Imm *p = ok ? imm_get(c->ecx, K_PROJECT) : NULL;
    if (p)
        p->dev = ARG(1);
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmProject::LoadProjectFromMemory(this %08x, %08x, device %08x) -> %d (%d objects)",
        c->ecx, ARG(0), ARG(1), ok, nifr);
    for (int i = 0; i < nifr; i++)
        LOG(LOG_INPUT, "  %s \"%s\": %d contained, period %d us, magnitude %d, duration %d us, delay %d us",
            ifr[i].type, ifr[i].name, ifr[i].ncontained, ifr[i].prop[P_PERIOD], ifr[i].prop[P_MAG],
            ifr[i].prop[P_DUR], ifr[i].prop[P_DELAY]);
    return (uint32_t)ok;
}

static int compound_role(const char *name)
{
    if (strcasestr(name, "crash")) return R_CRASH;
    if (strcasestr(name, "left")) return R_LAND_L;
    if (strcasestr(name, "right")) return R_LAND_R;
    return R_KICK;
}

/* an effect of the project as a contained effect record (and its program object) */
static int contained_new(const IfrObj *o, int parent)
{
    uint32_t obj = galloc(16);
    W32(obj, base_vtbl);
    Imm *e = imm_get(obj, K_CONTAINED);
    if (!e)
        return -1;
    e->parent = parent;
    e->role = imm[parent].role;
    snprintf(e->name, sizeof e->name, "%s", o->name);
    e->mag = o->has & (1u << P_MAG) ? o->prop[P_MAG] : 10000;
    e->period = (uint32_t)o->prop[P_PERIOD] / 1000;
    e->duration = o->prop[P_INF] ? INF : (uint32_t)o->prop[P_DUR] / 1000;
    e->delay = (uint32_t)o->prop[P_DELAY] / 1000;
    e->phase = (uint32_t)o->prop[P_PHASE] / 100;
    e->offset = o->prop[P_OFFSET];
    e->gain = o->has & (1u << P_GAIN) ? (uint32_t)o->prop[P_GAIN] : 10000;
    e->wave = o->prop[P_WAVE];
    return (int)(e - imm);
}

/* ?CreateEffect@CImmProject@@QAEPAVCImmCompoundEffect@@PBDPAVCImmDevice@@K@Z: (name, device or
   NULL for the project's, no-download flag) -> a compound effect: the game reads its count of
   contained effects at +4. A name the project does not have gets an empty one (the game would
   read through NULL). */
HOSTFN(w_IFC21__CreateEffect_CImmProject__QAEPAVCImmCompoundEffect__PBDPAVCImmDevice__K_Z, 3)
{
    const char *name = gstr(ARG(0)) ? gstr(ARG(0)) : "";
    pthread_mutex_lock(&ifc_lock);
    Imm *p = imm_find(c->ecx);
    const IfrObj *o = ifr_find(name, NULL);
    uint32_t obj = galloc(16);
    W32(obj, base_vtbl);
    Imm *e = imm_get(obj, K_COMPOUND);
    int n = 0;
    if (e) {
        e->dev = ARG(1) ? ARG(1) : p ? p->dev : 0;
        e->role = compound_role(name);
        snprintf(e->name, sizeof e->name, "%s", name);
        int idx = (int)(e - imm);
        if (o && !strcasecmp(o->type, "Compound")) {
            for (int k = 0; k < o->ncontained && e->nchild < 8; k++) {
                const IfrObj *ch = ifr_find(NULL, o->contained[k]);
                int ci = ch ? contained_new(ch, idx) : -1;
                if (ci >= 0)
                    e->child[e->nchild++] = ci;
            }
        } else if (o) {                         /* a single effect: a compound of one */
            int ci = contained_new(o, idx);
            if (ci >= 0)
                e->child[e->nchild++] = ci;
        }
        n = e->nchild;
    }
    W32(obj + 4, (uint32_t)n);
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmProject::CreateEffect(this %08x, \"%s\", device %08x) -> %08x (%d contained%s)",
        c->ecx, name, ARG(1), obj, n, o ? "" : ", not in the project");
    return obj;
}

/* ?Stop@CImmProject@@QAEHPBD@Z: (name, or NULL for all) */
HOSTFN(w_IFC21__Stop_CImmProject__QAEHPBD_Z, 1)
{
    const char *name = gstr(ARG(0));
    pthread_mutex_lock(&ifc_lock);
    for (int i = 0; i < MAXIMM; i++) {
        Imm *e = &imm[i];
        if (e->kind != K_CONTAINED)
            continue;
        if (!name || (e->parent >= 0 && !strcasecmp(imm[e->parent].name, name)))
            e->playing = 0;
    }
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmProject::Stop(this %08x, %s)", c->ecx, name ? name : "all");
    return 1;
}

/* ------------------------------------------------------------------ compound effects */
/* ?GetContainedEffect@CImmCompoundEffect@@QAEPAVCImmEffect@@J@Z */
HOSTFN(w_IFC21__GetContainedEffect_CImmCompoundEffect__QAEPAVCImmEffect__J_Z, 1)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    int k = (int)ARG(0);
    uint32_t r = e && e->kind == K_COMPOUND && k >= 0 && k < e->nchild ? imm[e->child[k]].obj : 0;
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmCompoundEffect::GetContainedEffect(this %08x, %d) -> %08x", c->ecx, k, r);
    return r;
}

/* ?Start@CImmCompoundEffect@@QAEHKK@Z: (iterations, flags) */
HOSTFN(w_IFC21__Start_CImmCompoundEffect__QAEHKK_Z, 2)
{
    double now = ifc_clock();
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    uint32_t g = 0;
    for (int k = 0; e && k < e->nchild; k++) {
        start(&imm[e->child[k]], now);
        if (imm[e->child[k]].gain > g)
            g = imm[e->child[k]].gain;
    }
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmCompoundEffect::Start(this %08x \"%s\", %u, %x)", c->ecx, e ? e->name : "?", ARG(0), ARG(1));
    if (rlog && e && e->nchild)
        LOGA("ff frame %lu: %s, gain %u (event x FORCE FEEDBACK INTENSITY)", frame_count, e->name, g);
    return 1;
}

/* ?Stop@CImmCompoundEffect@@QAEHXZ */
HOSTFN(w_IFC21__Stop_CImmCompoundEffect__QAEHXZ, 0)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    for (int k = 0; e && k < e->nchild; k++)
        imm[e->child[k]].playing = 0;
    pthread_mutex_unlock(&ifc_lock);
    LOGV("IFC21 CImmCompoundEffect::Stop(this %08x)", c->ecx);
    return 1;
}

/* ------------------------------------------------------------------ effects */
/* ?Start@CImmEffect@@UAEHKKH@Z: (iterations, flags, start delay emulation) */
HOSTFN(w_IFC21__Start_CImmEffect__UAEHKKH_Z, 3)
{
    double now = ifc_clock();
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    if (e)
        start(e, now);
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmEffect::Start(this %08x %s, %u, %x)", c->ecx, e ? role_names[e->role] : "?", ARG(0), ARG(1));
    return e != NULL;
}

/* ?Start@CImmCondition@@UAEHKK@Z */
HOSTFN(w_IFC21__Start_CImmCondition__UAEHKK_Z, 2)
{
    double now = ifc_clock();
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    if (e)
        start(e, now);
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmCondition::Start(this %08x, %u, %x)", c->ecx, ARG(0), ARG(1));
    return e != NULL;
}

/* ?Stop@CImmEffect@@UAEHXZ */
HOSTFN(w_IFC21__Stop_CImmEffect__UAEHXZ, 0)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    if (e)
        e->playing = 0;
    pthread_mutex_unlock(&ifc_lock);
    LOGV("IFC21 CImmEffect::Stop(this %08x %s)", c->ecx, e ? role_names[e->role] : "?");
    return 1;
}

/* ?ChangeBaseParams@CImmEffect@@QAEHJJKPAUFEELIT_ENVELOPE@@KKKKK@Z: (direction x, y, duration ms,
   envelope, sample period, gain, trigger button, repeat, start delay ms); the game only sets gains */
HOSTFN(w_IFC21__ChangeBaseParams_CImmEffect__QAEHJJKPAUFEELIT_ENVELOPE__KKKKK_Z, 9)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    if (e) {
        e->duration = ms_arg(ARG(2), e->duration);
        if (ARG(5) != DONT_CHANGE)
            e->gain = ARG(5) > 10000 ? 10000 : ARG(5);
        e->delay = ms_arg(ARG(8), e->delay);
    }
    pthread_mutex_unlock(&ifc_lock);
    LOGV("IFC21 CImmEffect::ChangeBaseParams(this %08x, gain %d)", c->ecx, (int)ARG(5));
    return e != NULL;
}

/* ?GetParameters@CImmEffect@@QAEXAAUFEELIT_EFFECT@@@Z: FEELIT_EFFECT is the effect's GUID then a
   DIEFFECT (0x48 bytes); the game keeps the gain (+0x20) to scale by an event's magnitude */
HOSTFN(w_IFC21__GetParameters_CImmEffect__QAEXAAUFEELIT_EFFECT___Z, 1)
{
    uint32_t p = ARG(0);
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    if (p) {
        memset(gptr(p), 0, 0x48);
        W32(p + 0x10, 0x34);                             /* dwSize (DX5 DIEFFECT) */
        W32(p + 0x18, !e || e->duration == INF ? INF : e->duration * 1000);
        W32(p + 0x20, e ? e->gain : 10000);              /* dwGain */
        W32(p + 0x24, INF);                              /* no trigger button */
        W32(p + 0x44, e ? e->delay * 1000 : 0);
    }
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmEffect::GetParameters(this %08x) gain %u", c->ecx, e ? e->gain : 10000);
    return 0;
}

/* ?InitializeFromProject@CImmEffect@@UAEHAAVCImmProject@@PBDPAVCImmDevice@@K@Z (unused by the game) */
HOSTFN(w_IFC21__InitializeFromProject_CImmEffect__UAEHAAVCImmProject__PBDPAVCImmDevice__K_Z, 4)
{
    LOG(LOG_INPUT, "IFC21 CImmEffect::InitializeFromProject this=%08x", c->ecx);
    return 0;
}

/* ------------------------------------------------------------------ constant force */
/* ??0CImmConstant@@QAE@XZ */
HOSTFN(w_IFC21___0CImmConstant__QAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmConstant::CImmConstant this=%08x", c->ecx);
    W32(c->ecx, base_vtbl);
    return c->ecx;
}

/* ??1CImmConstant@@UAE@XZ */
HOSTFN(w_IFC21___1CImmConstant__UAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmConstant::~CImmConstant this=%08x", c->ecx);
    pthread_mutex_lock(&ifc_lock);
    imm_free(c->ecx);
    pthread_mutex_unlock(&ifc_lock);
    return 0;
}

/* ?Initialize@CImmConstant@@UAEHPAVCImmDevice@@JJKJPAUFEELIT_ENVELOPE@@K@Z:
   (device, direction x, y, duration ms, magnitude, envelope, no download) */
HOSTFN(w_IFC21__Initialize_CImmConstant__UAEHPAVCImmDevice__JJKJPAUFEELIT_ENVELOPE__K_Z, 7)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(ARG(0)) ? imm_get(c->ecx, K_CONSTANT) : NULL;
    if (e) {
        e->kind = K_CONSTANT;
        e->dev = ARG(0);
        e->role = game_role(c->ecx);
        e->duration = ARG(3);
        e->mag = (int32_t)ARG(4);
        e->playing = 0;
    }
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmConstant::Initialize(this %08x, device %08x) -> %d", c->ecx, ARG(0), e != NULL);
    return e != NULL;
}

/* ?ChangeParameters@CImmConstant@@QAEHJJKJPAUFEELIT_ENVELOPE@@@Z: (direction x, y, duration ms,
   magnitude, envelope) */
HOSTFN(w_IFC21__ChangeParameters_CImmConstant__QAEHJJKJPAUFEELIT_ENVELOPE___Z, 5)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    if (e) {
        e->duration = ms_arg(ARG(2), e->duration);
        int neg = ARG(0) == DONT_CHANGE ? e->mag < 0 : (int32_t)ARG(0) < 0;
        if (ARG(3) != DONT_CHANGE)
            e->mag = neg ? -(int32_t)ARG(3) : (int32_t)ARG(3);
    }
    pthread_mutex_unlock(&ifc_lock);
    LOGV("IFC21 CImmConstant::ChangeParameters(this %08x, direction %d, magnitude %d)", c->ecx, (int)ARG(0), (int)ARG(3));
    return e != NULL;
}

/* ------------------------------------------------------------------ periodic */
static void periodic_ctor(CPU *c, uint32_t guid)
{
    W32(c->ecx, base_vtbl);
    /* the game makes its five surface waveforms with Immersion's GUIDs 5959DF73..77 */
    pthread_mutex_lock(&ifc_lock);
    Imm *e = guid ? imm_get(c->ecx, K_PERIODIC) : NULL;
    if (e) {
        uint32_t d1 = R32(guid);
        e->kind = K_PERIODIC;
        e->wave = (d1 & 0xffffff00u) == 0x5959df00u && (d1 & 0xff) >= 0x73 && (d1 & 0xff) <= 0x77
                      ? (int)(d1 & 0xff) - 0x73 : 1;
    }
    pthread_mutex_unlock(&ifc_lock);
}

/* ??0CImmPeriodic@@QAE@XZ (sine) */
HOSTFN(w_IFC21___0CImmPeriodic__QAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::CImmPeriodic this=%08x", c->ecx);
    periodic_ctor(c, 0);
    return c->ecx;
}

/* ??0CImmPeriodic@@QAE@ABU_GUID@@@Z */
HOSTFN(w_IFC21___0CImmPeriodic__QAE_ABU_GUID___Z, 1)
{
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::CImmPeriodic(this %08x, " GUID_FMT ")", c->ecx, GUID_ARGS(ARG(0)));
    periodic_ctor(c, ARG(0));
    return c->ecx;
}

/* ??1CImmPeriodic@@UAE@XZ */
HOSTFN(w_IFC21___1CImmPeriodic__UAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::~CImmPeriodic this=%08x", c->ecx);
    pthread_mutex_lock(&ifc_lock);
    imm_free(c->ecx);
    pthread_mutex_unlock(&ifc_lock);
    return 0;
}

/* ?GetIsCompatibleGUID@CImmPeriodic@@UAEHAAU_GUID@@@Z */
HOSTFN(w_IFC21__GetIsCompatibleGUID_CImmPeriodic__UAEHAAU_GUID___Z, 1)
{
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::GetIsCompatibleGUID this=%08x", c->ecx);
    return 1;
}

/* ?Initialize@CImmPeriodic@@UAEHPAVCImmDevice@@KKKJJJKPAUFEELIT_ENVELOPE@@K@Z:
   (device, magnitude, period ms, duration ms, direction x, y, offset, phase degrees, envelope,
   no download) */
HOSTFN(w_IFC21__Initialize_CImmPeriodic__UAEHPAVCImmDevice__KKKJJJKPAUFEELIT_ENVELOPE__K_Z, 10)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(ARG(0)) ? imm_get(c->ecx, K_PERIODIC) : NULL;
    if (e) {
        e->kind = K_PERIODIC;
        e->dev = ARG(0);
        e->role = game_role(c->ecx);
        e->mag = (int32_t)ARG(1);
        e->period = ARG(2);
        e->duration = ARG(3);
        e->offset = (int32_t)ARG(6);
        e->phase = ARG(7);
        e->playing = 0;
    }
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::Initialize(this %08x, device %08x) -> %d (%s, %s)", c->ecx, ARG(0), e != NULL,
        e ? role_names[e->role] : "-", e ? wave_names[e->wave % 5] : "-");
    return e != NULL;
}

/* ?ChangeParameters@CImmPeriodic@@QAEHKKKJJJKPAUFEELIT_ENVELOPE@@@Z: (magnitude, period ms,
   duration ms, direction x, y, offset, phase degrees, envelope) */
HOSTFN(w_IFC21__ChangeParameters_CImmPeriodic__QAEHKKKJJJKPAUFEELIT_ENVELOPE___Z, 8)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    if (e) {
        if (ARG(0) != DONT_CHANGE) e->mag = (int32_t)ARG(0);
        if (ARG(1) != DONT_CHANGE) e->period = ARG(1);
        e->duration = ms_arg(ARG(2), e->duration);
        if (ARG(5) != DONT_CHANGE) e->offset = (int32_t)ARG(5);
        if (ARG(6) != DONT_CHANGE) e->phase = ARG(6);
    }
    pthread_mutex_unlock(&ifc_lock);
    LOGV("IFC21 CImmPeriodic::ChangeParameters(this %08x, magnitude %d, period %d)", c->ecx, (int)ARG(0), (int)ARG(1));
    return e != NULL;
}

/* ?Initialize@CImmPeriodic@@UAEHPAVCImmDevice@@ABUFEELIT_EFFECT@@K@Z (unused by the game) */
HOSTFN(w_IFC21__Initialize_CImmPeriodic__UAEHPAVCImmDevice__ABUFEELIT_EFFECT__K_Z, 3)
{
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::Initialize(FEELIT_EFFECT) this=%08x", c->ecx);
    return 0;
}

/* ?InitializePolar@CImmPeriodic@@UAEHPAVCImmDevice@@KKKJJKPAUFEELIT_ENVELOPE@@K@Z (unused) */
HOSTFN(w_IFC21__InitializePolar_CImmPeriodic__UAEHPAVCImmDevice__KKKJJKPAUFEELIT_ENVELOPE__K_Z, 9)
{
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::InitializePolar this=%08x", c->ecx);
    return 0;
}

/* ?get_ffe_data@CImmPeriodic@@MAEHPAUDIEFFECT@@@Z (internal, unused) */
HOSTFN(w_IFC21__get_ffe_data_CImmPeriodic__MAEHPAUDIEFFECT___Z, 1)
{
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::get_ffe_data this=%08x", c->ecx);
    return 0;
}

/* ?buffer_ifr_data@CImmPeriodic@@MAEHPAD@Z (internal, unused) */
HOSTFN(w_IFC21__buffer_ifr_data_CImmPeriodic__MAEHPAD_Z, 1)
{
    LOG(LOG_INPUT, "IFC21 CImmPeriodic::buffer_ifr_data this=%08x", c->ecx);
    return 0;
}

/* ------------------------------------------------------------------ friction */
/* ??0CImmFriction@@QAE@XZ */
HOSTFN(w_IFC21___0CImmFriction__QAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmFriction::CImmFriction this=%08x", c->ecx);
    W32(c->ecx, base_vtbl);
    return c->ecx;
}

/* ??1CImmFriction@@UAE@XZ */
HOSTFN(w_IFC21___1CImmFriction__UAE_XZ, 0)
{
    LOG(LOG_INPUT, "IFC21 CImmFriction::~CImmFriction this=%08x", c->ecx);
    pthread_mutex_lock(&ifc_lock);
    imm_free(c->ecx);
    pthread_mutex_unlock(&ifc_lock);
    return 0;
}

/* ?Initialize@CImmFriction@@UAEHPAVCImmDevice@@KKKJJK@Z */
HOSTFN(w_IFC21__Initialize_CImmFriction__UAEHPAVCImmDevice__KKKJJK_Z, 7)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(ARG(0)) ? imm_get(c->ecx, K_FRICTION) : NULL;
    if (e) {
        e->kind = K_FRICTION;
        e->dev = ARG(0);
        e->role = R_FRICTION;
        e->mag = (int32_t)ARG(1);
        e->playing = 0;
    }
    pthread_mutex_unlock(&ifc_lock);
    LOG(LOG_INPUT, "IFC21 CImmFriction::Initialize(this %08x, device %08x) -> %d", c->ecx, ARG(0), e != NULL);
    return e != NULL;
}

/* ?ChangeParameters@CImmFriction@@QAEHKKJJ@Z: (coefficient x, y, ...) */
HOSTFN(w_IFC21__ChangeParameters_CImmFriction__QAEHKKJJ_Z, 4)
{
    pthread_mutex_lock(&ifc_lock);
    Imm *e = imm_find(c->ecx);
    if (e && ARG(0) != DONT_CHANGE)
        e->mag = (int32_t)ARG(0);
    pthread_mutex_unlock(&ifc_lock);
    LOGV("IFC21 CImmFriction::ChangeParameters(this %08x, %d)", c->ecx, (int)ARG(0));
    return e != NULL;
}

/* ------------------------------------------------------------------ rumble */
/* How the effects feel on a pad (Xbox: the left grip's motor is the heavy low-frequency one, the
   right grip's the light high-frequency one), as motor levels 0..1:
   - events (kicks): strength = the effect's magnitude x gain (the game's event magnitude x FORCE
     FEEDBACK INTENSITY), through a curve that makes a full-magnitude event at the game's default
     intensity (50%) full strength and still lets a scrape be felt; a tyre landing is held 90 ms
     and fades over 70 (the effect itself is a 35 ms pulse, too short for a pad motor), mostly on
     its side's motor; a crash is held 140 ms and fades over 160, on both;
   - the surface: its magnitude (the game's, up to 900 on gravel-like ground at the type's speed)
     x 3 on the low motor, half that on the high one; a slow period (200 ms and up: the sand's
     1 Hz wave, up to 2500) makes the low motor swell with the wave instead, x 1.5;
   - the engine (magnitude maxpow - ..., about 500 on the stock cars at the default intensity): a
     faint hum on the high motor, 1.6x (0.08 there: about where a pad's small motor starts);
   - the steering force and the friction are forces against a wheel's rim: nothing on a pad. */
#define KICK_HOLD_LAND  90.0            /* ms */
#define KICK_FADE_LAND  70.0
#define KICK_HOLD_CRASH 140.0
#define KICK_FADE_CRASH 160.0
#define KICK_FULL       0.5             /* the strength (magnitude x gain) that is full rumble */
#define KICK_CURVE      0.6
#define KICK_OTHER_SIDE 0.35            /* a tyre landing on the other grip's motor */
#define SURFACE_LEVEL   3.0
#define SURFACE_HIGH    0.5
#define SURFACE_SLOW_MS 200
#define SURFACE_SLOW_LEVEL 1.5
#define ENGINE_LEVEL    1.6
#define MOTOR_FLOOR     0.03            /* levels below this: off */
#define SEND_MS         250             /* every request ends by itself after this ... */
#define REFRESH_MS      120             /* ... and is renewed when less is left */
#define CHANGE_MIN      0.02            /* a smaller change is not sent */
#define SURFACE_TYPE    0x4c5288u       /* the game's current surface force-feedback type (for the log) */

#define CONT_RELEASE_MS 80.0            /* the surface and engine fade out with this time constant */
#define RACE_OVER       3               /* GAME_RACE_STATE: the race is over ("press Enter for results") */

/* per joystick: what was sent; the kicks and the continuous part as they fade */
typedef struct { double lo, hi, until, klo, khi, clo, chi, t; int on; } SlotOut;
static SlotOut out[INPUT_MAX_JOYS];

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static int effect_slot(const Imm *e)
{
    uint32_t dev = e->dev;
    if (e->kind == K_CONTAINED && e->parent >= 0)
        dev = imm[e->parent].dev;
    const Imm *d = dev ? imm_find(dev) : NULL;
    return d && d->kind == K_DEVICE ? d->slot : -1;
}

/* the waveform's absolute value at time t (ms) into it, 0..1 */
static double wave_abs(int wave, double t, uint32_t period, uint32_t phase)
{
    double x = fmod(t / period + phase / 360.0, 1.0);
    switch (wave) {
    case 0: return 1;                                   /* square */
    case 2: return fabs(1 - 4 * fabs(x - 0.5));         /* triangle */
    case 3: return fabs(2 * x - 1);                     /* sawtooth up */
    case 4: return fabs(1 - 2 * x);                     /* sawtooth down */
    default: return fabs(sin(2 * M_PI * x));            /* sine */
    }
}

/* the two motors' levels for one joystick, and what makes them (for the log) */
typedef struct { double klo, khi, clo, chi; } Mix;     /* kicks; continuous effects */

static Mix mix(int slot, double now, char *why, size_t n)
{
    double klo = 0, khi = 0, clo = 0, chi = 0;
    size_t w = 0;
    why[0] = 0;
    for (int i = 0; i < MAXIMM; i++) {
        Imm *e = &imm[i];
        if (!e->kind || !e->playing || effect_slot(e) != slot)
            continue;
        double t = now - e->started - e->delay;
        if (t < 0)
            continue;                                   /* start delay */
        if (e->kind == K_CONTAINED) {
            int crash = e->role == R_CRASH;
            double hold = crash ? KICK_HOLD_CRASH : KICK_HOLD_LAND, fade = crash ? KICK_FADE_CRASH : KICK_FADE_LAND;
            if (e->duration != INF && e->duration > hold)
                hold = e->duration;
            if (t >= hold + fade) {
                e->playing = 0;
                continue;
            }
            double s = fabs((double)e->mag) / 10000 * e->gain / 10000;
            double k = fmin(1, pow(s / KICK_FULL, KICK_CURVE)) * (t < hold ? 1 : 1 - (t - hold) / fade);
            double lo = e->role == R_LAND_R ? k * KICK_OTHER_SIDE : k;
            double hi = e->role == R_LAND_L ? k * KICK_OTHER_SIDE : e->role == R_KICK ? k * 0.5 : k;
            klo = fmax(klo, lo);
            khi = fmax(khi, hi);
            if (w < n)
                w += (size_t)snprintf(why + w, n - w, "%s%s %.2f", w ? ", " : "", role_names[e->role], s);
            continue;
        }
        if (e->duration != INF && t >= e->duration) {
            e->playing = 0;
            continue;
        }
        if (e->kind != K_PERIODIC)
            continue;                                   /* steering force, friction: nothing to feel */
        double m = clamp01((int32_t)e->mag / 10000.0);
        if (m <= 0 || !e->period)
            continue;
        if (e->role == R_ENGINE) {
            chi += m * ENGINE_LEVEL;
            if (w < n)
                w += (size_t)snprintf(why + w, n - w, "%sengine %d (%u ms)", w ? ", " : "", e->mag, e->period);
            continue;
        }
        if (e->period >= SURFACE_SLOW_MS) {
            clo += m * SURFACE_SLOW_LEVEL * wave_abs(e->wave, t, e->period, e->phase);
        } else {
            clo += m * SURFACE_LEVEL;
            chi += m * SURFACE_LEVEL * SURFACE_HIGH;
        }
        if (w < n)
            w += (size_t)snprintf(why + w, n - w, "%ssurface type %d: %d (%u ms)", w ? ", " : "",
                                  (int32_t)R32(SURFACE_TYPE), e->mag, e->period);
    }
    return (Mix){klo, khi, clamp01(clo), clamp01(chi)};
}

static void send(int slot, double lo, double hi, uint32_t ms, const char *why, int quiet)
{
    if (rlog && !quiet)
        LOGA("rumble frame %lu: joy%d low %.2f high %.2f for %u ms%s%s", frame_count, slot, lo, hi, ms,
             why[0] ? " <- " : "", why);
    input_joy_rumble(slot, (uint16_t)lround(lo * 65535), (uint16_t)lround(hi * 65535), ms);
}

/* once per frame: the motors follow what plays */
void ifc_frame(unsigned long frame)
{
    (void)frame;
    if (enhanced_on() <= 0)
        return;
    double now = ifc_clock();
    /* in a race (not a replay or the attract-mode demo) and not over, the controls driving the car */
    int on = option_bool("rumble", 1), driving = R32(GAME_DRIVING) == 1 && R32(GAME_RACE_MODE) == 1;
    int over = R32(GAME_RACE_STATE) == RACE_OVER, front = input_has_focus();
    pthread_mutex_lock(&ifc_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++) {
        SlotOut *o = &out[s];
        char why[256] = "";
        Mix m = mix(s, now, why, sizeof why);
        /* the surface flickers on and off as the wheels touch different ground: it fades out; a
           kick restarted by a lighter event (the game restarts the effect) fades out first */
        double dt = now - o->t, k = exp(-dt / CONT_RELEASE_MS), kd = dt / KICK_FADE_CRASH;
        o->t = now;
        o->klo = fmax(m.klo, o->klo - kd);
        o->khi = fmax(m.khi, o->khi - kd);
        if (o->clo * k > m.clo || o->chi * k > m.chi)
            snprintf(why + strlen(why), sizeof why - strlen(why), "%sfading", why[0] ? ", " : "");
        o->clo = fmax(m.clo, o->clo * k);
        o->chi = fmax(m.chi, o->chi * k);
        if (o->clo < MOTOR_FLOOR / 4) o->clo = 0;
        if (o->chi < MOTOR_FLOOR / 4) o->chi = 0;
        double lo = fmax(o->klo, o->clo), hi = fmax(o->khi, o->chi);
        lo = lo < MOTOR_FLOOR ? 0 : lo;
        hi = hi < MOTOR_FLOOR ? 0 : hi;
        /* RUMBLE off, out of the car (menus, pause, results, replays), the race over, the window
           behind another or the controller unplugged: nothing, at once */
        if (!on || !driving || over || !front || !input_joy_connected(s)) {
            if (o->on)
                snprintf(why, sizeof why, "%s", !on ? "RUMBLE off" : !driving ? "not driving" : over ? "race over" :
                         !front ? "window not in front" : "unplugged");
            lo = hi = o->klo = o->khi = o->clo = o->chi = 0;
        }
        if (!lo && !hi) {
            if (o->on)
                send(s, 0, 0, 0, why[0] ? why : "nothing playing", 0);
            o->on = 0;
            o->lo = o->hi = 0;
            continue;
        }
        int same = o->on && fabs(lo - o->lo) < CHANGE_MIN && fabs(hi - o->hi) < CHANGE_MIN;
        if (same && now + REFRESH_MS < o->until)
            continue;
        send(s, lo, hi, SEND_MS, why, same);           /* (renewals are not logged) */
        o->until = now + SEND_MS;
        o->on = 1;
        if (!same) {
            o->lo = lo;
            o->hi = hi;
        }
    }
    pthread_mutex_unlock(&ifc_lock);
}

/* at exit: every motor off */
void ifc_exit(void)
{
    pthread_mutex_lock(&ifc_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        if (out[s].on) {
            send(s, 0, 0, 0, "exit", 0);
            out[s].on = 0;
        }
    pthread_mutex_unlock(&ifc_lock);
}

/* ------------------------------------------------------------------ the game's side */
int ifc_rumble_wanted(void)
{
    return enhanced_on() && option_bool("rumble", 1);
}

void win_options(void);

/* FORCE FEEDBACK and RUMBLE. In a session with a controller the game's FORCE FEEDBACK line and
   the Enhanced RUMBLE are one switch: RUMBLE on sets FORCE FEEDBACK on (a stored "off" is from
   sessions in which no device could do it), RUMBLE off sets it off, and FORCE FEEDBACK changed
   in the controls menu changes RUMBLE. A session started with RUMBLE off keeps the game's own
   value until one of them is changed. */
static int armed, last_r, last_f;

static void link_arm(void)
{
    int r = ifc_rumble_wanted();
    armed = 1;
    last_r = r;
    if (r)
        W32(FF_ON, 1);
    last_f = R32(FF_ON) != 0;
    LOG(LOG_INPUT, "IFC21: FORCE FEEDBACK %s (RUMBLE %s)", last_f ? "on" : "off", r ? "on" : "off");
}

/* the game's settings are read (0x467d10, at start-up): FORCE FEEDBACK as RUMBLE says */
static int registry_load_hook(CPU *c)
{
    F_00467d10_orig(c);
    if (pad_count() > 0)
        link_arm();
    return 1;
}

static void link_poll(void)
{
    int r = ifc_rumble_wanted(), f = R32(FF_ON) != 0;
    if (r != last_r) {                          /* RUMBLE changed */
        W32(FF_ON, (uint32_t)r);
    } else if (f != last_f) {                   /* FORCE FEEDBACK changed in the controls menu */
        option_set("rumble", f ? "on" : "off");
        options_save();
        win_options();
        r = f;
        LOG(LOG_INPUT, "IFC21: FORCE FEEDBACK %s in the controls menu: RUMBLE %s", f ? "on" : "off", f ? "on" : "off");
    }
    last_r = r;
    last_f = R32(FF_ON) != 0;
}

/* The game plays force feedback only on the joystick its chosen layout steers with. With a
   keyboard or mouse layout chosen in a session with a controller (one plugged in later keeps
   the player's layout; padgame.c lets the controller drive beside it) and RUMBLE on, the game is
   shown the controller's joystick as the steering for its force-feedback start (0x44b460, the
   layout's entry put back after it) and its per-step steering force (0x441c30) takes the
   joystick way (the car's force for the effects every few steps) instead of pushing the
   keyboard's steering value. */
static int takeover(void)
{
    return armed && ifc_rumble_wanted() && R32(FF_ON) && (R32(GAME_LAYOUT) & 0xff) != 1;
}

/* the game's joystick to play on: a connected controller's, else one that was (-1: none) */
static int ff_joystick(void)
{
    int any = -1;
    uint32_t n = R32(JOY_COUNT);
    for (uint32_t i = 0; i < n && i < 4; i++) {
        uint32_t dev = R32(JOY_DEVS + 4 * i);
        int s = dev ? dinput_joy_slot(dev) : -1;
        if (s < 0 || !R32(FF_JOY + 4 * i))
            continue;
        if (input_joy_connected(s))
            return (int)i;
        if (any < 0)
            any = (int)i;
    }
    return any;
}

#define FF_STEER_COUNT  0x155ccacu      /* steps until the next steering force for the effects */
#define FF_STEER_EVERY  0x155d300u      /* ... every this many */
#define F_FF_STEER_SET  0x44b950u       /* (force, -0x8000, the car's force-feedback values) */

/* 0x441c30(force, car's force-feedback values), cdecl, per physics step of the player's car */
static int ff_steer_hook(CPU *c)
{
    if (!takeover())
        return 0;
    uint32_t n = R32(FF_STEER_COUNT) - 1;
    W32(FF_STEER_COUNT, n);
    if (!n) {
        guest_call(c, F_FF_STEER_SET, 3, R32(c->esp + 4), 0xffff8000u, R32(c->esp + 8));
        W32(FF_STEER_COUNT, R32(FF_STEER_EVERY));
    }
    c->ret = R32(c->esp);
    c->esp += 4;
    return 1;
}

/* 0x44b460(): force feedback starts (or goes on) on the layout's steering joystick */
static int ff_start_hook(CPU *c)
{
    int j = takeover() ? ff_joystick() : -1;
    if (j < 0)
        return 0;
    uint32_t keep[3] = {R32(GAME_LAYOUT), R32(GAME_LAYOUT + 4), R32(GAME_LAYOUT + 8)};
    W32(GAME_LAYOUT, 1);                        /* steer: joystick j's X axis */
    W32(GAME_LAYOUT + 4, (uint32_t)j);
    W32(GAME_LAYOUT + 8, 0);
    F_0044b460_orig(c);
    for (int k = 0; k < 3; k++)
        W32(GAME_LAYOUT + 4 * (uint32_t)k, keep[k]);
    LOG(LOG_INPUT, "IFC21: force feedback on the game's joystick %d beside a keyboard/mouse layout", j);
    return 1;
}

/* From the input poll in sessions with a controller. With RUMBLE on, every controller the game
   has is force-feedback capable to it (a controller plugged in later, or RUMBLE turned on during
   the session: the game only looks at start-up), and the game's force-feedback objects exist. */
void ifc_poll(struct CPU *c)
{
    if (!enhanced_on())
        return;
    if (!armed)
        link_arm();
    if (ifc_rumble_wanted()) {
        uint32_t n = R32(JOY_COUNT);
        for (uint32_t i = 0; i < n && i < 4; i++) {
            uint32_t dev = R32(JOY_DEVS + 4 * i);
            int s = dev ? dinput_joy_slot(dev) : -1;
            if (s < 0)
                continue;
            if (!dinput_dev_ff(dev))
                input_joy_set_ff(s, 1);
            if (!R32(FF_JOY + 4 * i)) {
                W32(FF_JOY + 4 * i, 1);
                W32(FF_FOUND, 1);
                LOG(LOG_INPUT, "IFC21: the game's joystick %u (joy%d) can do force feedback", i, s);
            }
        }
        if (R32(FF_FOUND) && !R32(FF_READY)) {
            guest_call(c, F_FF_OBJECTS, 0);
            LOG(LOG_INPUT, "IFC21: the game's force-feedback objects made");
        }
    }
    link_poll();
}

void ifc_init(void)
{
    base_vtbl = galloc(4 * 32);
    uint32_t t = thunk_addr(bad_virtual, "IFC21 base virtual");
    for (int i = 0; i < 32; i++)
        W32(base_vtbl + 4 * i, t);
    const char *v = getenv("INSANE_RUMBLE_LOG");
    rlog = v && *v && strcmp(v, "0");
    v = getenv("INSANE_LOG_INPUT_VERBOSE");
    verbose = v && *v && strcmp(v, "0");
    if (enhanced_on()) {                        /* (a plain --test run: the game as shipped) */
        hook_registry_load = registry_load_hook;
        hook_ff_steer = ff_steer_hook;
        hook_ff_start = ff_start_hook;
    }
}
