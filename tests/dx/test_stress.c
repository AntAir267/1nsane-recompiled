/* Concurrency stress for runtime/dsound.c: game threads hammering buffers while a puller mixes. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "runtime.h"
#include "sound.h"

unsigned log_mask = LOG_ALWAYS;
void logmsg(unsigned cat, const char *fmt, ...)
{
    static int n;
    if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) > 30) return;
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap); fputc('\n', stdout);
}
void fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap); fputc('\n', stdout); _exit(2);
}
static struct timespec t0;
double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) * 1000.0 + (t.tv_nsec - t0.tv_nsec) / 1e6;
}
CPU *current_cpu(void) { return NULL; }
static void (*fns[4096])(CPU *);
static int nf;
static pthread_mutex_t tl = PTHREAD_MUTEX_INITIALIZER;
uint32_t thunk_addr(void (*fn)(CPU *c), const char *name)
{
    pthread_mutex_lock(&tl);
    for (int i = 0; i < nf; i++) if (fns[i] == fn) { pthread_mutex_unlock(&tl); return THUNK_BASE + 16u * i; }
    fns[nf] = fn; uint32_t a = THUNK_BASE + 16u * nf++;
    pthread_mutex_unlock(&tl);
    return a;
}
extern void w_DSOUND_1(CPU *c);

typedef struct { CPU cpu; uint32_t stack, v; } Th;
static uint32_t callfn(Th *t, void (*fn)(CPU *), int nargs, const uint32_t *a)
{
    uint32_t esp = t->stack + 0x4000;
    for (int i = nargs - 1; i >= 0; i--) { esp -= 4; W32(esp, a[i]); }
    esp -= 4; W32(esp, 0x12345678);
    t->cpu.esp = esp;
    fn(&t->cpu);
    if (t->cpu.esp != esp + 4 + 4 * (uint32_t)nargs) { printf("stack imbalance\n"); _exit(3); }
    return t->cpu.eax;
}
static uint32_t com(Th *t, uint32_t obj, int idx, int nargs, ...)
{
    uint32_t a[9]; a[0] = obj;
    va_list ap; va_start(ap, nargs);
    for (int i = 1; i < nargs; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t th = R32(R32(obj) + 4 * idx);
    pthread_mutex_lock(&tl);
    void (*fn)(CPU *) = fns[(th - THUNK_BASE) >> 4];
    pthread_mutex_unlock(&tl);
    return callfn(t, fn, nargs, a);
}
enum { RELEASE = 2, CREATESB = 3, DUP = 5, SETCOOP = 6 };
enum { GETPOS = 4, GETSTATUS = 9, LOCK = 11, PLAY, SETPOS, SETVOL = 15, SETPAN, SETFREQ, STOP, UNLOCK };

static uint32_t ds, wfx_m, wfx_s, desc_m, desc_s;
static int stop_all;
static Th *th_new(void) { Th *t = calloc(1, sizeof *t); t->stack = galloc(0x4000); t->v = galloc(64); return t; }

static void *music(void *arg)
{
    Th *t = th_new();
    com(t, ds, CREATESB, 4, desc_m, t->v, 0);
    uint32_t b = R32(t->v), last = 0, bytes = 211680, n = 0;
    com(t, b, PLAY, 4, 0, 0, 1);
    while (!__atomic_load_n(&stop_all, __ATOMIC_RELAXED)) {
        com(t, b, GETPOS, 3, t->v, t->v + 4);
        uint32_t play = R32(t->v);
        uint32_t k = play >= last ? play - last : bytes - last + play;
        if (k) {
            if (com(t, b, LOCK, 8, last, k, t->v + 8, t->v + 12, t->v + 16, t->v + 20, 0)) { printf("lock failed\n"); _exit(4); }
            memset(gptr(R32(t->v + 8)), (int)(n++ & 0x7f), R32(t->v + 12));
            if (R32(t->v + 16)) memset(gptr(R32(t->v + 16)), 0, R32(t->v + 20));
            com(t, b, UNLOCK, 5, R32(t->v + 8), R32(t->v + 12), R32(t->v + 16), R32(t->v + 20));
        }
        last = play;
        usleep(1000);
    }
    com(t, b, RELEASE, 1);
    return arg;
}

static void *sfx(void *arg)
{
    Th *t = th_new();
    unsigned seed = (unsigned)(uintptr_t)arg;
    while (!__atomic_load_n(&stop_all, __ATOMIC_RELAXED)) {
        com(t, ds, CREATESB, 4, desc_s, t->v, 0);
        uint32_t b[8];
        b[0] = R32(t->v);
        com(t, b[0], LOCK, 8, 0, 0, t->v + 8, t->v + 12, 0, 0, 2);
        for (uint32_t i = 0; i < R32(t->v + 12); i++) W8(R32(t->v + 8) + i, (uint8_t)rand_r(&seed));
        for (int i = 1; i < 8; i++) { com(t, ds, DUP, 3, b[0], t->v); b[i] = R32(t->v); }
        for (int r = 0; r < 50; r++) {
            uint32_t x = b[rand_r(&seed) % 8];
            switch (rand_r(&seed) % 6) {
            case 0: com(t, x, PLAY, 4, 0, 0, rand_r(&seed) & 1); break;
            case 1: com(t, x, STOP, 1); break;
            case 2: com(t, x, SETFREQ, 2, 100 + rand_r(&seed) % 199900); break;
            case 3: com(t, x, SETVOL, 2, (uint32_t)-(int)(rand_r(&seed) % 10001)); break;
            case 4: com(t, x, SETPAN, 2, (uint32_t)(int)(rand_r(&seed) % 20001 - 10000)); break;
            case 5: com(t, x, GETSTATUS, 2, t->v); com(t, x, SETPOS, 2, rand_r(&seed) % 2000); break;
            }
            usleep(100);
        }
        for (int i = 7; i >= 0; i--) com(t, b[i], RELEASE, 1);
    }
    return arg;
}

int main(void)
{
    clock_gettime(CLOCK_MONOTONIC, &t0);
    mem_init();
    dsound_init();
    Th *t = th_new();
    uint32_t a[3] = {0, t->v, 0};
    callfn(t, w_DSOUND_1, 3, a);
    ds = R32(t->v);
    com(t, ds, SETCOOP, 3, 1, 2);
    wfx_m = galloc(18); W16(wfx_m, 1); W16(wfx_m + 2, 2); W32(wfx_m + 4, 44100); W32(wfx_m + 8, 176400); W16(wfx_m + 12, 4); W16(wfx_m + 14, 16);
    wfx_s = galloc(18); W16(wfx_s, 1); W16(wfx_s + 2, 1); W32(wfx_s + 4, 11025); W32(wfx_s + 8, 11025); W16(wfx_s + 12, 1); W16(wfx_s + 14, 8);
    desc_m = galloc(36); W32(desc_m, 36); W32(desc_m + 4, 0xe0); W32(desc_m + 8, 211680); W32(desc_m + 16, wfx_m);
    desc_s = galloc(36); W32(desc_s, 36); W32(desc_s + 4, 0x100e2); W32(desc_s + 8, 2000); W32(desc_s + 16, wfx_s);
    pthread_t p[4];
    pthread_create(&p[0], NULL, music, NULL);
    pthread_create(&p[1], NULL, sfx, (void *)1);
    pthread_create(&p[2], NULL, sfx, (void *)2);
    pthread_create(&p[3], NULL, sfx, (void *)3);
    /* half the time with an external puller replacing the WAV writer, half without */
    usleep(700 * 1000);
    sound_output_attach(48000, 512);
    static float out[2 * 512];
    for (int i = 0; i < 100; i++) { sound_mix(out, 512, 48000); usleep(10000); }
    sound_output_detach();
    usleep(700 * 1000);
    __atomic_store_n(&stop_all, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < 4; i++) pthread_join(p[i], NULL);
    printf("stress done\n");
    return 0;
}
