/* Test harness for runtime/dinput.c: drives it the way 1NSANE's code at 0x44aab0.. does. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "runtime.h"
#include "input.h"

/* ---------------- runtime stubs */
unsigned log_mask = LOG_ALWAYS | LOG_INPUT;
static int quiet;
void logmsg(unsigned cat, const char *fmt, ...)
{
    if (quiet && cat != LOG_ALWAYS) return;
    va_list ap; va_start(ap, fmt); printf("[%s] ", cat == LOG_ALWAYS ? "ALWAYS" : "input");
    vprintf(fmt, ap); printf("\n"); va_end(ap);
}
void fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); printf("FATAL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); exit(2);
}
#define LOW 0x10000000u
static uint32_t brk_ = LOW + 0x100000;
static pthread_mutex_t galloc_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t galloc(uint32_t size)
{
    pthread_mutex_lock(&galloc_lock);
    uint32_t p = brk_; brk_ += (size + 15) & ~15u;
    pthread_mutex_unlock(&galloc_lock);
    memset(gptr(p), 0, size);
    return p;
}
void gfree(uint32_t p) { (void)p; }
void gstrncpy(uint32_t dst, const char *src, uint32_t n)
{
    if (!n) return;
    uint32_t k = strlen(src); if (k >= n) k = n - 1;
    memcpy(gptr(dst), src, k); gptr(dst)[k] = 0;
}
double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void (*thunks[4096])(CPU *);
static const char *thunk_names[4096];
static int nthunks;
uint32_t thunk_addr(void (*fn)(CPU *c), const char *name)
{
    for (int i = 0; i < nthunks; i++) if (thunks[i] == fn) return 0x7f000000u + 16u * i;
    thunks[nthunks] = fn; thunk_names[nthunks] = name;
    return 0x7f000000u + 16u * nthunks++;
}
CPU cpu;
/* guest callbacks: fake addresses 0x60000000 + k */
typedef uint32_t (*Cb)(const uint32_t *args);
static Cb cbs[16];
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...)
{
    uint32_t args[16]; va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    assert(c == &cpu);
    assert(fn >= 0x60000000u && fn < 0x60000010u && cbs[fn - 0x60000000u]);
    uint32_t keep = cpu.esp, keep_ret = cpu.ret;
    uint32_t r = cbs[fn - 0x60000000u](args);
    cpu.esp = keep;
    cpu.ret = keep_ret;
    return r;
}

