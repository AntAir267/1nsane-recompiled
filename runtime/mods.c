/* Fan cars and maps (design point 8): mod folders listed in the user folder's mods.txt add
   their .idf archives to the game's Data\Vehicles and Data\Maps, read straight from their
   download zips (or loose .idf files). The game scans those two folders at start-up.

   mods.txt, one folder per line ("cars <folder>" / "maps <folder>", # comments). A zip may hold
   any number of .idf files; the first archive of a name wins (stock files always win).
   Archives are extracted once into <user>/modcache/ when the game first opens them.
   RAR downloads (also ones named .zip, and RAR self-extracting programs, which are only read,
   never run) go through the system's libarchive, loaded at run time ("other archives" below).
   A zip cut short by a byte or two still reads when its directory is whole; password-protected
   entries are left out (and say so in the log).

   With mods, the game's tables that cannot hold them all move to bigger memory (TABLES in the
   build config), each ending at an inaccessible page so an overflow faults instead of
   corrupting memory: the IDF loader's archive and directory-entry tables (fan archives hold
   far more files than the stock 6000) and the car and map lists, which held 200 each (see
   "the game's tables" below). A list holds up to 4096 names (the folder listing's size).
   Test runs (--test) never load mods unless INSANE_MODS is set. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#include "runtime.h"
#include "mods.h"
#include "image.h"
#include "options.h"
#include "funcs.h"

#define LIST_MAX 4096                   /* names in one folder listing (files.c), stock included */
#define NAME_MAX_GAME 15                /* the lists keep a car's or map's file name in 16 bytes */

typedef struct {
    char name[64];                      /* lower case, without .idf */
    char src[1024];                     /* zip or .idf path */
    char entry[256];                    /* entry inside the zip, "" for a loose file */
    char dir[1024];                     /* a folder of the same name next to a loose file, or "" */
    int usable;                         /* 0 unknown, 1 yes, -1 no (see usable()) */
    int other;                          /* in an archive libarchive reads, not a zip */
    long size;                          /* then the entry's size */
    int cached;                         /* its copy in the cache is known to be there (mods_path) */
} Mod;

static Mod *mods[2];
static int nmods[2], capmods[2];
int mods_active;

static const char *kind_dir[2] = {"Vehicles", "Maps"};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* the stock folder's names, read once (a fan archive's every .idf is checked against them) */
static char **stock_names[2];
static int nstock_names[2] = {-1, -1};

static int stock_has(int kind, const char *name)
{
    if (nstock_names[kind] < 0) {
        nstock_names[kind] = 0;
        char p[1400];
        snprintf(p, sizeof p, "%s/Data/%s", game_dir, kind_dir[kind]);
        DIR *d = opendir(p);
        struct dirent *e;
        int cap = 0;
        while (d && (e = readdir(d))) {
            if (nstock_names[kind] == cap) {
                cap = cap ? cap * 2 : 256;
                stock_names[kind] = realloc(stock_names[kind], cap * sizeof *stock_names[kind]);
            }
            stock_names[kind][nstock_names[kind]++] = strdup(e->d_name);
        }
        if (d) closedir(d);
    }
    char want[80];
    snprintf(want, sizeof want, "%s.idf", name);
    for (int i = 0; i < nstock_names[kind]; i++)
        if (!strcasecmp(stock_names[kind][i], want))
            return 1;
    return 0;
}

static int stock_count(int kind)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/Data/%s", game_dir, kind_dir[kind]);
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        size_t k = strlen(e->d_name);
        if (k > 4 && !strcasecmp(e->d_name + k - 4, ".idf"))
            n++;
    }
    closedir(d);
    return n;
}

static int is_idf(const char *file)
{
    size_t k = strlen(file);
    return k > 4 && !strcasecmp(file + k - 4, ".idf");
}

