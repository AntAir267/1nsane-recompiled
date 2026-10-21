/* The pieces of Microsoft's C/C++ runtime that NVIDIA's NGX link library (built with MSVC)
   expects, for linking it into a MinGW program with lld. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/* operator new / delete */
void *msvc_new(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
void msvc_delete(void *p) { free(p); }

/* std::_Xlength_error / std::_Xbad_alloc, imported through pointers (__declspec(dllimport)) */
static void msvc_xlength(const char *what) { fprintf(stderr, "NGX: length error: %s\n", what); abort(); }
static void msvc_xbad_alloc(void) { fprintf(stderr, "NGX: out of memory\n"); abort(); }
void (*msvc_imp_xlength)(const char *) = msvc_xlength;
void (*msvc_imp_xbad_alloc)(void) = msvc_xbad_alloc;

/* the MSVC names (with '?', which C cannot spell) as aliases */
__asm__(".globl \"??2@YAPEAX_K@Z\"\n.set \"??2@YAPEAX_K@Z\", msvc_new\n"
        ".globl \"??3@YAXPEAX@Z\"\n.set \"??3@YAXPEAX@Z\", msvc_delete\n"
        ".globl \"__imp_?_Xlength_error@std@@YAXPEBD@Z\"\n.set \"__imp_?_Xlength_error@std@@YAXPEBD@Z\", msvc_imp_xlength\n"
        ".globl \"__imp_?_Xbad_alloc@std@@YAXXZ\"\n.set \"__imp_?_Xbad_alloc@std@@YAXXZ\", msvc_imp_xbad_alloc\n");

/* Control Flow Guard (NGX is built with /guard:cf): every indirect call first calls
   __guard_check_icall_fptr with the target in rcx; this program does not use CFG, so the check
   passes everything (and must keep every register, as MSVC's own no-op does) */
__asm__(".text\n"
        ".globl msvc_guard_check_nop\n"
        "msvc_guard_check_nop: ret\n"
        ".globl msvc_guard_dispatch_nop\n"
        "msvc_guard_dispatch_nop: jmp *%rax\n");
void msvc_guard_check_nop(void);
void msvc_guard_dispatch_nop(void);
/* MinGW's runtime has these pointers (read-only, for the load configuration), not pointing at code
   that suits MSVC's calls: set at start-up */
extern void *__guard_check_icall_fptr, *__guard_dispatch_icall_fptr;
#ifdef __clang__
/* what MinGW's CFG support points __guard_dispatch_icall_fptr at by default, which Zig's build of
   MinGW's runtime leaves out (MinGW's own: a jump through rax) */
__asm__(".text\n"
        ".globl __guard_dispatch_icall_dummy\n"
        "__guard_dispatch_icall_dummy: jmp *%rax\n");
#endif

__attribute__((constructor)) static void msvc_guard_setup(void)
{
    void **p[2] = {&__guard_check_icall_fptr, &__guard_dispatch_icall_fptr};
    void *v[2] = {(void *)msvc_guard_check_nop, (void *)msvc_guard_dispatch_nop};
    for (int i = 0; i < 2; i++) {
        DWORD old;
        if (VirtualProtect(p[i], sizeof(void *), PAGE_READWRITE, &old)) {
            *p[i] = v[i];
            VirtualProtect(p[i], sizeof(void *), old, &old);
        }
    }
}

/* floating point in use */
int _fltused = 0x9875;

/* /GS stack cookies */
uintptr_t __security_cookie = 0x2B992DDFA232ull;
void __security_check_cookie(uintptr_t c)
{
    if (c != __security_cookie) {
        fprintf(stderr, "NGX: stack cookie overwritten\n");
        abort();
    }
}

/* the unwind handlers of /GS frames: only run while an exception passes through NGX code */
EXCEPTION_DISPOSITION __GSHandlerCheck(void *rec, void *frame, void *ctx, void *disp)
{
    (void)rec; (void)frame; (void)ctx; (void)disp;
    return ExceptionContinueSearch;
}
EXCEPTION_DISPOSITION __GSHandlerCheck_EH(void *rec, void *frame, void *ctx, void *disp)
{
    (void)rec; (void)frame; (void)ctx; (void)disp;
    return ExceptionContinueSearch;
}

/* Thread-safe function-local statics. MSVC's inline check is `if (guard > _Init_thread_epoch)`,
   with the epoch a thread-local variable read through the TLS directory: it stays at INT_MIN
   here, so every check after the first initialisation comes to _Init_thread_header, which sees
   the guard set and returns (MSVC's own runtime does the same until a thread's epoch catches
   up). */
__asm__(".section .tls$ZZZ,\"dw\"\n"
        ".globl _Init_thread_epoch\n"
        ".p2align 2\n"
        "_Init_thread_epoch: .long 0x80000000\n"
        ".text\n");
static SRWLOCK init_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE init_cv = CONDITION_VARIABLE_INIT;
static int init_global_epoch = (int)0x80000000;

void _Init_thread_header(int *once)
{
    AcquireSRWLockExclusive(&init_lock);
    while (*once == -1)                  /* another thread is initialising it */
        SleepConditionVariableSRW(&init_cv, &init_lock, INFINITE, 0);
    if (*once == 0)
        *once = -1;                      /* ours to initialise: the caller sees -1 */
    ReleaseSRWLockExclusive(&init_lock);
}

void _Init_thread_footer(int *once)
{
    AcquireSRWLockExclusive(&init_lock);
    *once = ++init_global_epoch;
    ReleaseSRWLockExclusive(&init_lock);
    WakeAllConditionVariable(&init_cv);
}

void _Init_thread_abort(int *once)
{
    AcquireSRWLockExclusive(&init_lock);
    *once = 0;
    ReleaseSRWLockExclusive(&init_lock);
    WakeAllConditionVariable(&init_cv);
}
