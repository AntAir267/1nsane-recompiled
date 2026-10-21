/* 1NSANE: Recompiled's launcher and first-run setup.

   The download holds no part of the game: this launcher, the translator (tools/recomp.py, frozen),
   a C compiler (Zig), the runtime built ahead of time (lib/libruntime.a, lib/libSDL3.a and the
   headers the generated code includes) and the files describing the GOG release (builds/). The
   first time it runs it finds the player's copy of the game (an installed GOG folder, or the GOG
   installer, unpacked with innoextract without running it), checks every file against the GOG
   release's hashes, copies them to the user folder (game/), translates Game.exe to C (build/gen),
   compiles that and links it with the runtime (bin/1nsane), and offers the fan pack. After that it
   starts the game at once, until a new download (another VERSION) builds it again.

   Bundle (the folder holding VERSION; by default <launcher's folder>/../lib/1nsane-recompiled, on
   Windows the launcher's own folder, or INSANE_BUNDLE): VERSION, zig/, translate, innoextract,
   builds/, include/, lib/, fanpack.txt, and on Windows bin/ (copied beside the game it builds).
   User folder: the game's own (INSANE_USER, else XDG_DATA_HOME or ~/.local/share, or %APPDATA%, then
   /1nsane-native).

   Without a window (--setup-from <folder or installer> [--fan-pack] [--no-launch]) it does the same
   in the terminal: for tests and for those who prefer it. */
#include <SDL3/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#include "font_1nsane_otf.h"

#ifdef _WIN32
#define EXE ".exe"
#define TARGET "x86_64-windows-gnu"
#else
#define EXE ""
#define TARGET "x86_64-linux-gnu.2.31"
#endif

static char bundle[1024], user[1024];
static char version[128];

/* ------------------------------------------------------------------ the shared state */
static SDL_Mutex *lock;
static char status[256], detail[1024];
static double progress = -1;            /* 0..1, or < 0 for none */
static volatile int task_done = 1, task_ok = 1;
static int headless;

static void say(const char *st, const char *de)
{
    SDL_LockMutex(lock);
    if (st)
        SDL_strlcpy(status, st, sizeof status);
    if (de)
        SDL_strlcpy(detail, de, sizeof detail);
    SDL_UnlockMutex(lock);
    if (headless)
        SDL_Log("%s%s%s", st ? st : "", st && de && *de ? ": " : "", de ? de : "");
}

static void set_progress(double p)
{
    SDL_LockMutex(lock);
    progress = p;
    SDL_UnlockMutex(lock);
}

/* ------------------------------------------------------------------ files */
static void path(char *out, size_t n, const char *a, const char *b)
{
    SDL_snprintf(out, n, "%s/%s", a, b);
}

static int exists(const char *p)
{
    SDL_PathInfo i;
    return SDL_GetPathInfo(p, &i);
}

static char *read_text(const char *p)
{
    size_t n;
    char *s = SDL_LoadFile(p, &n);
    return s;
}

static int write_text(const char *p, const char *s)
{
    return SDL_SaveFile(p, s, SDL_strlen(s));
}

static void make_dirs(const char *p)
{
    char t[1024];
    SDL_strlcpy(t, p, sizeof t);
    for (char *c = t + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char k = *c;
            *c = 0;
            SDL_CreateDirectory(t);
            *c = k;
        }
    SDL_CreateDirectory(t);
}

/* a path under root whose components match the manifest's case-blind (the game's files keep whatever
   case the installer gave them) */
typedef struct { const char *want; char found[256]; } FindName;