static Mod *add(int kind, const char *file, const char *src, const char *entry, const char *dir)
{
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    const char *bs = strrchr(base, '\\');
    base = bs ? bs + 1 : base;
    size_t k = strlen(base);
    if (k < 5 || k > 60 || strcasecmp(base + k - 4, ".idf"))
        return NULL;
    char name[64];
    for (size_t i = 0; i < k - 4; i++)
        name[i] = (char)tolower((unsigned char)base[i]);
    name[k - 4] = 0;
    if (k - 4 > NAME_MAX_GAME) {
        LOG(LOG_ALWAYS, "mods: %s.idf from %s: the game keeps at most %d letters of a name: left out",
            name, src, NAME_MAX_GAME);
        return NULL;
    }
    for (int i = 0; i < nmods[kind]; i++)
        if (!strcmp(mods[kind][i].name, name)) {
            LOG(LOG_FILE, "mods: %s.idf from %s is a duplicate (keeping %s)", name, src, mods[kind][i].src);
            return NULL;
        }
    if (stock_has(kind, name)) {
        LOG(LOG_FILE, "mods: %s.idf from %s has a stock car/map's name: left out", name, src);
        return NULL;
    }
    if (nmods[kind] == capmods[kind]) {
        capmods[kind] = capmods[kind] ? capmods[kind] * 2 : 256;
        mods[kind] = realloc(mods[kind], capmods[kind] * sizeof(Mod));
    }
    Mod *m = &mods[kind][nmods[kind]++];
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->src, sizeof m->src, "%s", src);
    snprintf(m->entry, sizeof m->entry, "%s", entry);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    m->usable = 0;
    m->other = 0;
    m->size = -1;
    m->cached = 0;
    return m;
}

/* `n` bytes to `out`, through a .part file so an interrupted write leaves nothing behind */
static int write_file(const char *out, const void *data, size_t n)
{
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.part", out);
    FILE *o = fopen(tmp, "wb");
    int ok = o && fwrite(data, 1, n, o) == n;
    if (o && fclose(o) != 0) ok = 0;
    ok = ok && rename(tmp, out) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

/* ------------------------------------------------------------------ zips */
/* The zip's central directory (malloc'ed), NULL if it has none. A download cut short by a byte
   or two keeps it: only the end record's last field, the comment's length, is gone. */
static uint8_t *zip_dir(FILE *f, uint32_t *n, uint32_t *cdsize, int *cut)
{
    if (fseek(f, 0, SEEK_END) != 0) return NULL;
    long size = ftell(f);
    long tail = size < 65557 ? size : 65557;
    if (tail < 20) return NULL;
    uint8_t *buf = malloc(tail), *cd = NULL;
    if (fseek(f, size - tail, SEEK_SET) == 0 && fread(buf, 1, tail, f) == (size_t)tail)
        for (long i = tail - 20; i >= 0 && !cd; i--) {
            if (le32(buf + i) != 0x06054b50) continue;
            uint32_t count = le16(buf + i + 10), cs = le32(buf + i + 12), co = le32(buf + i + 16);
            if ((uint64_t)co + cs > (uint64_t)(size - tail + i)) continue;
            cd = malloc(cs + 1);
            if (fseek(f, co, SEEK_SET) != 0 || fread(cd, 1, cs, f) != cs ||
                (count && (cs < 46 || le32(cd) != 0x02014b50))) {
                free(cd);
                cd = NULL;
                continue;
            }
            *n = count;
            *cdsize = cs;
            *cut = i + 22 > tail;
        }
    free(buf);
    return cd;
}

/* Every .idf entry in a zip; 0 if the file is not one */
static int scan_zip(int kind, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t n = 0, cdsize = 0;
    int cut = 0;
    uint8_t *cd = zip_dir(f, &n, &cdsize, &cut);
    fclose(f);
    if (!cd) return 0;
    if (cut)
        LOG(LOG_FILE, "mods: %s ends a little early (an incomplete download); its directory is whole", path);
    uint32_t p = 0;
    for (uint32_t i = 0; i < n && p + 46 <= cdsize; i++) {
        if (le32(cd + p) != 0x02014b50) break;
        uint32_t nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        if (p + 46 + nl > cdsize) break;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(nl < 255 ? nl : 255), (const char *)cd + p + 46);
        if (!(le16(cd + p + 8) & 1))
            add(kind, name, path, name, "");
        else if (is_idf(name))
            LOG(LOG_ALWAYS, "mods: %s in %s is password-protected: left out", name, path);
        p += 46 + nl + xl + cl;
    }
    free(cd);
    return 1;
}

/* ------------------------------------------------------------------ other archives */
/* RAR downloads, also ones named .zip, and RAR self-extracting programs (a Windows program with
   the archive appended: only read here, never run), through the system's libarchive. It is
   loaded when first needed and its few functions are declared here, so building needs no
   headers; without it such downloads are left out. libarchive does not unpack RAR 2.0's
   compression (RAR 2.9's and RAR 5's it does): such entries go through unar (The Unarchiver)
   when that is installed, which writes the entry to its standard output. */
