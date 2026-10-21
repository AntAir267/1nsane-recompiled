/* Harness for runtime/dsound.c: stubs main.c's pieces, drives the COM vtables like Game.exe. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "runtime.h"
#include "sound.h"

unsigned log_mask = LOG_ALWAYS;
static int quiet;
void logmsg(unsigned cat, const char *fmt, ...)
{
    if (quiet && !(cat & LOG_ALWAYS)) return;
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap); fputc('\n', stdout);
}
void fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); fprintf(stdout, "FATAL: "); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout); _exit(2);
}
static double vclock;
static struct timespec t0;
double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) * 1000.0 + (t.tv_nsec - t0.tv_nsec) / 1e6;
}
static double virt(void) { return vclock; }
CPU *current_cpu(void) { return NULL; }
static void (*fns[4096])(CPU *);
static const char *names[4096];
static int nf;
uint32_t thunk_addr(void (*fn)(CPU *c), const char *name)
{
    for (int i = 0; i < nf; i++) if (fns[i] == fn) return THUNK_BASE + 16u * i;
    fns[nf] = fn; names[nf] = name; return THUNK_BASE + 16u * nf++;
}
extern void w_DSOUND_1(CPU *c);

static CPU cpu;
static uint32_t stack;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("CHECK FAILED line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t callfn(void (*fn)(CPU *), int nargs, const uint32_t *a)
{
    uint32_t esp = stack + 0x8000;
    for (int i = nargs - 1; i >= 0; i--) { esp -= 4; W32(esp, a[i]); }
    esp -= 4; W32(esp, 0x12345678);
    cpu.esp = esp;
    fn(&cpu);
    if (cpu.esp != esp + 4 + 4 * (uint32_t)nargs || cpu.ret != 0x12345678)
        { fails++; printf("stack imbalance after call (%d args)\n", nargs); }
    return cpu.eax;
}
static uint32_t com(uint32_t obj, int idx, int nargs, ...)
{
    uint32_t a[9]; a[0] = obj;
    va_list ap; va_start(ap, nargs);
    for (int i = 1; i < nargs; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t t = R32(R32(obj) + 4 * idx);
    return callfn(fns[(t - THUNK_BASE) >> 4], nargs, a);
}
enum { QI, ADDREF, RELEASE, CREATESB = 3, DSGETCAPS, DUP, SETCOOP, COMPACT, GETSPK, SETSPK, DSINIT };
enum { B_GETCAPS = 3, GETPOS, GETFMT, GETVOL, GETPAN, GETFREQ, GETSTATUS, B_INIT, LOCK, PLAY, SETPOS,
       SETFMT, SETVOL, SETPAN, SETFREQ, STOP, UNLOCK, RESTORE };

static uint32_t mkguid(uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t *d4)
{
    uint32_t g = galloc(16); W32(g, d1); W16(g + 4, d2); W16(g + 6, d3);
    for (int i = 0; i < 8; i++) W8(g + 8 + i, d4[i]);
    return g;
}
static uint32_t mkwfx(int ch, int rate, int bits)
{
    uint32_t w = galloc(18);
    W16(w, 1); W16(w + 2, ch); W32(w + 4, rate); W32(w + 8, rate * ch * bits / 8);
    W16(w + 12, ch * bits / 8); W16(w + 14, bits); W16(w + 16, 0);
    return w;
}
static uint32_t mkdesc(uint32_t flags, uint32_t bytes, uint32_t wfx)
{
    uint32_t d = galloc(36); W32(d, 36); W32(d + 4, flags); W32(d + 8, bytes); W32(d + 16, wfx);
    return d;
}
static uint32_t v32;   /* program-memory scratch */
static void pos(uint32_t b, uint32_t *play, uint32_t *write)
{
    com(b, GETPOS, 3, v32, v32 + 4);
    *play = R32(v32); *write = R32(v32 + 4);
}
static uint32_t status(uint32_t b) { com(b, GETSTATUS, 2, v32); return R32(v32); }