/* ---------------- calling methods like the game */
#define STACK_TOP (LOW + 0xf0000)
static int last_nargs_ok = 1;
static uint32_t call_fn(void (*fn)(CPU *), const char *name, int nargs, const uint32_t *args)
{
    uint32_t saved = cpu.esp, saved_ret = cpu.ret;
    uint32_t esp = (saved ? saved : STACK_TOP) - 1024;
    W32(esp, 0xdeadbeef);
    for (int i = 0; i < nargs; i++) W32(esp + 4 + 4 * i, args[i]);
    cpu.esp = esp;
    cpu.ret = 0;
    fn(&cpu);
    if (cpu.esp != esp + 4 + 4u * nargs || cpu.ret != 0xdeadbeef) {
        printf("ARG COUNT MISMATCH in %s: popped %d bytes, expected %d\n", name, (int)(cpu.esp - esp - 4), 4 * nargs);
        last_nargs_ok = 0;
        exit(1);
    }
    cpu.esp = saved;
    cpu.ret = saved_ret;
    return cpu.eax;
}
static void (*method(uint32_t obj, int idx))(CPU *)
{
    uint32_t t = R32(R32(obj) + 4 * idx);
    return thunks[(t - 0x7f000000u) / 16];
}
static uint32_t callm(uint32_t obj, int idx, int nargs, ...)
{
    uint32_t args[16]; args[0] = obj;
    va_list ap; va_start(ap, nargs);
    for (int i = 1; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    void (*fn)(CPU *) = method(obj, idx);
    return call_fn(fn, thunk_names[(R32(R32(obj) + 4 * idx) - 0x7f000000u) / 16], nargs, args);
}
extern void w_DINPUT_DirectInputCreateA(CPU *c);

/* ---------------- GUIDs in game memory */
static uint32_t mkguid(uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t *d4)
{
    uint32_t p = galloc(16);
    W32(p, d1); W16(p + 4, d2); W16(p + 6, d3); memcpy(gptr(p + 8), d4, 8);
    return p;
}
static const uint8_t DI4[8] = {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0, 0};
static const uint8_t FF4[8] = {0x9a, 0xd0, 0x00, 0xa0, 0xc9, 0xa0, 0x6e, 0x35};
static uint32_t g_kbd, g_mouse, g_dev2, g_key, g_x, g_y, g_z, g_const, g_sine;

/* vtable indices */
enum { QI, ADDREF, RELEASE, CREATEDEVICE, ENUMDEVICES, GETDEVICESTATUS };
enum { D_QI, D_ADDREF, D_RELEASE, D_GETCAPS, D_ENUMOBJ, D_GETPROP, D_SETPROP, D_ACQ, D_UNACQ, D_GETSTATE,
       D_GETDATA, D_SETFMT, D_SETEVENT, D_SETCOOP, D_GETOBJINFO, D_GETDEVINFO, D_RUNCPL, D_INIT, D_CREATEEFF,
       D_ENUMEFF, D_GETEFFINFO, D_GETFFSTATE, D_SENDFFCMD, D_ENUMCREATED, D_ESCAPE, D_POLL, D_SENDDATA };

static uint32_t pdi;
/* game globals emulated */
static uint32_t objfmt;      /* 0x1b19758: DIOBJECTDATAFORMAT[] */
static uint32_t guidtab;     /* 0x1b199e0 */
static int nobj, cur, naxes[4], nbut[4], npov[4], njoy;
static uint32_t joydev[4];
static char joyname[4][32];

static uint32_t cb_axis(const uint32_t *a)
{
    uint32_t inst = a[0];
    uint32_t g = guidtab + 16 * nobj;
    memcpy(gptr(g), gptr(inst + 4), 16);
    W32(objfmt + 16 * nobj, g); W32(objfmt + 16 * nobj + 4, 4 * nobj); W32(objfmt + 16 * nobj + 8, 0xffff02);
    W32(objfmt + 16 * nobj + 12, 0);
    nobj++;
    return ++naxes[cur] < 6;
}
static uint32_t cb_button(const uint32_t *a)
{
    uint32_t g = guidtab + 16 * nobj;
    memcpy(gptr(g), gptr(a[0] + 4), 16);
    W32(objfmt + 16 * nobj, g); W32(objfmt + 16 * nobj + 4, nbut[cur] + 0x18); W32(objfmt + 16 * nobj + 8, 0xffff0c);
    nobj++;
    return ++nbut[cur] < 16;
}
static uint32_t cb_pov(const uint32_t *a)
{
    uint32_t g = guidtab + 16 * nobj;
    memcpy(gptr(g), gptr(a[0] + 4), 16);
    W32(objfmt + 16 * nobj, g); W32(objfmt + 16 * nobj + 4, 4 * npov[cur] + 0x28); W32(objfmt + 16 * nobj + 8, 0xffff10);
    nobj++;
    return ++npov[cur] < 2;
}
/* the game's EnumDevices callback 0x44a710 */
static uint32_t cb_joy(const uint32_t *a)
{
    uint32_t ddi = a[0];
    uint32_t pdev = galloc(4), pdev2 = galloc(4);
    assert(R32(ddi) == 0x244);
    if (callm(pdi, CREATEDEVICE, 4, ddi + 4, pdev, 0)) return 1;
    uint32_t dev = R32(pdev);
    if (callm(dev, D_QI, 3, g_dev2, pdev2)) return 1;
    snprintf(joyname[njoy], 32, "%.31s", gstr(ddi + 0x28));
    printf("Found joystick: %s\n", joyname[njoy]);
    cur = njoy; naxes[cur] = nbut[cur] = npov[cur] = 0; nobj = 0;
    callm(dev, D_RELEASE, 1);
    uint32_t dev2 = R32(pdev2);
    callm(dev2, D_ENUMOBJ, 4, 0x60000001u, 0, 2);
    callm(dev2, D_ENUMOBJ, 4, 0x60000002u, 0, 4);
    callm(dev2, D_ENUMOBJ, 4, 0x60000003u, 0, 0x10);
    if (!nobj) { callm(dev2, D_RELEASE, 1); return 1; }
    joydev[njoy++] = dev2;
    return njoy < 4;
}

static int rumble_calls; static uint16_t rumble_lo, rumble_hi; static uint32_t rumble_ms;
static void on_rumble(int slot, uint16_t lo, uint16_t hi, uint32_t ms)
{
    rumble_calls++; rumble_lo = lo; rumble_hi = hi; rumble_ms = ms;
    printf("RUMBLE slot %d lo %u hi %u ms %u\n", slot, lo, hi, ms);
}

/* the game's 0x44a860 */
static int joy_setup(int i)
{
    uint32_t dev = joydev[i];
    uint32_t df = galloc(24);
    W32(df, 0x18); W32(df + 4, 0x10); W32(df + 8, 1); W32(df + 12, 0x30); W32(df + 16, nobj); W32(df + 20, objfmt);
    assert(callm(dev, D_SETFMT, 2, df) == 0);
    assert(callm(dev, D_SETCOOP, 3, 0x1234, 5) == 0);
    callm(dev, D_UNACQ, 1);
    uint32_t pr = galloc(24);
    for (int k = 0; k < naxes[i]; k++) {
        W32(pr, 0x18); W32(pr + 4, 0x10); W32(pr + 8, 4 * k); W32(pr + 12, 1); W32(pr + 16, 0xff810000); W32(pr + 20, 0x7f0000);
        if (callm(dev, D_SETPROP, 3, 4, pr)) printf("SetProperty(RANGE) failed (joy%d axis %d)\n", i, k);
    }
    for (int k = 0; k < naxes[i]; k++) {
        W32(pr, 0x14); W32(pr + 4, 0x10); W32(pr + 8, 4 * k); W32(pr + 12, 1); W32(pr + 16, 1000);
        if (callm(dev, D_SETPROP, 3, 5, pr)) printf("SetProperty(DEADZONE) failed (joy%d axis %d)\n", i, k);
    }
    uint32_t caps = galloc(0x2c);
    W32(caps, 0x2c);
    assert(callm(dev, D_GETCAPS, 2, caps) == 0);
    return (R32(caps + 4) & 0x100) != 0;
}

/* ---------------- sweep: every vtable entry pops the SDK's argument count */
static void sweep(uint32_t obj, const int *nargs, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t args[8] = {obj};
        void (*fn)(CPU *) = method(obj, i);
        const char *nm = thunk_names[(R32(R32(obj) + 4 * i) - 0x7f000000u) / 16];
        if (!strstr(nm, names[i])) { printf("NAME MISMATCH at %d: %s vs %s\n", i, nm, names[i]); exit(1); }
        if (!strcmp(names[i], "Release")) continue;           /* would free; AddRef covered */
        call_fn(fn, nm, nargs[i], args);
        if (!strcmp(names[i], "AddRef")) { uint32_t a2[1] = {obj}; call_fn(method(obj, 2), "Release", 1, a2); }
    }
    printf("sweep ok: %d methods\n", n);
}