struct archive;
struct archive_entry;
enum { LA_EOF = 1, LA_OK = 0, LA_WARN = -20 };
static struct {
    int tried, ok;
    struct archive *(*read_new)(void);
    int (*support_format_all)(struct archive *);
    int (*support_filter_all)(struct archive *);
    int (*open_memory)(struct archive *, const void *, size_t);
    int (*next_header)(struct archive *, struct archive_entry **);
    ssize_t (*read_data)(struct archive *, void *, size_t);
    int (*data_skip)(struct archive *);
    const char *(*error_string)(struct archive *);
    int (*read_free)(struct archive *);
    const char *(*pathname)(struct archive_entry *);
    int64_t (*size)(struct archive_entry *);
    int (*is_encrypted)(struct archive_entry *);
    const char *(*version)(void);
} la;

static int la_load(void)
{
    if (la.tried) return la.ok;
    la.tried = 1;
    void *h = dlopen("libarchive.so.13", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libarchive.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 0;
    int ok = 1;
#define LA_SYM(field, sym) ok = ok && (*(void **)&la.field = dlsym(h, sym)) != NULL
    LA_SYM(read_new, "archive_read_new");
    LA_SYM(support_format_all, "archive_read_support_format_all");
    LA_SYM(support_filter_all, "archive_read_support_filter_all");
    LA_SYM(open_memory, "archive_read_open_memory");
    LA_SYM(next_header, "archive_read_next_header");
    LA_SYM(read_data, "archive_read_data");
    LA_SYM(data_skip, "archive_read_data_skip");
    LA_SYM(error_string, "archive_error_string");
    LA_SYM(read_free, "archive_read_free");
    LA_SYM(pathname, "archive_entry_pathname");
    LA_SYM(size, "archive_entry_size");
    LA_SYM(is_encrypted, "archive_entry_is_encrypted");
    LA_SYM(version, "archive_version_string");
#undef LA_SYM
    if (!ok) {
        LOG(LOG_ALWAYS, "mods: the system's libarchive lacks functions the game needs (3.2 or later)");
        dlclose(h);
        return 0;
    }
    LOG(LOG_FILE, "mods: using %s", la.version());
    return la.ok = 1;
}

typedef struct {
    struct archive *a;
    void *map;
    size_t len, off;                    /* the file's size and where its archive starts */
} LaFile;

static void la_close(LaFile *lf)
{
    if (lf->a) la.read_free(lf->a);
    if (lf->map) munmap(lf->map, lf->len);
    lf->a = NULL;
    lf->map = NULL;
}

/* A file opened with libarchive, from the RAR archive's start in a self-extracting program */
static int la_open(LaFile *lf, const char *path)
{
    memset(lf, 0, sizeof *lf);
    if (!la_load()) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8) { close(fd); return 0; }
    lf->len = (size_t)st.st_size;
    lf->map = mmap(NULL, lf->len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (lf->map == MAP_FAILED) { lf->map = NULL; return 0; }
    const uint8_t *b = lf->map;
    if (b[0] == 'M' && b[1] == 'Z') {
        /* libarchive looks for the archive in such a program only from 64 KB on; these
           programs are smaller */
        const uint8_t *r = memmem(b, lf->len < (4u << 20) ? lf->len : (4u << 20), "Rar!\x1a\x07", 6);
        if (!r) { la_close(lf); return 0; }
        lf->off = (size_t)(r - b);
    }
    lf->a = la.read_new();
    if (!lf->a) { la_close(lf); return 0; }
    la.support_format_all(lf->a);
    la.support_filter_all(lf->a);
    if (la.open_memory(lf->a, b + lf->off, lf->len - lf->off) != LA_OK) { la_close(lf); return 0; }
    return 1;
}

/* Every .idf entry in an archive libarchive reads; 0 if it reads none in the file */
static int scan_other(int kind, const char *path)
{
    LaFile lf;
    if (!la_open(&lf, path)) return 0;
    if (lf.off)
        LOG(LOG_FILE, "mods: %s is a self-extracting program: reading the RAR archive in it", path);
    struct archive_entry *e;
    int r, entries = 0;
    while ((r = la.next_header(lf.a, &e)) == LA_OK || r == LA_WARN) {
        entries++;
        const char *name = la.pathname(e);
        if (name && is_idf(name)) {
            if (la.is_encrypted(e) > 0)
                LOG(LOG_ALWAYS, "mods: %s in %s is password-protected: left out", name, path);
            else {
                Mod *m = add(kind, name, path, name, "");
                if (m) {
                    m->other = 1;
                    m->size = (long)la.size(e);
                }
            }
        }
        la.data_skip(lf.a);
    }
    if (r != LA_EOF)                    /* some RAR 2 archives end in a block it cannot read */
        LOG(LOG_FILE, "mods: %s: %s after %d entries", path, la.error_string(lf.a), entries);
    la_close(&lf);
    return entries > 0;
}

/* A download: a zip, or an archive libarchive reads */
static void scan_archive(int kind, const char *path)
{
    if (scan_zip(kind, path) || scan_other(kind, path))
        return;
    if (!la.ok)
        LOG(LOG_ALWAYS, "mods: %s is not a zip; other archives (RAR) need the system's libarchive "
            "(libarchive.so.13), which is not installed: left out", path);
    else
        LOG(LOG_ALWAYS, "mods: %s is neither a zip nor another archive libarchive reads (or is damaged): "
            "left out", path);
}

static int extract_other(const Mod *m, const char *out)
{
    LaFile lf;
    if (!la_open(&lf, m->src)) return 0;
    struct archive_entry *e;
    int ok = 0, found = 0;
    while (!found) {
        int r = la.next_header(lf.a, &e);
        if (r != LA_OK && r != LA_WARN) break;
        const char *name = la.pathname(e);
        if (!name || strcmp(name, m->entry)) {
            la.data_skip(lf.a);
            continue;
        }
        found = 1;
        size_t cap = m->size >= 0 ? (size_t)m->size + 1 : 1u << 20, n = 0;
        uint8_t *buf = malloc(cap);
        ssize_t k;
        while ((k = la.read_data(lf.a, buf + n, cap - n)) > 0)
            if ((n += (size_t)k) == cap)
                buf = realloc(buf, cap *= 2);
        if (k < 0)
            LOG(LOG_FILE, "mods: libarchive cannot unpack %s from %s: %s", m->entry, m->src, la.error_string(lf.a));
        ok = k == 0 && (m->size < 0 || n == (size_t)m->size) && write_file(out, buf, n);
        free(buf);
    }
    la_close(&lf);
    return ok;
}

static int extract_unar(const Mod *m, const char *out)
{
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.part", out);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
#ifdef _WIN32
    /* Windows: 7-Zip, which reads every RAR version (-spd: the entry name is not a pattern) */
    char *argv[] = {"7z", "e", "-so", "-y", "-spd", (char *)m->src, (char *)m->entry, NULL};
    static const char tool[] = "7z", tool_desc[] = "7-Zip (7z.exe)";
#else
    char *argv[] = {"unar", "-q", "-no-recursion", "-o", "-", (char *)m->src, (char *)m->entry, NULL};
    static const char tool[] = "unar", tool_desc[] = "unar (The Unarchiver)";
#endif
    pid_t pid;
    int status = 0, ok = posix_spawnp(&pid, tool, &fa, NULL, argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    if (!ok) {
        unlink(tmp);
        LOG(LOG_ALWAYS, "mods: %s from %s needs %s, which is not installed", m->entry, m->src, tool_desc);
        return 0;
    }
    struct stat st;
    ok = waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
         stat(tmp, &st) == 0 && st.st_size > 0 && (m->size < 0 || st.st_size == m->size) && rename(tmp, out) == 0;
    if (!ok) unlink(tmp);
    else LOG(LOG_FILE, "mods: unpacked %s from %s with %s", m->entry, m->src, tool);
    return ok;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void scan_folder(int kind, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        LOG(LOG_ALWAYS, "mods: cannot open folder %s", dir);
        return;
    }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) { cap = cap ? cap * 2 : 256; names = realloc(names, cap * sizeof *names); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    for (int i = 0; i < n; i++) {
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        size_t k = strlen(names[i]);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            if (k > 4 && (!strcasecmp(names[i] + k - 4, ".zip") || !strcasecmp(names[i] + k - 4, ".rar")))
                scan_archive(kind, p);
            else if (k > 4 && !strcasecmp(names[i] + k - 4, ".idf")) {
                /* a pack meant to be unpacked into the game's folder: the archive may hold just
                   the car's .vht, its other files in a folder of the same name */
                char folder[2048] = "";
                for (int j = 0; j < n && !folder[0]; j++)
                    if (strlen(names[j]) == k - 4 && !strncasecmp(names[j], names[i], k - 4)) {
                        snprintf(folder, sizeof folder, "%s/%s", dir, names[j]);
                        if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode))
                            folder[0] = 0;
                    }
                add(kind, names[i], p, "", folder);
            }
        }
    }
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* ------------------------------------------------------------------ the game's tables */
/* Moves a TABLES entry (build config) to room for `entries`; `limit` is what the game's own
   capacity checks then allow. */
