#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "sound.h"
unsigned log_mask = 0;
void logmsg(unsigned cat, const char *fmt, ...) { (void)cat; (void)fmt; }
void fatal(const char *fmt, ...) { (void)fmt; exit(2); }
double now_ms(void) { return 0; }
CPU *current_cpu(void) { return NULL; }
static void (*fns[4096])(CPU *); static int nf;
uint32_t thunk_addr(void (*fn)(CPU *c), const char *name) { (void)name; for (int i = 0; i < nf; i++) if (fns[i] == fn) return THUNK_BASE + 16u * i; fns[nf] = fn; return THUNK_BASE + 16u * nf++; }
extern void w_DSOUND_1(CPU *c);
static CPU cpu; static uint32_t stack;
static uint32_t com(uint32_t obj, int idx, int nargs, ...) {
    uint32_t a[9]; a[0] = obj; va_list ap; va_start(ap, nargs); for (int i = 1; i < nargs; i++) a[i] = va_arg(ap, uint32_t); va_end(ap);
    uint32_t esp = stack + 0x8000; for (int i = nargs - 1; i >= 0; i--) { esp -= 4; W32(esp, a[i]); } esp -= 4; W32(esp, 0); cpu.esp = esp;
    fns[(R32(R32(obj) + 4 * idx) - THUNK_BASE) >> 4](&cpu); return cpu.eax; }
int main(int argc, char **argv) {
    mem_init(); stack = galloc(0x10000); dsound_init();
    uint32_t v = galloc(64), a3[3] = {0, v, 0};
    uint32_t esp = stack + 0x8000 - 16; W32(esp, 0); W32(esp + 4, 0); W32(esp + 8, v); W32(esp + 12, 0); cpu.esp = esp; w_DSOUND_1(&cpu); (void)a3;
    uint32_t ds = R32(v);
    uint32_t w = galloc(18); W16(w, 1); W16(w + 2, 1); W32(w + 4, 11025); W32(w + 8, 11025); W16(w + 12, 1); W16(w + 14, 8);
    uint32_t d = galloc(36); W32(d, 36); W32(d + 4, 0x100e2); W32(d + 8, 22050); W32(d + 16, w);
    int voices = argc > 1 ? atoi(argv[1]) : 64;
    sound_output_attach(48000, 512);
    for (int i = 0; i < voices; i++) {
        com(ds, 3, 4, d, v, 0); uint32_t b = R32(v);
        com(b, 11, 8, 0, 22050, v + 8, v + 12, 0, 0, 0);
        for (int k = 0; k < 22050; k++) W8(R32(v + 8) + k, rand() & 0xff);
        com(b, 17, 2, 8000 + rand() % 30000); com(b, 15, 2, (uint32_t)-500); com(b, 12, 4, 0, 0, 1);
    }
    static float out[2 * 512];
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int chunks = 48000 * 10 / 512;
    for (int i = 0; i < chunks; i++) sound_mix(out, 512, 48000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d voices, 10 s of 48 kHz audio mixed in %.3f s (%.1f%% of one core)\n", voices, s, s * 10);
    return 0;
}