static SDL_EnumerationResult find_cb(void *ud, const char *dir, const char *name)
{
    FindName *f = ud;
    if (!SDL_strcasecmp(name, f->want)) {
        SDL_strlcpy(f->found, name, sizeof f->found);
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

static int resolve_ci(const char *root, const char *rel, char *out, size_t n)
{
    char cur[1024], part[256];
    SDL_strlcpy(cur, root, sizeof cur);
    const char *p = rel;
    while (*p) {
        size_t k = strcspn(p, "/\\");
        if (k >= sizeof part)
            return 0;
        SDL_memcpy(part, p, k);
        part[k] = 0;
        FindName f = {part, ""};
        SDL_EnumerateDirectory(cur, find_cb, &f);
        if (!f.found[0])
            return 0;
        SDL_strlcat(cur, "/", sizeof cur);
        SDL_strlcat(cur, f.found, sizeof cur);
        p += k;
        while (*p == '/' || *p == '\\')
            p++;
    }
    SDL_strlcpy(out, cur, n);
    return 1;
}

static int file_sha256(const char *p, char hex[65], Sint64 *size)
{
    SDL_IOStream *io = SDL_IOFromFile(p, "rb");
    if (!io)
        return 0;
    Sha256 s;
    sha256_init(&s);
    static uint8_t buf[1 << 16];
    size_t r;
    Sint64 total = 0;
    while ((r = SDL_ReadIO(io, buf, sizeof buf)) > 0) {
        sha256_update(&s, buf, r);
        total += (Sint64)r;
    }
    SDL_CloseIO(io);
    uint8_t d[32];
    sha256_final(&s, d);
    sha256_hex(d, hex);
    if (size)
        *size = total;
    return 1;
}

/* ------------------------------------------------------------------ running the tools */
/* Runs argv, waits; its output (stdout and stderr) goes to the setup log, and its last lines are
   kept for an error message. Returns its exit code (-1: not started). */
static char last_output[2048];

static int run(const char *const *argv)
{
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (!p) {
        SDL_snprintf(last_output, sizeof last_output, "cannot start %s: %s", argv[0], SDL_GetError());
        return -1;
    }
    size_t n = 0;
    int code = -1;
    char *out = SDL_ReadProcess(p, &n, &code);
    char log[1024];
    path(log, sizeof log, user, "setup.log");
    SDL_IOStream *lf = SDL_IOFromFile(log, "ab");
    if (lf) {
        SDL_IOprintf(lf, "$");
        for (int i = 0; argv[i]; i++)
            SDL_IOprintf(lf, " %s", argv[i]);
        SDL_IOprintf(lf, "\n%s\n[exit %d]\n", out ? out : "", code);
        SDL_CloseIO(lf);
    }
    last_output[0] = 0;
    if (out) {
        size_t k = n > sizeof last_output - 1 ? n - (sizeof last_output - 1) : 0;
        SDL_strlcpy(last_output, out + k, sizeof last_output);
        SDL_free(out);
    }
    SDL_DestroyProcess(p);
    return code;
}

/* ------------------------------------------------------------------ the steps */
typedef struct { char sha[65]; char rel[256]; } ManifestEntry;
static ManifestEntry *manifest;
static int nmanifest;

static int load_manifest(void)
{
    if (manifest)
        return 1;
    char p[1024];
    path(p, sizeof p, bundle, "builds/gog_1_0.manifest.sha256");
    char *s = read_text(p);
    if (!s)
        return 0;
    int cap = 128;
    manifest = SDL_calloc(cap, sizeof *manifest);
    char *save = NULL;
    for (char *line = SDL_strtok_r(s, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        if (SDL_strlen(line) < 67)
            continue;
        if (nmanifest == cap)
            manifest = SDL_realloc(manifest, (size_t)(cap *= 2) * sizeof *manifest);
        SDL_memcpy(manifest[nmanifest].sha, line, 64);
        manifest[nmanifest].sha[64] = 0;
        SDL_strlcpy(manifest[nmanifest].rel, line + 66, sizeof manifest[nmanifest].rel);
        nmanifest++;
    }
    SDL_free(s);
    return nmanifest > 0;
}

/* the folder holding Game.exe under root (itself or up to three levels down), into out */
typedef struct { char *out; size_t n; int depth; int found; } FindGame;

static SDL_EnumerationResult find_game_cb(void *ud, const char *dir, const char *name)
{
    FindGame *f = ud;
    char p[1024];
    SDL_snprintf(p, sizeof p, "%s%s", dir, name);
    if (!SDL_strcasecmp(name, "Game.exe")) {
        SDL_strlcpy(f->out, dir, f->n);
        size_t k = SDL_strlen(f->out);
        if (k && (f->out[k - 1] == '/' || f->out[k - 1] == '\\'))
            f->out[k - 1] = 0;
        f->found = 1;
        return SDL_ENUM_SUCCESS;
    }
    SDL_PathInfo i;
    if (f->depth < 3 && SDL_GetPathInfo(p, &i) && i.type == SDL_PATHTYPE_DIRECTORY) {
        f->depth++;
        SDL_EnumerateDirectory(p, find_game_cb, f);
        f->depth--;
        if (f->found)
            return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

static int find_game(const char *root, char *out, size_t n)
{
    FindGame f = {out, n, 0, 0};
    SDL_EnumerateDirectory(root, find_game_cb, &f);
    return f.found;
}

/* checks the GOG release's files under src, then copies them into the user folder's game/ */
static int check_and_copy(const char *src)
{
    if (!load_manifest()) {
        say("The download is incomplete", "its list of the game's files is missing");
        return 0;
    }
    char dst_root[1024];
    path(dst_root, sizeof dst_root, user, "game");
    int same = !SDL_strcmp(src, dst_root);
    for (int i = 0; i < nmanifest; i++) {
        char p[1024], hex[65];
        set_progress((double)i / nmanifest);
        say("Checking your copy of 1NSANE", manifest[i].rel);
        if (!resolve_ci(src, manifest[i].rel, p, sizeof p)) {
            char m[512];
            SDL_snprintf(m, sizeof m, "%s is missing: this needs the GOG release of 1NSANE", manifest[i].rel);
            say("This isn't the GOG copy of 1NSANE", m);
            return 0;
        }
        if (!file_sha256(p, hex, NULL) || SDL_strcmp(hex, manifest[i].sha)) {
            char m[512];
            SDL_snprintf(m, sizeof m, "%s is not the GOG release's (another version, or damaged)", manifest[i].rel);
            say("This isn't the GOG copy of 1NSANE", m);
            return 0;
        }
        if (!same) {
            char d[1024];
            path(d, sizeof d, dst_root, manifest[i].rel);
            char dir[1024];
            SDL_strlcpy(dir, d, sizeof dir);
            char *slash = SDL_strrchr(dir, '/');
            if (slash) {
                *slash = 0;
                make_dirs(dir);
            }
            if (!SDL_CopyFile(p, d)) {
                say("Could not copy the game's files", SDL_GetError());
                return 0;
            }
        }
    }
    char mark[1024];
    path(mark, sizeof mark, dst_root, ".gog-checked");
    write_text(mark, version);
    set_progress(1);
    return 1;
}

static SDL_EnumerationResult remove_entry(void *ud, const char *dir, const char *name);

static void remove_tree(const char *p)
{
    SDL_EnumerateDirectory(p, remove_entry, NULL);
    SDL_RemovePath(p);
}

static SDL_EnumerationResult remove_entry(void *ud, const char *dir, const char *name)
{
    (void)ud;
    char p[1024];
    SDL_PathInfo i;
    SDL_snprintf(p, sizeof p, "%s%s", dir, name);   /* (dir ends in a separator) */
    if (SDL_GetPathInfo(p, &i) && i.type == SDL_PATHTYPE_DIRECTORY)
        remove_tree(p);
    else
        SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}

static int unpack_installer(const char *exe, char *out, size_t n)
{
    char tool[1024], tmp[1024];
    path(tool, sizeof tool, bundle, "innoextract" EXE);
    path(tmp, sizeof tmp, user, "build/unpack");
    say("Unpacking the GOG installer", "nothing in it is run, only read");
    set_progress(-1);
    remove_tree(tmp);                   /* (what a stopped run left) */
    make_dirs(tmp);
    const char *argv[] = {tool, "-s", "-I", "app", "-d", tmp, exe, NULL};   /* (the game is in app/) */
    if (run(argv) != 0) {
        say("Could not unpack the installer", last_output);
        return 0;
    }
    if (!find_game(tmp, out, n)) {
        say("This installer doesn't hold 1NSANE", "no Game.exe in it");
        return 0;
    }
    return 1;
}

#ifdef _WIN32
/* the files of one folder into another (not its subfolders) */
typedef struct { const char *from, *to; int ok; } CopyDir;

static SDL_EnumerationResult copy_cb(void *ud, const char *dir, const char *name)
{
    CopyDir *c = ud;
    char s[1024], d[1024];
    SDL_PathInfo i;
    (void)dir;
    path(s, sizeof s, c->from, name);
    path(d, sizeof d, c->to, name);
    if (SDL_GetPathInfo(s, &i) && i.type == SDL_PATHTYPE_FILE && !SDL_CopyFile(s, d)) {
        c->ok = 0;
        return SDL_ENUM_FAILURE;
    }
    return SDL_ENUM_CONTINUE;
}
#endif

/* Game.exe to C, then compiled (several at once) and linked with the runtime */
static int build_game(void)
{
    char zig[1024], gen[1024], obj[1024], bin[1024], tool[1024], cfg[1024], exe[1024], inc[1024];
    path(zig, sizeof zig, bundle, "zig/zig" EXE);
    path(gen, sizeof gen, user, "build/gen");
    path(obj, sizeof obj, user, "build/obj");
    path(bin, sizeof bin, user, "bin");
    path(tool, sizeof tool, bundle, "translate" EXE);
    path(cfg, sizeof cfg, bundle, "builds/gog_1_0.py");
    path(exe, sizeof exe, user, "game/Game.exe");
    path(inc, sizeof inc, bundle, "include");
    make_dirs(gen);
    make_dirs(obj);
    make_dirs(bin);
    char cache[1024];
    path(cache, sizeof cache, user, "build/zig-cache");
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "ZIG_GLOBAL_CACHE_DIR", cache, true);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "ZIG_LOCAL_CACHE_DIR", cache, true);

    say("Translating the game", "Game.exe to C, on this computer (a few seconds)");
    set_progress(-1);
    const char *targv[] = {tool, exe, cfg, gen, NULL};
    if (run(targv) != 0) {
        say("Translating failed", last_output);
        return 0;
    }
    /* the runtime was built against this very translation: its declarations must match */
    char want_p[1024], have_p[1024], have[65];
    path(want_p, sizeof want_p, bundle, "FUNCS_SHA256");
    path(have_p, sizeof have_p, gen, "funcs.h");
    char *want = read_text(want_p);
    if (want && (!file_sha256(have_p, have, NULL) || SDL_strncmp(want, have, 64))) {
        SDL_free(want);
        say("The translation doesn't match this download", "please report it (setup.log in the user folder)");
        return 0;
    }
    SDL_free(want);

    /* the generated files */
    const char *names[64];
    int nnames = 0;
    static char namebuf[64][32];
    for (int i = 0; i < 100 && nnames < 60; i++) {
        char f[32], p[1024];
        SDL_snprintf(f, sizeof f, "code_%03d.c", i);
        path(p, sizeof p, gen, f);
        if (!exists(p))
            break;
        SDL_strlcpy(namebuf[nnames], f, sizeof namebuf[0]);
        names[nnames] = namebuf[nnames];
        nnames++;
    }
    SDL_strlcpy(namebuf[nnames], "table.c", 32), names[nnames] = namebuf[nnames], nnames++;
    SDL_strlcpy(namebuf[nnames], "stubs.c", 32), names[nnames] = namebuf[nnames], nnames++;

    int jobs = SDL_GetNumLogicalCPUCores();
    jobs = jobs > 2 ? jobs - 1 : 1;
    if (SDL_getenv("INSANE_JOBS"))      /* (fewer, to keep the computer free for other things) */
        jobs = SDL_atoi(SDL_getenv("INSANE_JOBS"));
    if (jobs < 1)
        jobs = 1;
    if (jobs > 16)
        jobs = 16;
    SDL_Process *procs[16] = {0};
    int which[16];
    int next = 0, done = 0, failed = 0;
    char objs[64][1024];
    say("Building the game", "compiling the translated code (a few minutes, once)");
    while (done < nnames && !failed) {
        for (int j = 0; j < jobs && next < nnames; j++) {
            if (procs[j])
                continue;
            char src[1024];
            path(src, sizeof src, gen, names[next]);
            SDL_snprintf(objs[next], sizeof objs[0], "%s/%.*s.o", obj, (int)(SDL_strlen(names[next]) - 2), names[next]);
            const char *argv[] = {zig, "cc", "-target", TARGET, "-std=gnu11", "-O2", "-w", "-DCPU_FAST_MEM",
                                  "-fno-strict-aliasing", "-fno-omit-frame-pointer", "-fno-slp-vectorize",
#ifndef _WIN32
                                  "-fPIE",
#endif
                                  "-I", inc, "-I", gen, "-c", src, "-o", objs[next], NULL};
            SDL_PropertiesID pr = SDL_CreateProperties();
            SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
            SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
            SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
            procs[j] = SDL_CreateProcessWithProperties(pr);
            SDL_DestroyProperties(pr);
            if (!procs[j]) {
                SDL_snprintf(last_output, sizeof last_output, "cannot start the compiler: %s", SDL_GetError());
                failed = 1;
                break;
            }
            which[j] = next++;
        }
        for (int j = 0; j < jobs; j++) {
            int code;
            if (procs[j] && SDL_WaitProcess(procs[j], false, &code)) {
                size_t n;
                char *out = SDL_ReadProcess(procs[j], &n, NULL);
                if (code != 0) {
                    SDL_snprintf(last_output, sizeof last_output, "%s: %s", names[which[j]], out ? out : "");
                    failed = 1;
                }
                SDL_free(out);
                SDL_DestroyProcess(procs[j]);
                procs[j] = NULL;
                done++;
                set_progress(0.9 * done / nnames);
                char m[256];
                SDL_snprintf(m, sizeof m, "compiled %d of %d parts", done, nnames);
                say(NULL, m);
            }
        }
        SDL_Delay(50);
    }
    for (int j = 0; j < jobs; j++)
        if (procs[j]) {
            SDL_WaitProcess(procs[j], true, NULL);
            SDL_DestroyProcess(procs[j]);
        }
    if (failed) {
        say("Compiling failed", last_output);
        return 0;
    }

    say("Building the game", "linking it with the runtime");
    char out[1024], rt[1024], sdl[1024], stubs[1024];
    path(out, sizeof out, bin, "1nsane" EXE);
    path(rt, sizeof rt, bundle, "lib/libruntime.a");
    path(sdl, sizeof sdl, bundle, "lib/libSDL3.a");
    path(stubs, sizeof stubs, bundle, "lib/stubs");
    const char *largv[128];
    int k = 0;
    largv[k++] = zig, largv[k++] = "c++", largv[k++] = "-target", largv[k++] = TARGET;
#ifndef _WIN32
    largv[k++] = "-fPIE", largv[k++] = "-pie", largv[k++] = "-rdynamic";
#endif
    largv[k++] = "-o", largv[k++] = out;
    for (int i = 0; i < nnames; i++)
        largv[k++] = objs[i];
    largv[k++] = "-Wl,--whole-archive", largv[k++] = rt, largv[k++] = "-Wl,--no-whole-archive", largv[k++] = sdl;
#ifdef _WIN32
    /* lib/ holds the static libraries and Vulkan's import library; with NVIDIA's DLSS link library
       there too, it goes in, with empty stand-ins for the MSVC runtime libraries its objects name
       (the runtime's msvc_shim.c has what they use) */
    char libdir[1024], ngx[1024], ftl[1024];
    path(libdir, sizeof libdir, bundle, "lib");
    path(ngx, sizeof ngx, bundle, "lib/nvsdk_ngx_d.lib");
    path(ftl, sizeof ftl, bundle, "lib/libfreetype.a");
    (void)stubs;
    largv[k++] = "-L", largv[k++] = libdir;
    if (exists(ngx))
        largv[k++] = ngx;
    if (exists(ftl))
        largv[k++] = "-lfreetype";
    largv[k++] = "-lvulkan-1", largv[k++] = "-lz", largv[k++] = "-liconv", largv[k++] = "-luser32",
    largv[k++] = "-lgdi32", largv[k++] = "-lwinmm", largv[k++] = "-limm32", largv[k++] = "-lole32",
    largv[k++] = "-loleaut32", largv[k++] = "-lversion", largv[k++] = "-luuid", largv[k++] = "-ladvapi32",
    largv[k++] = "-lsetupapi", largv[k++] = "-lshell32", largv[k++] = "-ldinput8", largv[k++] = "-lpthread",
    largv[k++] = "-static", largv[k++] = "-Wl,--stack,8388608", largv[k++] = "-mwindows";
#else
    largv[k++] = "-L", largv[k++] = stubs;
    largv[k++] = "-lvulkan", largv[k++] = "-lfreetype", largv[k++] = "-lz", largv[k++] = "-lm", largv[k++] = "-ldl";
    largv[k++] = "-pthread", largv[k++] = "-Wl,--allow-shlib-undefined";
#endif
    largv[k] = NULL;
    if (run(largv) != 0) {
        say("Linking failed", last_output);
        return 0;
    }
#ifdef _WIN32
    /* the programs and libraries the game finds beside itself (DLSS's, 7-Zip, libarchive, ...) */
    char from[1024];
    path(from, sizeof from, bundle, "bin");
    CopyDir cd = {from, bin, 1};
    SDL_EnumerateDirectory(from, copy_cb, &cd);
    if (!cd.ok) {
        say("Could not copy the game's helper files", SDL_GetError());
        return 0;
    }
#endif
    char stamp[1024];
    path(stamp, sizeof stamp, bin, "VERSION");
    write_text(stamp, version);
    set_progress(1);
    say("Built", "1NSANE: Recompiled is ready");
    return 1;
}

/* ------------------------------------------------------------------ the fan pack */
typedef struct { char sha[65]; Sint64 size; char name[128]; } PackFile;

static int fan_pack(void)
{
    char p[1024];
    path(p, sizeof p, bundle, "fanpack.txt");
    char *s = read_text(p);
    if (!s) {
        say("The fan pack's list is missing from this download", "");
        return 0;
    }
    PackFile files[16];
    int nf = 0;
    char url_base[512] = "";
    char *save = NULL;
    for (char *line = SDL_strtok_r(s, "\r\n", &save); line && nf < 16; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        if (!SDL_strncmp(line, "url ", 4)) {
            SDL_strlcpy(url_base, line + 4, sizeof url_base);
            continue;
        }
        long long size;
        if (SDL_sscanf(line, "%64s %lld %127s", files[nf].sha, &size, files[nf].name) == 3) {
            files[nf].size = size;
            nf++;
        }
    }
    SDL_free(s);
    Sint64 total = 0, got = 0;
    for (int i = 0; i < nf; i++)
        total += files[i].size;
    char mods[1024];
    path(mods, sizeof mods, user, "mods");
    make_dirs(mods);
    for (int i = 0; i < nf; i++) {
        char dst[1024], url[1024], m[256];
        path(dst, sizeof dst, mods, files[i].name);
        SDL_snprintf(url, sizeof url, "%s/%s", url_base, files[i].name);
        SDL_snprintf(m, sizeof m, "%s (%lld MB)", files[i].name, (long long)(files[i].size >> 20));
        say("Downloading the fan cars and maps", m);
        const char *argv[] = {"curl", "-fsSL", "--retry", "3", "-o", dst, url, NULL};
        SDL_PropertiesID pr = SDL_CreateProperties();
        SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_Process *proc = SDL_CreateProcessWithProperties(pr);
        SDL_DestroyProperties(pr);
        if (!proc) {
            say("Could not download", "curl is needed to fetch the fan pack");
            return 0;
        }
        int code;
        while (!SDL_WaitProcess(proc, false, &code)) {
            SDL_PathInfo info;
            if (SDL_GetPathInfo(dst, &info))
                set_progress(0.8 * (double)(got + (Sint64)info.size) / (double)total);
            SDL_Delay(200);
        }
        char *out = SDL_ReadProcess(proc, NULL, NULL);
        SDL_DestroyProcess(proc);
        if (code != 0) {
            say("Could not download the fan pack", out ? out : "");
            SDL_free(out);
            return 0;
        }
        SDL_free(out);
        char hex[65];
        say("Checking the download", files[i].name);
        if (!file_sha256(dst, hex, NULL) || SDL_strcmp(hex, files[i].sha)) {
            say("The download was damaged", files[i].name);
            return 0;
        }
        say("Unpacking", files[i].name);
        const char *targv[] = {"tar", "-xf", dst, "-C", mods, NULL};
        if (run(targv) != 0) {
            say("Could not unpack the fan pack", last_output);
            return 0;
        }
        SDL_RemovePath(dst);
        got += files[i].size;
        set_progress(0.8 * (double)got / (double)total);
    }
    /* mods.txt names its folders; FAN CARS & MAPS on */
    char mt[1024];
    path(mt, sizeof mt, user, "mods.txt");
    char *old = read_text(mt);
    char *add = SDL_malloc(4096);
    add[0] = 0;
    static const char *const lines[][2] = {{"cars", "Cars"}, {"maps", "Maps"}};
    for (int i = 0; i < 2; i++) {
        char l[1200];
        SDL_snprintf(l, sizeof l, "%s %s/%s\n", lines[i][0], mods, lines[i][1]);
        if (!old || !SDL_strstr(old, l))
            SDL_strlcat(add, l, 4096);
    }
    SDL_IOStream *io = SDL_IOFromFile(mt, "ab");
    if (io) {
        if (!old)
            SDL_IOprintf(io, "# 1NSANE: Recompiled: folders of fan cars and maps, one per line\n");
        SDL_IOprintf(io, "%s", add);
        SDL_CloseIO(io);
    }
    SDL_free(old);
    SDL_free(add);
    char op[1024];
    path(op, sizeof op, user, "options.txt");
    char *opts = read_text(op);
    if (!opts || !SDL_strstr(opts, "\nmods = on")) {
        io = SDL_IOFromFile(op, "ab");
        if (io) {
            SDL_IOprintf(io, "\nmods = on\n");
            SDL_CloseIO(io);
        }
    }
    SDL_free(opts);
    set_progress(1);
    say("Fan cars and maps added", "they show up in the game's car and map lists");
    return 1;
}

/* ------------------------------------------------------------------ tasks */
static char source[1024];               /* what the player chose: a folder or the installer */
static int want_fanpack;

static int setup_task(void *unused)
{
    (void)unused;
    char dir[1024], game[1024], mark[1024];
    path(game, sizeof game, user, "game");
    path(mark, sizeof mark, game, ".gog-checked");
    int ok = 1;
    if (source[0]) {
        SDL_PathInfo i;
        if (!SDL_GetPathInfo(source, &i)) {
            say("Can't find that", source);
            ok = 0;
        } else if (i.type == SDL_PATHTYPE_FILE) {
            ok = unpack_installer(source, dir, sizeof dir) && check_and_copy(dir);
            char tmp[1024];
            path(tmp, sizeof tmp, user, "build/unpack");
            remove_tree(tmp);
        } else if (find_game(source, dir, sizeof dir) || exists(source)) {
            if (!find_game(source, dir, sizeof dir))
                SDL_strlcpy(dir, source, sizeof dir);
            ok = check_and_copy(dir);
        }
    } else if (!exists(mark))
        ok = check_and_copy(game);
    if (ok)
        ok = build_game();
    if (ok && want_fanpack)
        ok = fan_pack();
    task_ok = ok;
    task_done = 1;
    return 0;
}

static int fanpack_task(void *unused)
{
    (void)unused;
    task_ok = fan_pack();
    task_done = 1;
    return 0;
}

static void start(SDL_ThreadFunction fn)
{
    task_done = 0;
    task_ok = 1;
    SDL_Thread *t = SDL_CreateThread(fn, "setup", NULL);
    SDL_DetachThread(t);
}

static int built(void)
{
    char stamp[1024];
    path(stamp, sizeof stamp, user, "bin/VERSION");
    char *s = read_text(stamp);
    int ok = s && !SDL_strcmp(s, version);
    SDL_free(s);
    char g[1024];
    path(g, sizeof g, user, "bin/1nsane" EXE);
    return ok && exists(g);
}

static void launch_game(int argc, char **argv)
{
    char g[1024];
    path(g, sizeof g, user, "bin/1nsane" EXE);
    const char *args[64];
    int k = 0;
    args[k++] = g;
    for (int i = 1; i < argc && k < 62; i++)
        if (SDL_strcmp(argv[i], "--setup"))
            args[k++] = argv[i];
    args[k] = NULL;
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (p)
        SDL_DestroyProcess(p);
}

/* where a GOG copy is often found */
static int detect(char *out, size_t n)
{
    char mark[1024], d0[1024];
    SDL_snprintf(mark, sizeof mark, "%s/game", user);
    if (find_game(mark, d0, sizeof d0)) {           /* (checked again before it is used) */
        SDL_strlcpy(out, d0, n);
        return 1;
    }
    const char *home = SDL_getenv("HOME");
    if (!home)
        home = SDL_getenv("USERPROFILE");
    if (!home)
        return 0;
    static const char *const places[] = {"GOG Games/1NSANE", "Games/Heroic/1NSANE", "Games/gog/1nsane",
                                         "Games/1nsane/drive_c/GOG Games/1NSANE", "GOG Games/Insane", NULL};
    for (int i = 0; places[i]; i++) {
        char p[1024], d[1024];
        SDL_snprintf(p, sizeof p, "%s/%s", home, places[i]);
        if (exists(p) && find_game(p, d, sizeof d)) {
            SDL_strlcpy(out, d, n);
            return 1;
        }
    }
#ifdef _WIN32
    static const char *const win[] = {"C:/GOG Games/1NSANE", "C:/Program Files (x86)/GOG Galaxy/Games/1NSANE",
                                      "C:/Program Files (x86)/GOG.com/1NSANE", NULL};
    for (int i = 0; win[i]; i++)
        if (exists(win[i])) {
            SDL_strlcpy(out, win[i], n);
            return 1;
        }
#endif
    return 0;
}

/* ------------------------------------------------------------------ drawing */
static SDL_Window *win;
static SDL_Renderer *ren;
static FT_Library ft;
static FT_Face face;
static float scale = 1;

typedef struct { char text[256]; int size; Uint32 color; SDL_Texture *tex; int w, h; Uint64 used; } TextTex;
static TextTex cache[96];
static Uint64 frame;

static SDL_Texture *text_tex(const char *s, int size, Uint32 rgb, int *w, int *h)
{
    for (int i = 0; i < 96; i++)
        if (cache[i].tex && cache[i].size == size && cache[i].color == rgb && !SDL_strcmp(cache[i].text, s)) {
            cache[i].used = frame;
            *w = cache[i].w, *h = cache[i].h;
            return cache[i].tex;
        }
    int slot = 0;
    for (int i = 1; i < 96; i++)
        if (!cache[i].tex || cache[i].used < cache[slot].used)
            slot = i;
    if (cache[slot].tex)
        SDL_DestroyTexture(cache[slot].tex);
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
    int asc = (int)(face->size->metrics.ascender >> 6), desc = (int)(-face->size->metrics.descender >> 6);
    int width = 0;
    for (const char *c = s; *c; c++)
        if (!FT_Load_Char(face, (unsigned char)*c, FT_LOAD_DEFAULT))
            width += (int)(face->glyph->advance.x >> 6);
    int ht = asc + desc;
    if (width < 1)
        width = 1;
    Uint8 *px = SDL_calloc((size_t)width * ht, 4);
    int pen = 0;
    for (const char *c = s; *c; c++) {
        if (FT_Load_Char(face, (unsigned char)*c, FT_LOAD_RENDER))
            continue;
        FT_Bitmap *b = &face->glyph->bitmap;
        int x0 = pen + face->glyph->bitmap_left, y0 = asc - face->glyph->bitmap_top;
        for (unsigned r = 0; r < b->rows; r++)
            for (unsigned k = 0; k < b->width; k++) {
                int x = x0 + (int)k, y = y0 + (int)r;
                if (x < 0 || y < 0 || x >= width || y >= ht)
                    continue;
                Uint8 *o = px + ((size_t)y * width + x) * 4;
                Uint8 a = b->buffer[r * (unsigned)b->pitch + k];
                if (a > o[3])
                    o[0] = (Uint8)(rgb >> 16), o[1] = (Uint8)(rgb >> 8), o[2] = (Uint8)rgb, o[3] = a;
            }
        pen += (int)(face->glyph->advance.x >> 6);
    }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, ht);
    SDL_UpdateTexture(t, NULL, px, width * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_free(px);
    SDL_strlcpy(cache[slot].text, s, sizeof cache[slot].text);
    cache[slot].size = size, cache[slot].color = rgb, cache[slot].tex = t, cache[slot].w = width, cache[slot].h = ht;
    cache[slot].used = frame;
    *w = width, *h = ht;
    return t;
}

static float text(float x, float y, const char *s, int size, Uint32 rgb)
{
    if (!*s)
        return 0;
    int w, h;
    SDL_Texture *t = text_tex(s, (int)(size * scale), rgb, &w, &h);
    SDL_FRect d = {x * scale, y * scale, (float)w, (float)h};
    SDL_RenderTexture(ren, t, NULL, &d);
    return (float)w / scale;
}

/* words wrapped to width; returns the height used */
static float text_wrapped(float x, float y, float width, const char *s, int size, Uint32 rgb)
{
    char line[512] = "", word[256];
    float yy = y;
    const char *p = s;
    while (*p) {
        size_t k = strcspn(p, " \n");
        if (k >= sizeof word)
            k = sizeof word - 1;
        SDL_memcpy(word, p, k);
        word[k] = 0;
        char test[768];
        SDL_snprintf(test, sizeof test, "%s%s%s", line, *line ? " " : "", word);
        int w, h;
        text_tex(test, (int)(size * scale), rgb, &w, &h);
        if (w / scale > width && *line) {
            text(x, yy, line, size, rgb);
            yy += size * 1.35f;
            SDL_strlcpy(line, word, sizeof line);
        } else
            SDL_strlcpy(line, test, sizeof line);
        p += k;
        if (*p == '\n') {
            text(x, yy, line, size, rgb);
            yy += size * 1.35f;
            line[0] = 0;
        }
        if (*p)
            p++;
    }
    if (*line) {
        text(x, yy, line, size, rgb);
        yy += size * 1.35f;
    }
    return yy - y;
}

static void rect(float x, float y, float w, float h, Uint32 rgb, Uint8 a)
{
    SDL_SetRenderDrawColor(ren, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, a);
    SDL_FRect r = {x * scale, y * scale, w * scale, h * scale};
    SDL_RenderFillRect(ren, &r);
}

/* ------------------------------------------------------------------ the pages */
enum { P_WELCOME, P_WORK, P_DONE, P_ERROR };
enum { B_FOUND, B_INSTALLER, B_FOLDER, B_PLAY, B_FANPACK, B_QUIT, B_BACK };
static int page = P_WELCOME, sel = 0;
static char found[1024];
static int have_found;
static int buttons[8], nbuttons;
static SDL_FRect button_rects[8];
static volatile int dialog_done;
static char dialog_result[1024];

static void dialog_cb(void *ud, const char *const *files, int filter)
{
    (void)ud, (void)filter;
    if (files && files[0])
        SDL_strlcpy(dialog_result, files[0], sizeof dialog_result);
    dialog_done = 1;
}

static const char *button_label(int b)
{
    switch (b) {
    case B_FOUND: return "Use the copy found here";
    case B_INSTALLER: return "Choose the GOG installer (.exe)";
    case B_FOLDER: return "Choose the installed game's folder";
    case B_PLAY: return "Play";
    case B_FANPACK: return "Add the fan cars and maps (1.9 GB download)";
    case B_QUIT: return "Quit";
    case B_BACK: return "Back";
    }
    return "";
}

static void press(int b)
{
    switch (b) {
    case B_FOUND:
        SDL_strlcpy(source, found, sizeof source);
        page = P_WORK;
        start(setup_task);
        break;
    case B_INSTALLER: {
        static const SDL_DialogFileFilter f[] = {{"GOG installer", "exe"}};
        dialog_done = 0, dialog_result[0] = 0;
        SDL_ShowOpenFileDialog(dialog_cb, NULL, win, f, 1, NULL, false);
        break;
    }
    case B_FOLDER:
        dialog_done = 0, dialog_result[0] = 0;
        SDL_ShowOpenFolderDialog(dialog_cb, NULL, win, NULL, false);
        break;
    case B_FANPACK:
        page = P_WORK;
        start(fanpack_task);
        break;
    case B_BACK:
        page = P_WELCOME;
        break;
    }
}

static void layout(void)
{
    nbuttons = 0;
    if (page == P_WELCOME) {
        if (have_found)
            buttons[nbuttons++] = B_FOUND;
        buttons[nbuttons++] = B_INSTALLER;
        buttons[nbuttons++] = B_FOLDER;
        buttons[nbuttons++] = B_QUIT;
    } else if (page == P_DONE) {
        buttons[nbuttons++] = B_PLAY;
        buttons[nbuttons++] = B_FANPACK;
        buttons[nbuttons++] = B_QUIT;
    } else if (page == P_ERROR) {
        buttons[nbuttons++] = B_BACK;
        buttons[nbuttons++] = B_QUIT;
    }
    if (sel >= nbuttons)
        sel = nbuttons - 1;
    if (sel < 0)
        sel = 0;
}

static void draw(void)
{
    frame++;
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    scale = (float)wh / 600.0f;
    float W = ww / scale;
    SDL_SetRenderDrawColor(ren, 10, 14, 28, 255);
    SDL_RenderClear(ren);
    rect(0, 0, W, 4, 0xf0d020, 255);
    text(48, 36, "1NSANE: Recompiled", 40, 0xf8d830);
    char st[256], de[1024];
    double pr;
    SDL_LockMutex(lock);
    SDL_strlcpy(st, status, sizeof st);
    SDL_strlcpy(de, detail, sizeof de);
    pr = progress;
    SDL_UnlockMutex(lock);
    float y = 112;
    if (page == P_WELCOME) {
        y += text_wrapped(48, y, W - 96,
                          "The off-road racer from 2000, rebuilt for today's computers. It needs your own copy of "
                          "1NSANE from GOG: this builds the game from it, once, on this computer.",
                          20, 0xe8e8f0);
        y += 12;
        if (have_found) {
            char m[1200];
            SDL_snprintf(m, sizeof m, "Found: %s", found);
            y += text_wrapped(48, y, W - 96, m, 16, 0x9ad0ff);
        } else
            y += text_wrapped(48, y, W - 96, "Show it where your copy is: the GOG installer, or the folder it was "
                                             "installed in.", 18, 0xb8b8c8);
    } else if (page == P_WORK || page == P_DONE || page == P_ERROR) {
        text(48, y, st, 26, page == P_ERROR ? 0xff7060 : 0xffffff);
        y += 44;
        y += text_wrapped(48, y, W - 96, de, 17, 0xb8b8c8);
        if (page == P_WORK && pr >= 0) {
            rect(48, 300, W - 96, 14, 0x303850, 255);
            rect(48, 300, (float)((W - 96) * pr), 14, 0xf0d020, 255);
        } else if (page == P_WORK) {
            float t = (float)(SDL_GetTicks() % 1600) / 1600.0f;
            rect(48, 300, W - 96, 14, 0x303850, 255);
            rect(48 + (W - 96 - 120) * t, 300, 120, 14, 0xf0d020, 255);
        }
    }
    layout();
    float by = 600 - 64.0f * nbuttons - 24;
    for (int i = 0; i < nbuttons; i++) {
        int on = i == sel;
        rect(48, by, W - 96, 52, on ? 0x2a3860 : 0x161c34, 255);
        if (on)
            rect(48, by, 6, 52, 0xf0d020, 255);
        text(72, by + 12, button_label(buttons[i]), 22, on ? 0xf8d830 : 0xe0e0e8);
        button_rects[i] = (SDL_FRect){48, by, W - 96, 52};
        by += 64;
    }
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ main */
static void find_folders(void)
{
    const char *b = SDL_getenv("INSANE_BUNDLE");
    if (b)
        SDL_strlcpy(bundle, b, sizeof bundle);
    else {
#ifdef _WIN32
        /* (the zip unpacks to one folder: the launcher and its bundle side by side) */
        SDL_strlcpy(bundle, SDL_GetBasePath(), sizeof bundle);
        size_t k = SDL_strlen(bundle);
        if (k && (bundle[k - 1] == '/' || bundle[k - 1] == '\\'))
            bundle[k - 1] = 0;
#else
        SDL_snprintf(bundle, sizeof bundle, "%s../lib/1nsane-recompiled", SDL_GetBasePath());
#endif
    }
    const char *u = SDL_getenv("INSANE_USER");
    if (u)
        SDL_strlcpy(user, u, sizeof user);
    else {
#ifdef _WIN32
        SDL_snprintf(user, sizeof user, "%s/1nsane-native", SDL_getenv("APPDATA"));
#else
        const char *x = SDL_getenv("XDG_DATA_HOME");
        if (x && *x)
            SDL_snprintf(user, sizeof user, "%s/1nsane-native", x);
        else
            SDL_snprintf(user, sizeof user, "%s/.local/share/1nsane-native", SDL_getenv("HOME"));
#endif
    }
    make_dirs(user);
    char vp[1024];
    path(vp, sizeof vp, bundle, "VERSION");
    char *v = read_text(vp);
    SDL_strlcpy(version, v ? v : "unknown", sizeof version);
    SDL_free(v);
    for (char *c = version; *c; c++)
        if (*c == '\n' || *c == '\r')
            *c = 0;
}

int main(int argc, char **argv)
{
    lock = SDL_CreateMutex();
    find_folders();
    int force = 0, fan = 0, no_launch = 0;
    const char *from = NULL;
    for (int i = 1; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--setup"))
            force = 1;
        else if (!SDL_strcmp(argv[i], "--setup-from") && i + 1 < argc)
            from = argv[++i];
        else if (!SDL_strcmp(argv[i], "--fan-pack"))
            fan = 1;
        else if (!SDL_strcmp(argv[i], "--no-launch"))
            no_launch = 1;
    }
    if (from) {                          /* in the terminal */
        headless = 1;
        SDL_strlcpy(source, from, sizeof source);
        want_fanpack = fan;
        setup_task(NULL);
        if (!task_ok) {
            SDL_Log("setup failed: %s: %s", status, detail);
            return 1;
        }
        if (!no_launch)
            launch_game(1, argv);
        return 0;
    }
    if (!force && built()) {
        launch_game(argc, argv);
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL: %s", SDL_GetError());
        return 1;
    }
    if (FT_Init_FreeType(&ft) || FT_New_Memory_Face(ft, font_1nsane_otf, (FT_Long)sizeof font_1nsane_otf, 0, &face)) {
        SDL_Log("FreeType could not open the font");
        return 1;
    }
    win = SDL_CreateWindow("1NSANE: Recompiled", 1000, 600, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    ren = SDL_CreateRenderer(win, NULL);
    SDL_SetRenderVSync(ren, 1);
    have_found = detect(found, sizeof found);
    int quit = 0, was_working = 0;
    /* INSANE_LAUNCHER_SHOT=FILE saves each page it shows as FILE-<page>.bmp and quits on the first, or with
       INSANE_LAUNCHER_PRESS=N (the Nth button of the first page) when the work it starts has ended. */
    const char *shot = SDL_getenv("INSANE_LAUNCHER_SHOT"), *auto_press = SDL_getenv("INSANE_LAUNCHER_PRESS");
    int shot_page = -1, page_frames = 0;
    while (!quit) {
        SDL_Event e;
        if (auto_press && frame == 5) {
            SDL_Event a = {0};
            a.type = SDL_EVENT_USER;
            SDL_PushEvent(&a);
        }
        while (SDL_PollEvent(&e)) {
            int act = -1;
            if (e.type == SDL_EVENT_USER && auto_press)
                act = SDL_atoi(auto_press);
            switch (e.type) {
            case SDL_EVENT_QUIT: quit = 1; break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_UP) sel--;
                else if (e.key.key == SDLK_DOWN) sel++;
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE) act = sel;
                else if (e.key.key == SDLK_ESCAPE && page != P_WORK) quit = 1;
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) sel--;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) sel++;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) act = sel;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                for (int i = 0; i < nbuttons; i++) {
                    SDL_FRect r = button_rects[i];
                    float x = e.motion.x / scale, y = e.motion.y / scale;
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                        sel = i;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                float x = e.button.x / scale, y = e.button.y / scale;
                for (int i = 0; i < nbuttons; i++) {
                    SDL_FRect r = button_rects[i];
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                        act = i;
                }
                break;
            }
            }
            if (act >= 0 && act < nbuttons && page != P_WORK) {
                int b = buttons[act];
                if (b == B_QUIT)
                    quit = 1;
                else if (b == B_PLAY) {
                    launch_game(argc, argv);
                    quit = 1;
                } else
                    press(b);
            }
        }
        if (dialog_done) {                /* a file or folder was chosen */
            dialog_done = 0;
            if (dialog_result[0]) {
                SDL_strlcpy(source, dialog_result, sizeof source);
                page = P_WORK;
                start(setup_task);
            }
        }
        if (page == P_WORK) {
            was_working = 1;
            if (task_done)
                page = task_ok ? P_DONE : P_ERROR, sel = 0;
        } else if (was_working && page != P_WORK)
            was_working = 0;
        draw();
        if (shot) {
            if (page != shot_page)
                shot_page = page, page_frames = 0;
            if (++page_frames == 20) {
                char f[1100];
                SDL_snprintf(f, sizeof f, "%s-%d.bmp", shot, page);
                SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
                if (sf) {
                    SDL_SaveBMP(sf, f);
                    SDL_DestroySurface(sf);
                }
                if (!auto_press || page == P_DONE || page == P_ERROR)
                    quit = 1;
            }
        }
    }
    SDL_Quit();
    return 0;
}