static const TableEntry *table_move(const char *name, uint32_t entries, uint32_t limit)
{
    for (const TableEntry *t = table_list; t->name; t++)
        if (!strcmp(t->name, name)) {
            *t->reloc = vm_alloc_table(entries * t->entry);
            if (!*t->reloc)
                fatal("mods: no memory for a bigger %s table", name);
            *t->limit = limit;
            LOG(LOG_FILE, "mods: %s table moved to %08x, %u entries, limit %u", name, *t->reloc, entries, limit);
            return t;
        }
    fatal("mods: no table %s in this build", name);
}

/* The car and map lists (design point 8). Start-up (0x47d8a0) reads data\vehicles.cfg and
   data\scenes.cfg (GOG has neither) and then lists Data\Vehicles\*.idf and Data\Maps\*.idf,
   at most 200 lines and 200 names for each list, all into fixed tables of 200 entries. Here
   the tables move to room for everything mounted (TABLES "cars", "maps"; "net_cars", the
   cars a network game offers, has room for 3200), the 200s become that size, and two things
   that cannot move are worked around:
   - the folder listing goes into 200 names of 64 bytes at the top of start-up's stack frame,
     right below its return address: start-up (which goes on to run the whole session) runs
     lower on the stack, with room above its frame for the rest of the listing, which
     overwrites the copy of its return address there (the hook returns for it);
   - each car class (a group letter: 4x4, sports, ...) keeps its cars in a list of 32, 16
     classes of 0x8c bytes at 0x1c0dd18, and nearly 300 fan cars are 4x4s: only the lists'
     base moves (TABLES "class_lists"), so a long list runs into the next class's list instead
     of over the counts and the car list, and the one list the game reads (the championship
     class's, when choosing a vehicle for a championship) is written out again right before.
     Classes past the 16th land in the old car list, now unused: room for 107 in all. */