/* amplitude/SNR of a sine fit at frequency f over n stereo frames (left channel) */
static void sine_fit(const float *x, int n, double f, int rate, double *amp, double *snr_db)
{
    double sc = 0, cc = 0;
    for (int i = 0; i < n; i++) { double ph = 2 * M_PI * f * i / rate; sc += x[2 * i] * sin(ph); cc += x[2 * i] * cos(ph); }
    sc *= 2.0 / n; cc *= 2.0 / n;
    double err = 0, sig = 0;
    for (int i = 0; i < n; i++) {
        double ph = 2 * M_PI * f * i / rate, fit = sc * sin(ph) + cc * cos(ph);
        err += (x[2 * i] - fit) * (x[2 * i] - fit); sig += fit * fit;
    }
    *amp = sqrt(sc * sc + cc * cc);
    *snr_db = 10 * log10(sig / (err + 1e-30));
}

int main(int argc, char **argv)
{
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (getenv("VERBOSE")) log_mask |= LOG_SOUND;
    mem_init();
    stack = galloc(0x10000);
    v32 = galloc(64);
    dsound_init();
    int wavmode = argc > 1 && !strcmp(argv[1], "wav");
    if (!wavmode) sound_set_clock(virt);

    static const uint8_t dsclsid4[8] = {0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0, 0};
    static const uint8_t ds4[8] = {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60};
    uint32_t clsid = mkguid(0x47d4d946, 0x62e8, 0x11cf, dsclsid4), iid = mkguid(0x279afa83, 0x4981, 0x11ce, ds4);
    uint32_t pp = galloc(4);
    if (wavmode) {
        uint32_t a3[3] = {0, pp, 0}, lk = galloc(16), play, write;
        callfn(w_DSOUND_1, 3, a3);
        uint32_t d = R32(pp);
        com(d, SETCOOP, 3, 0x1234, 2);
        usleep(300 * 1000);       /* silence first */
        CHECK(com(d, CREATESB, 4, mkdesc(0xe0, 11025, mkwfx(1, 11025, 8)), pp, 0) == 0, "wav sfx");
        uint32_t s = R32(pp);
        com(s, LOCK, 8, 0, 11025, lk, lk + 4, 0, 0, 0);
        for (uint32_t i = 0; i < 11025; i++)
            W8(R32(lk) + i, (uint8_t)lrint(128 + 63.5 * sin(2 * M_PI * 1000.0 * i / 11025)));
        com(s, PLAY, 4, 0, 0, 1);
        usleep(1000 * 1000);
        com(s, SETFREQ, 2, 22050);  /* an octave up for half a second */
        usleep(500 * 1000);
        pos(s, &play, &write);
        com(s, STOP, 1);
        printf("after 1.5 s: play %u write %u\n", play, write);
        usleep(300 * 1000);
        exit(fails ? 1 : 0);
    }

    /* --- the game's main path: CoCreateInstance + Initialize + PRIORITY + primary */
    CHECK(dsound_cocreate(iid, iid, pp) == 0x80040154u, "non-DS clsid must be CLASSNOTREG");
    CHECK(dsound_cocreate(clsid, iid, pp) == 0, "cocreate");
    uint32_t ds = R32(pp);
    uint32_t prim_desc = mkdesc(1, 0, 0);
    CHECK(com(ds, CREATESB, 4, prim_desc, pp, 0) == 0x887800aau, "uninitialized must fail");
    CHECK(com(ds, DSINIT, 2, 0) == 0, "Initialize");
    CHECK(com(ds, DSINIT, 2, 0) == 0x88780082u, "second Initialize");
    CHECK(com(ds, SETCOOP, 3, 0x1234, 2) == 0, "coop");
    CHECK(com(ds, CREATESB, 4, prim_desc, pp, 0) == 0, "primary");
    uint32_t prim = R32(pp);
    uint32_t pf = mkwfx(2, 22050, 16);
    CHECK(com(prim, SETFMT, 2, pf) == 0, "SetFormat");
    CHECK(com(prim, PLAY, 4, 0, 0, 0) == 0x80070057u, "primary play without loop");
    CHECK(com(prim, PLAY, 4, 0, 0, 1) == 0, "primary play");
    CHECK(com(prim, SETVOL, 2, -100) == 0x8878001eu, "primary has no CTRLVOLUME");
    uint32_t fbuf = galloc(64);
    CHECK(com(prim, GETFMT, 4, fbuf, 64, v32) == 0 && R32(v32) == 18 && R32(fbuf + 4) == 22050, "primary GetFormat");
    W32(fbuf, 20);
    CHECK(com(prim, B_GETCAPS, 2, fbuf) == 0 && (R32(fbuf + 4) & 1), "primary caps");
    CHECK(com(prim, LOCK, 8, 0, 4, v32, v32 + 4, 0, 0, 0) == 0x88780046u, "primary lock");

    /* --- SFX: 11025 Hz mono 8-bit, STATIC|CTRLFREQUENCY|CTRLPAN|CTRLVOLUME|GETCURRENTPOSITION2 */
    uint32_t sfx_fmt = mkwfx(1, 11025, 8);
    const uint32_t sfx_bytes = 11025;      /* one second */
    CHECK(com(ds, CREATESB, 4, mkdesc(0x100e2, sfx_bytes, sfx_fmt), pp, 0) == 0, "sfx create");
    uint32_t sfx = R32(pp);
    uint32_t lk = galloc(16);
    CHECK(com(sfx, LOCK, 8, 0, sfx_bytes, lk, lk + 4, lk + 8, lk + 12, 0) == 0, "sfx lock");
    CHECK(R32(lk + 4) == sfx_bytes && R32(lk + 8) == 0 && R32(lk + 12) == 0, "sfx lock regions");
    uint32_t mem = R32(lk);
    for (uint32_t i = 0; i < sfx_bytes; i++)        /* 1 kHz sine, amplitude 0.5 */
        W8(mem + i, (uint8_t)lrint(128 + 63.5 * sin(2 * M_PI * 1000.0 * i / 11025)));
    CHECK(com(sfx, UNLOCK, 5, mem, sfx_bytes, 0, 0) == 0, "sfx unlock");
    CHECK(com(sfx, UNLOCK, 5, mem + 5, sfx_bytes, 0, 0) == 0x80070057u, "unlock outside");
    /* duplicate shares memory */
    CHECK(com(ds, DUP, 3, sfx, pp) == 0, "dup");
    uint32_t dup = R32(pp);
    CHECK(com(dup, LOCK, 8, 100, 10, lk, lk + 4, 0, 0, 0) == 0 && R32(lk) == mem + 100, "dup shares memory");
    CHECK(com(ds, DUP, 3, prim, pp) == 0x88780032u, "dup primary");
    /* play once, cursors follow the clock */
    vclock = 1000;
    CHECK(com(sfx, SETPOS, 2, 0) == 0, "setpos");
    CHECK(com(sfx, PLAY, 4, 0, 0, 0) == 0, "play");
    CHECK(status(sfx) == 1, "playing status %x", status(sfx));
    uint32_t play, write;
    vclock = 1100;     /* 100 ms -> 1102.5 frames */
    pos(sfx, &play, &write);
    CHECK(play == 1102, "sfx play cursor after 100 ms: %u", play);
    CHECK(write == 1102 + 166 + 17, "sfx write lead 15 ms = 166 frames + 17 look-ahead: %u", write);
    CHECK(com(sfx, SETFREQ, 2, 22050) == 0, "setfreq");
    vclock = 1200;     /* +100 ms at 22050 -> +2205 */
    pos(sfx, &play, &write);
    CHECK(play == 1102 + 2205 || play == 1103 + 2205, "after freq change: %u", play);
    CHECK(com(sfx, GETFREQ, 2, v32) == 0 && R32(v32) == 22050, "getfreq");
    CHECK(com(sfx, SETFREQ, 2, 0) == 0 && com(sfx, GETFREQ, 2, v32) == 0 && R32(v32) == 11025, "freq 0 = original");
    CHECK(com(sfx, SETFREQ, 2, 50) == 0x80070057u, "freq too low");
    vclock = 3000;     /* past the end: one-shot stops */
    CHECK(status(sfx) == 0, "stopped at end, status %x", status(sfx));
    pos(sfx, &play, &write);
    CHECK(play == 0 && write == 0, "one-shot at end: %u %u", play, write);
    /* volume/pan validation */
    CHECK(com(sfx, SETVOL, 2, 1) == 0x80070057u, "vol > 0");
    CHECK(com(sfx, SETVOL, 2, (uint32_t)-10001) == 0x80070057u, "vol < -10000");
    CHECK(com(sfx, SETPAN, 2, 10001) == 0x80070057u, "pan > 10000");
    CHECK(com(sfx, SETVOL, 2, (uint32_t)-600) == 0 && com(sfx, GETVOL, 2, v32) == 0 && (int32_t)R32(v32) == -600, "vol");

    /* --- music: 44100 stereo 16-bit, 1.2 s, CTRLFREQUENCY|CTRLPAN|CTRLVOLUME, looping */
    uint32_t mus_bytes = 176400 * 120 / 100;
    CHECK(com(ds, CREATESB, 4, mkdesc(0xe0, mus_bytes, mkwfx(2, 44100, 16)), pp, 0) == 0, "music create");
    uint32_t mus = R32(pp);
    vclock = 10000;
    com(mus, SETVOL, 2, 0);
    com(mus, SETPOS, 2, 0);
    CHECK(com(mus, PLAY, 4, 0, 0, 1) == 0, "music play");
    CHECK(status(mus) == 5, "looping status %x", status(mus));
    vclock = 10000 + 1300;  /* 1.3 s: wrapped once -> 0.1 s = 4410 frames */
    pos(mus, &play, &write);
    CHECK(play == 4410 * 4, "music wrapped: %u", play);
    CHECK(write == (4410 + 662 + 17) * 4, "music write lead 662 + 17 frames: %u", write);
    /* Lock across the end: two regions */
    CHECK(com(mus, LOCK, 8, mus_bytes - 400, 1000, lk, lk + 4, lk + 8, lk + 12, 0) == 0, "wrap lock");
    uint32_t mm = R32(lk) - (mus_bytes - 400);
    CHECK(R32(lk + 4) == 400 && R32(lk + 8) == mm && R32(lk + 12) == 600, "wrap regions %u %08x %u",
          R32(lk + 4), R32(lk + 8), R32(lk + 12));
    CHECK(com(mus, LOCK, 8, 0, 4, lk, lk + 4, 0, 0, 1) == 0 && R32(lk) == mm + write, "FROMWRITECURSOR");
    CHECK(com(mus, LOCK, 8, 5, 0, lk, lk + 4, lk + 8, lk + 12, 2) == 0 && R32(lk + 4) == mus_bytes - 5 && R32(lk + 12) == 5,
          "ENTIREBUFFER from 5: %u + %u", R32(lk + 4), R32(lk + 12));
    CHECK(com(mus, LOCK, 8, mus_bytes, 4, lk, lk + 4, 0, 0, 0) == 0x80070057u, "lock past end");
    com(mus, STOP, 1);
    pos(mus, &play, &write);
    CHECK(play == write, "stopped: write == play");

    /* --- the game's music refill loop (timer, 100 ms): play cursor must step ~17640 bytes */
    vclock = 20000;
    com(mus, SETPOS, 2, 0);
    com(mus, PLAY, 4, 0, 0, 1);
    uint32_t last = 0, total = 0;
    for (int t = 1; t <= 50; t++) {
        vclock = 20000 + 100.0 * t;
        pos(mus, &play, &write);
        uint32_t n = play >= last ? play - last : mus_bytes - last + play;
        total += n;
        if (n) {
            CHECK(com(mus, LOCK, 8, last, n, lk, lk + 4, lk + 8, lk + 12, 0) == 0, "refill lock");
            com(mus, UNLOCK, 5, R32(lk), R32(lk + 4), R32(lk + 8), R32(lk + 12));
        }
        last = play;
    }
    CHECK(total == 5 * 176400, "5 s of music consumed: %u", total);

    /* --- mixer: attach an output and check levels */
    quiet = 1;
    com(mus, STOP, 1);
    com(sfx, SETFREQ, 2, 0);
    com(sfx, SETVOL, 2, 0);
    com(sfx, SETPAN, 2, 0);
    com(sfx, SETPOS, 2, 0);
    sound_output_attach(48000, 256);
    com(sfx, PLAY, 4, 0, 0, 1);
    enum { N = 24000 };
    static float out[2 * 60000];
    sound_mix(out, N, 48000);
    double amp, snr;
    sine_fit(out + 2 * 2000, N - 4000, 1000, 48000, &amp, &snr);
    printf("11025->48000, 1 kHz: amplitude %.4f (want ~0.496), SNR %.1f dB\n", amp, snr);
    CHECK(fabs(amp - 63.5 / 128) < 0.01 && snr > 40, "resampled sine");
    /* a 4 kHz tone: closer to the 11025 Hz Nyquist */
    for (uint32_t i = 0; i < sfx_bytes; i++)
        W8(mem + i, (uint8_t)lrint(128 + 63.5 * sin(2 * M_PI * 4000.0 * i / 11025)));
    sound_mix(out, N, 48000);
    sine_fit(out + 2 * 2000, N - 4000, 4000, 48000, &amp, &snr);
    printf("11025->48000, 4 kHz: amplitude %.4f, SNR %.1f dB\n", amp, snr);
    for (uint32_t i = 0; i < sfx_bytes; i++)
        W8(mem + i, (uint8_t)lrint(128 + 63.5 * sin(2 * M_PI * 1000.0 * i / 11025)));
    /* volume -600 mB = 0.501, pan +2000 -> left additionally -2000 mB = 0.1 */
    com(sfx, SETVOL, 2, (uint32_t)-600);
    com(sfx, SETPAN, 2, 2000);
    sound_mix(out, N, 48000);
    double ampr, snrr;
    sine_fit(out + 2 * 2000, N - 4000, 1000, 48000, &amp, &snr);
    sine_fit(out + 2 * 2000 + 1, N - 4000, 1000, 48000, &ampr, &snrr);
    printf("vol -600 pan +2000: left %.4f right %.4f (want %.4f %.4f)\n", amp, ampr,
           0.496 * pow(10, -2600 / 2000.0), 0.496 * pow(10, -600 / 2000.0));
    CHECK(fabs(ampr / (63.5 / 128) - pow(10, -0.3)) < 0.01 && fabs(amp / ampr - 0.1) < 0.005, "vol/pan law");
    /* pitch up beyond the output rate (stretched kernel path) */
    com(sfx, SETVOL, 2, 0);
    com(sfx, SETPAN, 2, 0);
    com(sfx, SETFREQ, 2, 11025 * 6);   /* 1 kHz -> 6 kHz at 66150 Hz input into 48000 */
    sound_mix(out, N, 48000);
    sine_fit(out + 2 * 2000, N - 4000, 6000, 48000, &amp, &snr);
    printf("66150->48000 (downsampling), 6 kHz: amplitude %.4f, SNR %.1f dB\n", amp, snr);
    CHECK(fabs(amp - 0.496) < 0.02 && snr > 35, "downsampled sine");
    com(sfx, STOP, 1);
    /* resampler quality on a 16-bit source */
    {
        CHECK(com(ds, CREATESB, 4, mkdesc(0xe0, 22050 * 2, mkwfx(1, 11025, 16)), pp, 0) == 0, "16-bit sfx");
        uint32_t s16 = R32(pp);
        com(s16, LOCK, 8, 0, 0, lk, lk + 4, 0, 0, 2);
        double tones[] = {1000, 3000, 4500};
        for (int k = 0; k < 3; k++) {
            for (uint32_t i = 0; i < 22050; i++)
                W16(R32(lk) + 2 * i, (uint16_t)(int16_t)lrint(16000 * sin(2 * M_PI * tones[k] * i / 11025)));
            com(s16, SETPOS, 2, 0);
            com(s16, PLAY, 4, 0, 0, 1);
            sound_mix(out, N, 48000);
            sine_fit(out + 2 * 2000, N - 4000, tones[k], 48000, &amp, &snr);
            printf("16-bit 11025->48000, %.0f Hz: amplitude %.4f (want %.4f), SNR %.1f dB\n", tones[k], amp,
                   16000 / 32768.0, snr);
            com(s16, STOP, 1);
        }
        com(s16, RELEASE, 1);
        CHECK(com(ds, CREATESB, 4, mkdesc(0xe0, 176400, mkwfx(2, 44100, 16)), pp, 0) == 0, "44k");
        uint32_t s44 = R32(pp);
        com(s44, LOCK, 8, 0, 0, lk, lk + 4, 0, 0, 2);
        double t44[] = {1000, 10000, 16000};
        for (int k = 0; k < 3; k++) {
            for (uint32_t i = 0; i < 44100; i++) {
                int16_t v = (int16_t)lrint(16000 * sin(2 * M_PI * t44[k] * i / 44100));
                W16(R32(lk) + 4 * i, (uint16_t)v); W16(R32(lk) + 4 * i + 2, (uint16_t)v);
            }
            com(s44, SETPOS, 2, 0);
            com(s44, PLAY, 4, 0, 0, 1);
            sound_mix(out, N, 48000);
            sine_fit(out + 2 * 2000, N - 4000, t44[k], 48000, &amp, &snr);
            printf("16-bit 44100->48000, %.0f Hz: amplitude %.4f, SNR %.1f dB\n", t44[k], amp, snr);
            com(s44, STOP, 1);
        }
        com(s44, RELEASE, 1);
    }
    /* music bit-exact at 44100 */
    uint32_t mmem;
    com(mus, LOCK, 8, 0, 0, lk, lk + 4, 0, 0, 2);
    mmem = R32(lk);
    for (uint32_t i = 0; i < mus_bytes / 2; i++) W16(mmem + 2 * i, (uint16_t)(int16_t)((i * 7919) % 30000 - 15000));
    com(mus, SETPOS, 2, 0);
    com(mus, PLAY, 4, 0, 0, 1);
    sound_mix(out, 1000, 44100);
    int exact = 1;
    for (int i = 0; i < 2000; i++)
        if (out[i] != (int16_t)((i * 7919u) % 30000 - 15000) / 32768.0f) { exact = 0; break; }
    CHECK(exact, "44100 passthrough bit-exact");
    pos(mus, &play, &write);
    CHECK(play == 4000, "after mixing 1000 frames at 44100: %u", play);
    CHECK(write == (1000 + 662 + 17) * 4, "attached write lead: %u", write);
    vclock += 5000;      /* attached: the clock must not move positions */
    pos(mus, &play, &write);
    CHECK(play == 4000, "clock ignored while attached: %u", play);
    /* looping wrap in the mixer */
    sound_mix(out, 52920, 44100);
    pos(mus, &play, &write);
    CHECK(play == 4000, "mixer wrap: %u", play);
    /* streaming while resampling: the refill behind the play cursor must not reach the output */
    {
        com(mus, STOP, 1);
        const uint32_t fr = 52920;
        uint64_t song = 0;
        #define SONG(s) ((int16_t)lrint(16000 * sin(2 * M_PI * 1000.5 * (double)(s) / 44100)))
        com(mus, LOCK, 8, 0, 0, lk, lk + 4, 0, 0, 2);
        for (uint32_t i = 0; i < fr; i++, song++) { W16(mmem + 4 * i, (uint16_t)SONG(song)); W16(mmem + 4 * i + 2, (uint16_t)SONG(song)); }
        com(mus, SETPOS, 2, 0);
        com(mus, PLAY, 4, 0, 0, 1);
        static float sbuf[2 * 96000];
        uint32_t last = 0;
        for (int ch = 0; ch < 96000 / 256; ch++) {
            sound_mix(sbuf + 2 * 256 * ch, 256, 48000);
            if (ch % 19 == 18) {
                pos(mus, &play, &write);
                uint32_t n = play >= last ? play - last : mus_bytes - last + play;
                com(mus, LOCK, 8, last, n, lk, lk + 4, lk + 8, lk + 12, 0);
                for (int part = 0; part < 2; part++) {
                    uint32_t p0 = R32(lk + 8 * part), pn = R32(lk + 4 + 8 * part);
                    for (uint32_t k = 0; k < pn / 4; k++, song++) { W16(p0 + 4 * k, (uint16_t)SONG(song)); W16(p0 + 4 * k + 2, (uint16_t)SONG(song)); }
                }
                com(mus, UNLOCK, 5, R32(lk), R32(lk + 4), R32(lk + 8), R32(lk + 12));
                last = play;
            }
        }
        sine_fit(sbuf + 2 * 2000, 96000 - 4000, 1000.5, 48000, &amp, &snr);
        printf("streamed 44100->48000 with refills: amplitude %.4f, SNR %.1f dB\n", amp, snr);
        CHECK(snr > 60, "streaming refill reaches the output (SNR %.1f dB)", snr);
        com(mus, STOP, 1);
    }
    pos(mus, &play, &write);
    uint32_t before = play;
    sound_output_detach();
    vclock += 1000;
    pos(mus, &play, &write);
    CHECK(play == before, "stopped music stays put after detach: %u", play);
    com(mus, PLAY, 4, 0, 0, 1);
    vclock += 100;
    pos(mus, &play, &write);
    CHECK(play == (before / 4 + 4410) % 52920 * 4, "clock drives it again after detach: %u", play);
    quiet = 0;

    /* --- release, duplicate lifetime, dead objects */
    CHECK(com(sfx, RELEASE, 1) == 0, "release sfx");
    CHECK(com(dup, LOCK, 8, 0, 10, lk, lk + 4, 0, 0, 0) == 0 && R32(lk) == mem, "dup memory survives original");
    CHECK(com(sfx, PLAY, 4, 0, 0, 0) == 0x88780032u, "dead object fails cleanly");
    CHECK(com(sfx, LOCK, 8, 0, 10, lk, lk + 4, 0, 0, 0) == 0x88780032u, "dead Lock pops 8 args");
    CHECK(com(ds, ADDREF, 1) == 2 && com(ds, RELEASE, 1) == 1, "ds refcount");
    CHECK(com(ds, RELEASE, 1) == 0, "ds release");
    CHECK(com(dup, STOP, 1) == 0, "buffers released with the DS object");
    CHECK(com(dup, GETSTATUS, 2, v32) == 0x88780032u, "dup dead");

    /* --- the fallback path: DirectSoundCreate by ordinal + EXCLUSIVE */
    uint32_t args[3] = {0, pp, 0};
    CHECK(callfn(w_DSOUND_1, 3, args) == 0, "DirectSoundCreate");
    uint32_t ds2 = R32(pp);
    CHECK(com(ds2, DSINIT, 2, 0) == 0x88780082u, "already initialized");
    CHECK(com(ds2, SETCOOP, 3, 0x1234, 3) == 0, "exclusive");
    uint32_t caps = galloc(96);
    W32(caps, 96);
    CHECK(com(ds2, DSGETCAPS, 2, caps) == 0 && R32(caps + 12) == 200000, "caps");
    CHECK(com(ds2, GETSPK, 2, v32) == 0 && R32(v32) == 0x00140004u, "speaker");
    CHECK(com(ds2, QI, 3, iid, pp) == 0 && R32(pp) == ds2, "QI self");
    CHECK(com(ds2, QI, 3, clsid, pp) == 0x80004002u, "QI other");

    if (wavmode) {
        /* real clock, WAV writer pulling: a 1 kHz tone for one second */
        CHECK(com(ds2, CREATESB, 4, mkdesc(0xe0, 11025, mkwfx(1, 11025, 8)), pp, 0) == 0, "wav sfx");
        uint32_t s = R32(pp);
        com(s, LOCK, 8, 0, 11025, lk, lk + 4, 0, 0, 0);
        for (uint32_t i = 0; i < 11025; i++)
            W8(R32(lk) + i, (uint8_t)lrint(128 + 63.5 * sin(2 * M_PI * 1000.0 * i / 11025)));
        com(s, PLAY, 4, 0, 0, 1);
        usleep(1000 * 1000);
        pos(s, &play, &write);
        printf("after 1 s of WAV writer: play %u write %u\n", play, write);
        com(s, STOP, 1);
        usleep(200 * 1000);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    exit(fails ? 1 : 0);
}