static uint32_t kstate(uint32_t kbd, uint32_t buf, int dik)
{
    assert(callm(kbd, D_GETSTATE, 3, 256, buf) == 0);
    return R8(buf + dik);
}

static void *hammer(void *arg)
{
    (void)arg;
    for (int k = 0; k < 200000; k++) {
        input_joy_axis(0, k & 7, (k * 37) % 65536 - 32768);
        input_joy_button(0, k % 11, k & 1);
        input_mouse_move(1, -1);
        input_key(DIK_A, k & 1);
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *m = mmap((void *)(uintptr_t)LOW, 0x4000000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(m == (void *)(uintptr_t)LOW);
    setenv("INSANE_KEYS", "5:RETURN, 9:esc*2,12:DIK_LEFT,bogus,14:NOSUCHKEY", 1);
    cbs[0] = cb_joy; cbs[1] = cb_axis; cbs[2] = cb_button; cbs[3] = cb_pov;
    objfmt = galloc(0x408); guidtab = galloc(0x400);
    g_kbd = mkguid(0x6f1d2b61, 0xd5a0, 0x11cf, DI4);
    g_mouse = mkguid(0x6f1d2b60, 0xd5a0, 0x11cf, DI4);
    g_dev2 = mkguid(0x5944e682, 0xc92e, 0x11cf, DI4);
    g_key = mkguid(0x55728220, 0xd33c, 0x11cf, DI4);
    g_x = mkguid(0xa36d02e0, 0xc9f3, 0x11cf, DI4);
    g_y = mkguid(0xa36d02e1, 0xc9f3, 0x11cf, DI4);
    g_z = mkguid(0xa36d02e2, 0xc9f3, 0x11cf, DI4);
    g_const = mkguid(0x13541c20, 0x8e33, 0x11d0, FF4);
    g_sine = mkguid(0x13541c23, 0x8e33, 0x11d0, FF4);

    /* controllers plugged in before start-up (window layer) */
    assert(input_joy_attach("pad-serial-1", "Xbox Series X Controller", 6, 11, 1) == 0);
    assert(input_joy_attach("wheel-1", "Logitech G29 Driving Force Racing Wheel USB", 3, 25, 1) == 1);
    input_joy_set_ff(1, 1);
    input_set_rumble_handler(on_rumble);
    dinput_init();

    /* DirectInputCreateA(hinst, 0x700, &pdi, NULL) */
    uint32_t ppdi = galloc(4);
    uint32_t a4[4] = {0x400000, 0x700, ppdi, 0};
    assert(call_fn(w_DINPUT_DirectInputCreateA, "DirectInputCreateA", 4, a4) == 0);
    pdi = R32(ppdi);

    static const int di_n[] = {3, 1, 1, 4, 5, 2, 3, 3, 4, 5};
    static const char *const di_names[] = {"QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices",
        "GetDeviceStatus", "RunControlPanel", "Initialize", "FindDevice", "CreateDeviceEx"};
    quiet = 1; sweep(pdi, di_n, di_names, 10); quiet = 0;

    /* keyboard */
    uint32_t pk = galloc(4), pm = galloc(4);
    assert(callm(pdi, CREATEDEVICE, 4, g_kbd, pk, 0) == 0);
    uint32_t kbd = R32(pk);
    static const int dev_n[] = {3, 1, 1, 2, 4, 3, 3, 1, 1, 3, 5, 2, 2, 3, 4, 2, 3, 4, 5, 4, 3, 2, 2, 4, 2, 1, 5, 5, 5};
    static const char *const dev_names[] = {"QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
        "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat",
        "SetEventNotification", "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel",
        "Initialize", "CreateEffect", "EnumEffects", "GetEffectInfo", "GetForceFeedbackState",
        "SendForceFeedbackCommand", "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData",
        "EnumEffectsInFile", "WriteEffectToFile"};
    quiet = 1; sweep(kbd, dev_n, dev_names, 29); quiet = 0;
    assert(callm(kbd, D_SETCOOP, 3, 0x1234, 0x16) == 0);
    /* c_dfDIKeyboard as the game has it */
    uint32_t kodf = galloc(256 * 16), kdf = galloc(24);
    for (int i = 0; i < 256; i++) {
        W32(kodf + 16 * i, g_key); W32(kodf + 16 * i + 4, i); W32(kodf + 16 * i + 8, 0x8000000c | i << 8);
    }
    W32(kdf, 0x18); W32(kdf + 4, 0x10); W32(kdf + 8, 2); W32(kdf + 12, 256); W32(kdf + 16, 256); W32(kdf + 20, kodf);
    assert(callm(kbd, D_SETFMT, 2, kdf) == 0);
    uint32_t pr = galloc(32);
    W32(pr, 0x14); W32(pr + 4, 0x10); W32(pr + 8, 0); W32(pr + 12, 0); W32(pr + 16, 16);
    assert(callm(kbd, D_SETPROP, 3, 1, pr) == 0);

    /* mouse with c_dfDIMouse */
    assert(callm(pdi, CREATEDEVICE, 4, g_mouse, pm, 0) == 0);
    uint32_t mouse = R32(pm);
    assert(callm(mouse, D_SETCOOP, 3, 0x1234, 5) == 0);
    uint32_t modf = galloc(7 * 16), mdf = galloc(24);
    uint32_t mg[7] = {g_x, g_y, g_z, 0, 0, 0, 0}, mt[7] = {0xffff03, 0xffff03, 0x80ffff03, 0xffff0c, 0xffff0c, 0x80ffff0c, 0x80ffff0c};
    for (int i = 0; i < 7; i++) { W32(modf + 16 * i, mg[i]); W32(modf + 16 * i + 4, i < 3 ? 4 * i : 12 + i - 3); W32(modf + 16 * i + 8, mt[i]); }
    W32(mdf, 0x18); W32(mdf + 4, 0x10); W32(mdf + 8, 2); W32(mdf + 12, 16); W32(mdf + 16, 7); W32(mdf + 20, modf);
    assert(callm(mouse, D_SETFMT, 2, mdf) == 0);

    /* joysticks: EnumDevices(DIDEVTYPE_JOYSTICK, cb, 0, DIEDFL_ATTACHEDONLY) */
    assert(callm(pdi, ENUMDEVICES, 5, 4, 0x60000000u, 0, 1) == 0);
    assert(njoy == 2);
    printf("joy0: %d axes %d buttons %d povs; joy1: %d/%d/%d; shared format has %d objects\n",
           naxes[0], nbut[0], npov[0], naxes[1], nbut[1], npov[1], nobj);
    assert(naxes[0] == 6 && nbut[0] == 11 && npov[0] == 1);
    assert(naxes[1] == 3 && nbut[1] == 16 && npov[1] == 1);
    int ff0 = 0, ff1 = 0;
    /* the game builds ONE shared format (from the last joystick) for all: emulate exactly that */
    ff0 = joy_setup(0);
    ff1 = joy_setup(1);
    printf("ff: joy0 %d joy1 %d\n", ff0, ff1);
    assert(!ff0 && ff1);
    for (int i = 0; i < njoy; i++) assert(callm(joydev[i], D_ACQ, 1) == 0);
    assert(callm(mouse, D_ACQ, 1) == 0);
    assert(callm(kbd, D_ACQ, 1) == 0);
    assert(callm(kbd, D_ACQ, 1) == 1);     /* already acquired: S_FALSE */

    /* keyboard buffered + immediate */
    uint32_t kbuf = galloc(256), od = galloc(16 * 32), pn = galloc(4);
    input_key(DIK_RETURN, 1);
    input_key(DIK_RETURN, 1);              /* repeat: ignored */
    assert(kstate(kbd, kbuf, DIK_RETURN) == 0x80);
    W32(pn, 1);
    assert(callm(kbd, D_GETDATA, 5, 16, od, pn, 0) == 0);
    assert(R32(pn) == 1 && R32(od) == DIK_RETURN && R32(od + 4) == 0x80);
    W32(pn, 1);
    assert(callm(kbd, D_GETDATA, 5, 16, od, pn, 0) == 0 && R32(pn) == 0);
    input_key(DIK_RETURN, 0);
    assert(kstate(kbd, kbuf, DIK_RETURN) == 0);
    /* overflow + flush like 0x44b010 */
    for (int k = 0; k < 20; k++) input_key(DIK_SPACE, k % 2 == 0);
    W32(pn, 0xffffffff);
    assert(callm(kbd, D_GETDATA, 5, 16, 0, pn, 0) == 1);     /* DI_BUFFEROVERFLOW */
    assert(R32(pn) == 16);
    W32(pn, 0xffffffff);
    assert(callm(kbd, D_GETDATA, 5, 16, 0, pn, 0) == 0 && R32(pn) == 0);
    /* sequence numbers increase */
    input_key(DIK_W, 1); input_key(DIK_W, 0);
    W32(pn, 8);
    assert(callm(kbd, D_GETDATA, 5, 16, od, pn, 0) == 0 && R32(pn) == 2);
    assert(R32(od + 12) < R32(od + 16 + 12) && R32(od + 4) == 0x80 && R32(od + 16 + 4) == 0);

    /* mouse: relative motion since the last read */
    uint32_t ms = galloc(16);
    input_mouse_move(5, -3); input_mouse_move(2, 0); input_mouse_wheel(120); input_mouse_button(1, 1);
    assert(callm(mouse, D_GETSTATE, 3, 16, ms) == 0);
    printf("mouse: %d %d %d buttons %02x %02x\n", (int)R32(ms), (int)R32(ms + 4), (int)R32(ms + 8), R8(ms + 12), R8(ms + 13));
    assert((int)R32(ms) == 7 && (int)R32(ms + 4) == -3 && (int)R32(ms + 8) == 120 && R8(ms + 13) == 0x80);
    assert(callm(mouse, D_GETSTATE, 3, 16, ms) == 0);
    assert(R32(ms) == 0 && R32(ms + 4) == 0 && R32(ms + 8) == 0 && R8(ms + 13) == 0x80);
    assert(input_mouse_captured() == 1);
    assert(callm(mouse, D_GETSTATE, 3, 20, ms) == 0x80070057u);

    /* joystick state in the game's 48-byte layout, range -0x7f0000..0x7f0000, dead zone 10% */
    uint32_t js = galloc(48);
    assert(callm(joydev[0], D_POLL, 1) == 1);
    assert(callm(joydev[0], D_GETSTATE, 3, 48, js) == 0);
    printf("joy0 rest: X %d Y %d POV %08x\n", (int)R32(js), (int)R32(js + 4), R32(js + 0x28));
    assert(R32(js) == 0 && R32(js + 0x28) == 0xffffffffu);
    input_joy_axis(0, INPUT_AXIS_X, 32767);
    input_joy_axis(0, INPUT_AXIS_Y, -32768);
    input_joy_axis(0, INPUT_AXIS_RX, 3000);      /* inside the dead zone */
    input_joy_axis(0, INPUT_AXIS_RY, 16384);
    input_joy_button(0, 0, 1);
    input_joy_button(0, 10, 1);
    input_joy_pov(0, 0, 9000);
    assert(callm(joydev[0], D_GETSTATE, 3, 48, js) == 0);
    printf("joy0: X %d Y %d Rx %d Ry %d b0 %02x b10 %02x POV %u\n", (int)R32(js), (int)R32(js + 4), (int)R32(js + 12),
           (int)R32(js + 16), R8(js + 0x18), R8(js + 0x18 + 10), R32(js + 0x28));
    assert((int)R32(js) == 0x7f0000 && (int)R32(js + 4) == -0x7f0000 && R32(js + 12) == 0);
    assert(R8(js + 0x18) == 0x80 && R8(js + 0x18 + 10) == 0x80 && R32(js + 0x28) == 9000);
    /* joy1 (wheel, 3 axes) got the shared format built from itself; joy0's extra axes are just absent */
    input_joy_axis(1, INPUT_AXIS_X, -16384);
    assert(callm(joydev[1], D_GETSTATE, 3, 48, js) == 0);
    printf("joy1: X %d\n", (int)R32(js));

    /* unplug / replug keeps the slot */
    input_joy_disconnect(0);
    assert(callm(joydev[0], D_GETSTATE, 3, 48, js) == 0x8007001eu);
    assert(callm(joydev[0], D_ACQ, 1) == 0x80040209u);
    assert(callm(joydev[0], D_GETSTATE, 3, 48, js) == 0x8007000cu);
    input_joy_attach("some-other-pad", "Other pad", 4, 10, 1);   /* goes to slot 2, not 0 */
    assert(input_joy_connected(2) && !input_joy_connected(0));
    assert(input_joy_attach("pad-serial-1", "Xbox Series X Controller", 6, 11, 1) == 0);
    assert(callm(joydev[0], D_ACQ, 1) == 0);
    assert(callm(joydev[0], D_GETSTATE, 3, 48, js) == 0);
    assert(R32(js) == 0 && R32(js + 0x28) == 0xffffffffu);

    /* properties read back */
    W32(pr, 0x18); W32(pr + 4, 0x10); W32(pr + 8, 4); W32(pr + 12, 1);
    assert(callm(joydev[0], D_GETPROP, 3, 4, pr) == 0 && (int)R32(pr + 16) == -0x7f0000);
    W32(pr, 0x218 );
    uint32_t ps = galloc(0x218);
    W32(ps, 0x218); W32(ps + 4, 0x10); W32(ps + 8, 0); W32(ps + 12, 0);
    assert(callm(joydev[1], D_GETPROP, 3, 14, ps) == 0 && R16(ps + 16) == 'L');
    assert(callm(joydev[1], D_GETPROP, 3, 99, ps) == 0x80004001u);

    /* GetDeviceInfo / GetObjectInfo */
    uint32_t di = galloc(0x244);
    W32(di, 0x244);
    assert(callm(joydev[1], D_GETDEVINFO, 2, di) == 0);
    printf("joy1 info: \"%s\" type %x\n", gstr(di + 0x28), R32(di + 0x24));
    uint32_t oi = galloc(0x13c);
    W32(oi, 0x13c);
    assert(callm(joydev[0], D_GETOBJINFO, 4, oi, 0x28, 1) == 0);
    printf("object at 0x28: \"%s\" type %x\n", gstr(oi + 0x20), R32(oi + 0x18));

    /* force feedback on the wheel: constant force, then a sine */
    uint32_t eff = galloc(0x38), cf = galloc(4), pe = galloc(4), axes = galloc(8), dirs = galloc(8);
    W32(cf, 5000); W32(axes, 0); W32(dirs, 0);
    W32(eff, 0x38); W32(eff + 4, 0x12); W32(eff + 8, 1000000); W32(eff + 0x10, 10000); W32(eff + 0x14, 0xffffffff);
    W32(eff + 0x1c, 1); W32(eff + 0x20, axes); W32(eff + 0x24, dirs); W32(eff + 0x2c, 4); W32(eff + 0x30, cf);
    assert(callm(joydev[0], D_CREATEEFF, 5, g_const, eff, pe, 0) == 0x80004001u);
    assert(callm(joydev[1], D_CREATEEFF, 5, g_const, eff, pe, 0) == 0);
    uint32_t e1 = R32(pe);
    static const int eff_n[] = {3, 1, 1, 4, 2, 3, 3, 3, 1, 2, 1, 1, 2};
    static const char *const eff_names[] = {"QueryInterface", "AddRef", "Release", "Initialize", "GetEffectGuid",
        "GetParameters", "SetParameters", "Start", "Stop", "GetEffectStatus", "Download", "Unload", "Escape"};
    quiet = 1; sweep(e1, eff_n, eff_names, 13); quiet = 0;
    assert(callm(e1, 7, 3, 1, 0) == 0);          /* Start */
    assert(rumble_calls && rumble_lo > 32000 && rumble_lo < 33000 && rumble_ms > 900);
    uint32_t pst = galloc(4);
    assert(callm(e1, 9, 2, pst) == 0 && R32(pst) == 1);
    uint32_t per = galloc(16);
    W32(per, 8000); W32(per + 4, 0); W32(per + 8, 0); W32(per + 12, 20000);
    W32(eff + 0x2c, 16); W32(eff + 0x30, per);
    assert(callm(joydev[1], D_CREATEEFF, 5, g_sine, eff, pe, 0) == 0);
    uint32_t e2 = R32(pe);
    assert(callm(e2, 7, 3, 1, 0) == 0);
    assert(rumble_hi > 52000);
    assert(callm(joydev[1], D_SENDFFCMD, 2, 2) == 0);    /* STOPALL */
    assert(rumble_lo == 0 && rumble_hi == 0);
    /* game's autocenter toggling 0x44b880 */
    W32(pr, 0x14); W32(pr + 4, 0x10); W32(pr + 8, 0); W32(pr + 12, 0); W32(pr + 16, 1);
    callm(joydev[1], D_UNACQ, 1);
    assert(callm(joydev[1], D_SETPROP, 3, 9, pr) == 0);
    assert(callm(joydev[1], D_ACQ, 1) == 0);
    assert(dinput_joy_slot(joydev[1]) == 1 && dinput_joy_slot(kbd) == -1);
    callm(e1, RELEASE, 1);
    callm(e2, RELEASE, 1);

    /* scripted keys */
    for (unsigned long f = 1; f <= 16; f++) {
        input_frame(f);
        uint32_t ret = kstate(kbd, kbuf, DIK_RETURN), esc = kstate(kbd, kbuf, DIK_ESCAPE), left = kstate(kbd, kbuf, DIK_LEFT);
        printf("frame %2lu: RETURN %02x ESC %02x LEFT %02x\n", f, ret, esc, left);
        if (f >= 5 && f < 8) assert(ret == 0x80); else assert(ret == 0);
        if (f >= 9 && f < 11) assert(esc == 0x80); else assert(esc == 0);
    }

    /* focus loss releases keys */
    input_key(DIK_UP, 1);
    input_focus(0);
    assert(kstate(kbd, kbuf, DIK_UP) == 0);

    /* names */
    assert(input_dik_from_name("enter") == DIK_RETURN && input_dik_from_name("0x1c") == DIK_RETURN);
    assert(input_dik_from_name("1") == DIK_1 && input_dik_from_name("F12") == DIK_F12);
    assert(input_dik_from_hid(40) == DIK_RETURN && input_dik_from_hid(82) == DIK_UP && input_dik_from_hid(4) == DIK_A);

    /* threads: hammer the host side while the game side reads */
    quiet = 1;
    pthread_t th;
    pthread_create(&th, NULL, hammer, NULL);
    for (int k = 0; k < 20000; k++) {
        callm(joydev[0], D_GETSTATE, 3, 48, js);
        callm(mouse, D_GETSTATE, 3, 16, ms);
        W32(pn, 8);
        callm(kbd, D_GETDATA, 5, 16, od, pn, 0);
    }
    pthread_join(th, NULL);
    quiet = 0;

    /* release everything like 0x44ad00 */
    callm(mouse, D_UNACQ, 1); assert(callm(mouse, D_RELEASE, 1) == 0);
    callm(kbd, D_UNACQ, 1); assert(callm(kbd, D_RELEASE, 1) == 0);
    for (int i = 0; i < njoy; i++) { callm(joydev[i], D_UNACQ, 1); assert(callm(joydev[i], D_RELEASE, 1) == 0); }
    assert(callm(pdi, RELEASE, 1) == 0);
    printf("ALL TESTS PASSED\n");
    return 0;
}