#define CAR_SIZE 0x40                   /* car list entry: group letter at +0x30, class at +0x34 */
#define CAR_GROUP 0x30
#define CAR_CLASS 0x34
#define CLASS_SLOTS 35                  /* 0x8c bytes per class: a list starts every 35 numbers */
#define CLASSES_ROOM 107
#define CHAMPS 5                        /* championship records: group letter, class number */
#define CHAMP_SIZE 0x7f0

static uint32_t stack_room;

static int startup_hook(CPU *c)
{
    uint32_t esp = c->esp, ret = R32(esp);
    c->esp = esp - stack_room;
    W32(c->esp, ret);
    F_0047d8a0_orig(c);
    c->esp = esp + 4;
    c->ret = ret;           /* its copy of the return address is under the longer listings */
    return 1;
}

/* data\champdef.cfg is read (0x47d280) once both lists are complete, right before the classes
   are built: one class for each championship and one for each other group letter of a car.
   With CAR CLASSES REORGANIZED (classes.c) the letters and the list's order change here; fan
   cars that share a name get a tag (carnames.c). */
static int champdef_hook(CPU *c)
{
    F_0047d280_orig(c);
    classes_letters();
    carnames_tag();
    uint8_t seen[256] = {0};
    uint32_t classes = CHAMPS, n = R32(GAME_CAR_COUNT);
    for (int k = 0; k < CHAMPS; k++)
        seen[R8(GAME_CHAMPS + k * CHAMP_SIZE)] = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t g = (uint8_t)R8(reloc_cars + i * CAR_SIZE + CAR_GROUP);
        classes += !seen[g];
        seen[g] = 1;
    }
    logmsg(LOG_ALWAYS, "mods: the game lists %u cars in %u classes and %u maps", n, classes, R32(GAME_MAP_COUNT));
    if (classes > CLASSES_ROOM)
        fatal("mods: the cars have %u group letters; the game has room for %d classes", classes, CLASSES_ROOM);
    return 1;
}

static int class_list_hook(CPU *c)
{
    (void)c;
    uint32_t champ = R32(GAME_CHAMP);
    if (champ >= CHAMPS)
        return 0;
    uint32_t cls = R32(GAME_CHAMPS + champ * CHAMP_SIZE + 4);
    if (cls >= CLASSES_ROOM)
        return 0;
    uint32_t n = R32(GAME_CAR_COUNT), k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (R32(reloc_cars + i * CAR_SIZE + CAR_CLASS) == cls)
            W32(reloc_class_lists + (cls * CLASS_SLOTS + k++) * 4, i);
    return 0;
}

/* Vehicle > Browse's steps (0x479000 back, 0x479180 on): the championship class's list first;
   after the step Browse's position of ours (classes.c) follows at once, so the list's own test
   for a wrap round (the position not having moved on) sees the step */
static int vehicle_prev_hook(CPU *c)
{
    class_list_hook(c);
    F_00479000_orig(c);
    classes_browse_sync();
    return 1;
}

static int vehicle_next_hook(CPU *c)
{
    class_list_hook(c);
    F_00479180_orig(c);
    classes_browse_sync();
    return 1;
}

static void lists_init(int cars, int maps)
{
    /* each list: the .cfg lines and the folder listing, both up to the limit */
    uint32_t lc = cars > 200 ? (uint32_t)cars : 200, lm = maps > 200 ? (uint32_t)maps : 200;
    table_move("cars", 2 * lc, lc);
    table_move("maps", 2 * lm, lm);
    table_move("class_lists", CLASSES_ROOM * CLASS_SLOTS + 2 * lc, 1);
    table_move("net_cars", 2 * lc > 3200 ? 2 * lc : 3200, 3200);
    stack_room = ((lc > lm ? lc : lm) - 200) * 64;
    hook_startup = startup_hook;
    hook_champdef = champdef_hook;
    hook_vehicle_prev = vehicle_prev_hook;
    hook_vehicle_next = vehicle_next_hook;
    hook_myvehicle_prev = hook_myvehicle_next = hook_champ_vehicle = class_list_hook;
}

void mods_init(void)
{
    if (deterministic && !getenv("INSANE_MODS"))      /* test runs: only when asked */
        return;
    if (!option_bool("mods", 1)) {
        logmsg(LOG_ALWAYS, "mods: fan cars and maps are turned off in the options");
        return;
    }
    char cfg[1200];
    snprintf(cfg, sizeof cfg, "%s/mods.txt", user_dir);
    FILE *f = fopen(cfg, "r");
    if (!f)
        return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        int kind = !strncasecmp(s, "cars ", 5) ? 0 : !strncasecmp(s, "maps ", 5) ? 1 : -1;
        if (kind < 0) {
            LOG(LOG_ALWAYS, "mods.txt: \"%s\" should start with cars or maps", s);
            continue;
        }
        s += 5;
        while (*s == ' ' || *s == '\t') s++;
        scan_folder(kind, s);
    }
    fclose(f);
    int nstock[2];
    for (int kind = 0; kind < 2; kind++) {
        nstock[kind] = stock_count(kind);
        int room = LIST_MAX - nstock[kind];
        if (room < 0) room = 0;
        if (nmods[kind] > room) {
            for (int i = room; i < nmods[kind]; i++)
                LOG(LOG_FILE, "mods: list full (%d names): %s left out", LIST_MAX, mods[kind][i].src);
            logmsg(LOG_ALWAYS, "mods: %d fan %s left out: a list holds %d (see the log with --log file)",
                   nmods[kind] - room, kind ? "maps" : "cars", LIST_MAX);
            nmods[kind] = room;
        }
    }
    mods_active = nmods[0] + nmods[1] > 0;
    if (mods_active) {
        /* fan archives together hold far more files than the stock directory table allows */
        table_move("idf_archives", 2048, 2048);
        table_move("idf_entries", 262144, 262144);
        lists_init(nstock[0] + nmods[0], nstock[1] + nmods[1]);
        picker_init();
        classes_init();
    }
    logmsg(LOG_ALWAYS, "mods: %d fan cars, %d fan maps", nmods[0], nmods[1]);
}

/* ------------------------------------------------------------------ for files.c */
static int extract_zip(const Mod *m, const char *out)
{
    FILE *z = fopen(m->src, "rb");
    if (!z) return 0;
    /* find the entry's local header through the central directory again */
    uint32_t n = 0, cdsize = 0;
    int cut = 0, ok = 0;
    uint8_t *cd = zip_dir(z, &n, &cdsize, &cut);
    uint32_t p = 0;
    for (uint32_t i = 0; cd && i < n && p + 46 <= cdsize; i++) {
        uint32_t nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        if (p + 46 + nl <= cdsize && nl == strlen(m->entry) && !memcmp(cd + p + 46, m->entry, nl)) {
            uint32_t method = le16(cd + p + 10), csize = le32(cd + p + 20), usize = le32(cd + p + 24);
            uint32_t lho = le32(cd + p + 42);
            uint8_t lh[30];
            if (fseek(z, lho, SEEK_SET) == 0 && fread(lh, 1, 30, z) == 30 && le32(lh) == 0x04034b50 &&
                fseek(z, lho + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET) == 0) {
                uint8_t *in = malloc(csize ? csize : 1), *outb = malloc(usize ? usize : 1);
                if (fread(in, 1, csize, z) == csize) {
                    if (method == 0 && csize == usize) {
                        memcpy(outb, in, usize);
                        ok = 1;
                    } else if (method == 8) {
                        z_stream s = {0};
                        s.next_in = in; s.avail_in = csize;
                        s.next_out = outb; s.avail_out = usize;
                        if (inflateInit2(&s, -15) == Z_OK) {
                            ok = inflate(&s, Z_FINISH) == Z_STREAM_END && s.total_out == usize;
                            inflateEnd(&s);
                        }
                    }
                }
                ok = ok && write_file(out, outb, usize);
                free(in);
                free(outb);
            }
            break;
        }
        p += 46 + nl + xl + cl;
    }
    free(cd);
    fclose(z);
    return ok;
}

static int extract(const Mod *m, const char *out)
{
    if (!m->other)
        return extract_zip(m, out);
    return extract_other(m, out) || extract_unar(m, out);
}

/* The host path of a fan archive, extracting it into the cache the first time. */
int mods_path(int kind, const char *name_idf, char *out, size_t n)
{
    if (!mods_active) return 0;
    char name[64];
    size_t k = strlen(name_idf);
    if (k < 5 || k > 63 || strcasecmp(name_idf + k - 4, ".idf")) return 0;
    for (size_t i = 0; i < k - 4; i++) name[i] = (char)tolower((unsigned char)name_idf[i]);
    name[k - 4] = 0;
    for (int i = 0; i < nmods[kind]; i++) {
        Mod *m = &mods[kind][i];
        if (strcmp(m->name, name)) continue;
        if (!m->entry[0]) {
            snprintf(out, n, "%s", m->src);
            return 1;
        }
        /* (the game asks for an archive many times at start-up: the folders are made once, and
           a copy once found is not looked for again) */
        static int made[2];
        char dir[1300];
        snprintf(dir, sizeof dir, "%s/modcache/%s", user_dir, kind_dir[kind]);
        if (!made[kind]) {
            char top[1300];
            snprintf(top, sizeof top, "%s/modcache", user_dir);
            mkdir(top, 0755);
            mkdir(dir, 0755);
            made[kind] = 1;
        }
        snprintf(out, n, "%s/%s.idf", dir, m->name);
        struct stat st;
        if (m->cached || stat(out, &st) == 0) {
            m->cached = 1;
            return 1;
        }
        if (!extract(m, out)) {
            LOG(LOG_ALWAYS, "mods: cannot extract %s from %s", m->entry, m->src);
            return 0;
        }
        LOG(LOG_FILE, "mods: extracted %s from %s", m->entry, m->src);
        m->cached = 1;
        return 1;
    }
    return 0;
}

/* `rel` under `base`, ignoring case; 1 if it exists */
static int find_ci(const char *base, const char *rel, char *out, size_t n)
{
    char cur[2048], tmp[1024];
    snprintf(cur, sizeof cur, "%s", base);
    snprintf(tmp, sizeof tmp, "%s", rel);
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        DIR *d = opendir(cur);
        if (!d) return 0;
        struct dirent *e;
        int found = 0;
        while (!found && (e = readdir(d)))
            if (!strcasecmp(e->d_name, t)) {
                size_t k = strlen(cur);
                snprintf(cur + k, sizeof cur - k, "/%s", e->d_name);
                found = 1;
            }
        closedir(d);
        if (!found) return 0;
    }
    snprintf(out, n, "%s", cur);
    return 1;
}

/* A game path under Data\Vehicles or Data\Maps ("Data/Vehicles/x/..." relative to the game
   folder) that a fan car's or map's folder of loose files supplies: its host path. */
int mods_file(const char *rel, char *out, size_t n)
{
    if (!mods_active) return 0;
    int kind = !strncasecmp(rel, "Data/Vehicles/", 14) ? 0 : !strncasecmp(rel, "Data/Maps/", 10) ? 1 : -1;
    if (kind < 0) return 0;
    const char *file = rel + (kind ? 10 : 14), *slash = strchr(file, '/');
    size_t k = slash ? (size_t)(slash - file) : strlen(file);
    if (!slash && k > 4 && !strcasecmp(file + k - 4, ".idf"))
        return mods_path(kind, file, out, n);
    char name[64];
    if (k == 0 || k > NAME_MAX_GAME) return 0;
    for (size_t i = 0; i < k; i++) name[i] = (char)tolower((unsigned char)file[i]);
    name[k] = 0;
    for (int i = 0; i < nmods[kind]; i++) {
        Mod *m = &mods[kind][i];
        if (!strcmp(m->name, name))
            return m->dir[0] && find_ci(m->dir, slash ? slash + 1 : "", out, n);
    }
    return 0;
}

/* A car archive that holds nothing but the car's .vht (a pack's list entry whose files sit in
   a folder next to it, or an incomplete download) makes the game crash as soon as it shows
   the car, unless the folder is there. Such cars stay out of the list. */
static int usable(int kind, Mod *m)
{
    if (m->usable)
        return m->usable > 0;
    m->usable = 1;
    if (kind != 0 || m->dir[0])
        return 1;
    char path[1400], idf[80];
    snprintf(idf, sizeof idf, "%s.idf", m->name);
    FILE *f = mods_path(kind, idf, path, sizeof path) ? fopen(path, "rb") : NULL;
    uint8_t h[20], c[4];
    int read = 0, other = 0;
    if (f && fread(h, 1, 20, f) == 20 && !memcmp(h, "FFFL", 4)) {
        /* directory (tools/idfx.py in 1nsane-rip): count, 64-byte entries, name first */
        uint32_t ver = le32(h + 8), off = le32(h + 12), flags = le32(h + 16);
        if (ver == 0x10100) off ^= 0x123;
        else flags = 0;
        if (fseek(f, off, SEEK_SET) == 0 && fread(c, 1, 4, f) == 4 && le32(c) <= 100000) {
            uint32_t count = le32(c);
            uint8_t *d = malloc(count * 64 + 1);
            if (fread(d, 1, count * 64, f) == count * 64) {
                read = 1;
                uint8_t key = (uint8_t)((flags & 0xff) + 0x27);
                for (uint32_t i = 0; flags >> 24 == 1 && i < count * 64; i++) {
                    d[i] ^= key;
                    key = (uint8_t)(key * 5 + d[i] + i);
                }
                for (uint32_t i = 0; i < count && !other; i++) {
                    char nm[0x39];
                    memcpy(nm, d + i * 64, 0x38);
                    nm[0x38] = 0;
                    size_t k = strlen(nm);
                    other = k < 4 || strcasecmp(nm + k - 4, ".vht");
                }
            }
            free(d);
        }
    }
    if (f) fclose(f);
    if (read && !other) {
        m->usable = -1;
        logmsg(LOG_ALWAYS, "mods: %s.idf from %s holds no more than the car's description: left out", m->name, m->src);
    }
    return m->usable > 0;
}

/* The download a fan car or map comes from (its zip, RAR or loose .idf), NULL if `name` (the
   list's file name, without .idf) is not a fan one */
const char *mods_download(int kind, const char *name)
{
    for (int i = 0; mods_active && i < nmods[kind]; i++)
        if (!strcasecmp(mods[kind][i].name, name))
            return mods[kind][i].src;
    return NULL;
}

int mods_list(int kind, char names[][64], int max)
{
    int n = 0;
    for (int i = 0; i < nmods[kind] && n < max; i++)
        if (usable(kind, &mods[kind][i]))
            snprintf(names[n++], 64, "%s.idf", mods[kind][i].name);
    return n;
}
