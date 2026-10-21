/* Intro videos (design point 7): a stand-in for DirectShow's multimedia streaming, the part
   the game uses (0x43df50): IAMMultiMediaStream opening an AVI, a DirectDraw video stream and
   one stream sample whose Update() puts the next frame into a DirectDraw surface, plus the
   soundtrack played by the default renderer. The Indeo 5 AVIs are decoded by the user's
   ffmpeg (no ffmpeg, no intros: the game then skips them, as on a PC without the codec).
   Only in a window: headless and --test runs skip the intros. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"
#include "options.h"
#include "sound.h"

extern char **environ;

#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define MS_S_ENDOFSTREAM 0x00040003u

static const uint8_t CLSID_AMMMS_4[8] = {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45};
static const uint8_t MSPID_4[8] = {0x8f, 0xdf, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d};
static const uint8_t IID_DDMS_4[8] = {0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d};

static ComClass mms_cls, vs_cls, smp_cls;

typedef struct {
    uint32_t obj, vstream, sample;   /* program interfaces */
    uint32_t dd;                     /* the IDirectDraw the video stream draws with */
    int refs, opened, running;
    char path[2048];
    int w, h, fps_num, fps_den;
    /* sample surface */
    uint32_t surf;
    int bpp;
    char pixfmt[16];
    /* decoding */
    pid_t vpid;
    int vfd;
    uint8_t *frame;
    long frames_shown;
    double start_ms;
    int eof;
    int voice;
    int16_t *pcm;
    uint32_t pcm_frames;
} Movie;

static Movie *movie;                 /* the game plays one video at a time */

/* Skipping (Ant: "can we have controller be able to skip the intro videos? and the skips should be
   instantaneous"). The game skips a video when GetAsyncKeyState says Escape is down, looking once a
   video frame (0x43e1f0), so a quick tap between two looks was lost, and a controller never skipped.
   Escape and any controller button pressed while a video plays are kept until the game next looks
   (user32.c), then it ends at once; each video needs its own press. */
static int skip_wanted;

void video_skip_press(void)
{
    if (movie && movie->running)
        skip_wanted = 1;
}

int video_skip_taken(void)
{
    int s = skip_wanted && movie && movie->running;
    skip_wanted = 0;
    return s;
}

static int ffmpeg_ok(void)
{
    static int ok = -1;
    if (ok < 0) {
        const char *path = getenv("PATH");
        char p[4096];
        ok = 0;
#ifdef _WIN32
        /* Windows: ';' between folders, the program is ffmpeg.exe (next to ours or on PATH) */
        static const char sep = ';', exe[] = "ffmpeg.exe";
#define X_OK_HOST 0
#else
        static const char sep = ':', exe[] = "ffmpeg";
#define X_OK_HOST X_OK
#endif
        for (const char *s = path; s && *s && !ok;) {
            const char *e = strchr(s, sep);
            size_t n = e ? (size_t)(e - s) : strlen(s);
            snprintf(p, sizeof p, "%.*s/%s", (int)n, s, exe);
            ok = access(p, X_OK_HOST) == 0;
            s = e ? e + 1 : NULL;
        }
    }
    return ok;
}

/* run ffmpeg with argv, return a pipe to its stdout */
static int spawn(char *const argv[], pid_t *pid)
{
    int fd[2];
    if (pipe(fd))
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    int r = posix_spawnp(pid, "ffmpeg", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (r) {
        close(fd[0]);
        return -1;
    }
    return fd[0];
}

static int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        got += r;
    }
    return 1;
}

static int probe(Movie *m)
{
    /* ffprobe isn't needed: ask ffmpeg for the stream line on stderr would be fragile, so read
       the AVI header (avih: microseconds per frame, frames, width, height). */
    FILE *f = fopen(m->path, "rb");
    if (!f) return 0;
    uint8_t h[256];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    for (size_t i = 0; i + 64 < n; i++)
        if (!memcmp(h + i, "avih", 4)) {
            uint32_t usec, w, ht;
            memcpy(&usec, h + i + 8, 4);
            memcpy(&w, h + i + 8 + 32, 4);
            memcpy(&ht, h + i + 8 + 36, 4);
            m->w = (int)w;
            m->h = (int)ht;
            m->fps_num = 1000000;
            m->fps_den = usec ? (int)usec : 40000;
            return m->w > 0 && m->h > 0;
        }
    return 0;
}

static void movie_stop_decoders(Movie *m)
{
    if (m->vpid > 0) {
        kill(m->vpid, SIGTERM);
        close(m->vfd);
        waitpid(m->vpid, NULL, 0);
        m->vpid = 0;
    }
    if (m->voice >= 0) {
        sound_stream_stop(m->voice);
        m->voice = -1;
    }
}

/* The last release stops decoding and frees the big buffers. The record itself stays: the
   game holds several interfaces of one movie and may touch one after another is gone. */
static void movie_free(Movie *m)
{
    movie_stop_decoders(m);
    free(m->frame);
    free(m->pcm);
    m->frame = NULL;
    m->pcm = NULL;
    m->eof = 1;
    if (movie == m) movie = NULL;
}

/* ------------------------------------------------------------------ IAMMultiMediaStream */
METHOD(mms_QueryInterface, 3) { W32(ARG(2), ARG(0)); ((Movie *)com_host(ARG(0)))->refs++; return 0; }
METHOD(mms_AddRef, 1) { return ++((Movie *)com_host(ARG(0)))->refs; }
METHOD(mms_Release, 1)
{
    Movie *m = com_host(ARG(0));
    int r = --m->refs;
    if (r <= 0) movie_free(m);
    return r;
}
METHOD(mms_GetInformation, 3) { if (ARG(1)) W32(ARG(1), 3); if (ARG(2)) W32(ARG(2), 0); return 0; }

METHOD(mms_GetMediaStream, 3)
{
    Movie *m = com_host(ARG(0));
    uint32_t purpose = ARG(1);
    if (R32(purpose) == 0xa35ff56au && m->vstream) {      /* MSPID_PrimaryVideo */
        m->refs++;
        W32(ARG(2), m->vstream);
        return 0;
    }
    W32(ARG(2), 0);
    return 0x80040203u;                  /* MS_E_NOSTREAM */
}

METHOD(mms_EnumMediaStreams, 3) { return 1; }
METHOD(mms_GetState, 2) { W32(ARG(1), ((Movie *)com_host(ARG(0)))->running); return 0; }

METHOD(mms_SetState, 2)
{
    Movie *m = com_host(ARG(0));
    if (ARG(1) == 1 && !m->running) {
        m->running = 1;
        skip_wanted = 0;                 /* (a press for the video before is not this one's) */
        m->start_ms = now_ms();
        m->frames_shown = 0;
        if (m->pcm)
            m->voice = sound_stream_start(m->pcm, m->pcm_frames, 2, 44100);
        LOG(LOG_DDRAW, "video: play %s (%dx%d, %.2f fps)", m->path, m->w, m->h, (double)m->fps_num / m->fps_den);
    } else if (ARG(1) == 0 && m->running) {
        m->running = 0;
        movie_stop_decoders(m);
    }
    return 0;
}

METHOD(mms_GetTime, 2) { Movie *m = com_host(ARG(0)); uint64_t t = (uint64_t)((now_ms() - m->start_ms) * 10000); W64(ARG(1), t); return 0; }
METHOD(mms_GetDuration, 2) { return E_NOTIMPL; }
METHOD(mms_Seek, 3) { return E_NOTIMPL; }
METHOD(mms_GetEndOfStreamEventHandle, 2) { return E_NOTIMPL; }
METHOD(mms_Initialize, 4) { return 0; }
METHOD(mms_GetFilterGraph, 2) { W32(ARG(1), 0); return E_NOTIMPL; }
METHOD(mms_GetFilter, 2) { W32(ARG(1), 0); return E_NOTIMPL; }

METHOD(mms_AddMediaStream, 5)
{
    Movie *m = com_host(ARG(0));
    uint32_t obj = ARG(1), purpose = ARG(2), out = ARG(4);
    if (R32(purpose) == 0xa35ff56au) {                    /* video: a DirectDraw stream */
        m->dd = obj;
        if (!m->vstream)
            m->vstream = com_new(&vs_cls, m);
        if (out) { m->refs++; W32(out, m->vstream); }
        return 0;
    }
    if (out) W32(out, 0);
    return 0;                                             /* audio: the default renderer */
}

METHOD(mms_OpenFile, 3)
{
    Movie *m = com_host(ARG(0));
    char win[600];
    int k = 0;
    for (uint32_t p = ARG(1); R16(p) && k < 590; p += 2)
        win[k++] = (char)(R16(p) < 0x80 ? R16(p) : '?');
    win[k] = 0;
    if (!path_to_host(win, m->path, sizeof m->path, 0) || !probe(m)) {
        LOG(LOG_ALWAYS, "video: cannot open %s", win);
        return 0x80070002u;
    }
    /* the soundtrack, decoded up front (a minute and a half is ~16 MB) */
    char *aargs[] = {"ffmpeg", "-v", "error", "-i", m->path, "-vn", "-f", "s16le", "-ac", "2",
                     "-ar", "44100", "-", NULL};
    pid_t pid;
    int fd = spawn(aargs, &pid);
    if (fd >= 0) {
        size_t cap = 1 << 20, len = 0;
        uint8_t *buf = malloc(cap);
        for (;;) {
            if (len == cap) buf = realloc(buf, cap *= 2);
            ssize_t r = read(fd, buf + len, cap - len);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            len += r;
        }
        close(fd);
        waitpid(pid, NULL, 0);
        m->pcm = (int16_t *)buf;
        m->pcm_frames = (uint32_t)(len / 4);
    }
    m->opened = 1;
    LOG(LOG_DDRAW, "video: opened %s (%dx%d, %u sound frames)", win, m->w, m->h, m->pcm_frames);
    return 0;
}

METHOD(mms_OpenMoniker, 4) { return E_NOTIMPL; }
METHOD(mms_Render, 2) { return E_NOTIMPL; }

/* ------------------------------------------------------------------ video stream */
METHOD(vs_QueryInterface, 3)
{
    Movie *m = com_host(ARG(0));
    m->refs++;
    W32(ARG(2), ARG(0));                 /* IMediaStream / IDirectDrawMediaStream: one object */
    return 0;
}
METHOD(vs_AddRef, 1) { return ++((Movie *)com_host(ARG(0)))->refs; }
METHOD(vs_Release, 1) { Movie *m = com_host(ARG(0)); int r = --m->refs; if (r <= 0) movie_free(m); return r; }
METHOD(vs_GetMultiMediaStream, 2) { Movie *m = com_host(ARG(0)); m->refs++; W32(ARG(1), m->obj); return 0; }
METHOD(vs_GetInformation, 3) { if (ARG(2)) W32(ARG(2), 0); return 0; }
METHOD(vs_SetSameFormat, 3) { return E_NOTIMPL; }
METHOD(vs_AllocateSample, 3) { return E_NOTIMPL; }
METHOD(vs_CreateSharedSample, 4) { return E_NOTIMPL; }
METHOD(vs_SendEndOfStream, 2) { return 0; }
METHOD(vs_GetFormat, 5) { return E_NOTIMPL; }
METHOD(vs_SetFormat, 3) { return E_NOTIMPL; }
METHOD(vs_GetDirectDraw, 2) { W32(ARG(1), ((Movie *)com_host(ARG(0)))->dd); return 0; }
METHOD(vs_SetDirectDraw, 2) { ((Movie *)com_host(ARG(0)))->dd = ARG(1); return 0; }
METHOD(vs_GetTimePerFrame, 2)
{
    Movie *m = com_host(ARG(0));
    W64(ARG(1), (uint64_t)m->fps_den * 10);
    return 0;
}

/* The sample's surface: made through the program's own IDirectDraw (CreateSurface, vtable 6),
   off-screen in system memory, in the display's pixel format. */
static uint32_t make_surface(CPU *c, Movie *m)
{
    uint32_t desc = galloc(108), out = galloc(4);
    memset(gptr(desc), 0, 108);
    W32(desc, 108);
    W32(desc + 4, 1 | 2 | 4);            /* DDSD_CAPS | HEIGHT | WIDTH */
    W32(desc + 8, m->h);
    W32(desc + 12, m->w);
    W32(desc + 104, 0x40 | 0x800);       /* OFFSCREENPLAIN | SYSTEMMEMORY */
    uint32_t hr = guest_call(c, R32(R32(m->dd) + 6 * 4), 4, m->dd, desc, out, 0);
    uint32_t s = hr ? 0 : R32(out);
    if (s) {
        /* learn the pixel format with one Lock (vtable 25) / Unlock (vtable 32) */
        memset(gptr(desc), 0, 108);
        W32(desc, 108);
        if (!guest_call(c, R32(R32(s) + 25 * 4), 5, s, 0, desc, 1 /* DDLOCK_WAIT */, 0)) {
            uint32_t bits = R32(desc + 72 + 12), rmask = R32(desc + 72 + 16);
            m->bpp = (int)bits / 8;
            snprintf(m->pixfmt, sizeof m->pixfmt, "%s",
                     bits == 32 ? "bgra" : rmask == 0x7c00 ? "rgb555le" : "rgb565le");
            guest_call(c, R32(R32(s) + 32 * 4), 2, s, 0);
        } else {
            s = 0;
        }
    }
    gfree(desc);
    gfree(out);
    LOG(LOG_DDRAW, "video: sample surface %08x (%dx%d, %s)", s, m->w, m->h, m->pixfmt);
    return s;
}

METHOD(vs_CreateSample, 5)
{
    Movie *m = com_host(ARG(0));
    if (ARG(1)) {
        LOG(LOG_ALWAYS, "video: CreateSample with the program's own surface is not supported");
        return E_NOTIMPL;
    }
    if (!m->dd || !(m->surf = make_surface(c, m)))
        return E_FAIL;
    m->sample = com_new(&smp_cls, m);
    m->refs++;
    W32(ARG(4), m->sample);
    return 0;
}

/* ------------------------------------------------------------------ stream sample */
METHOD(smp_QueryInterface, 3) { Movie *m = com_host(ARG(0)); m->refs++; W32(ARG(2), ARG(0)); return 0; }
METHOD(smp_AddRef, 1) { return ++((Movie *)com_host(ARG(0)))->refs; }
METHOD(smp_Release, 1) { Movie *m = com_host(ARG(0)); int r = --m->refs; if (r <= 0) movie_free(m); return r; }
METHOD(smp_GetMediaStream, 2) { Movie *m = com_host(ARG(0)); m->refs++; W32(ARG(1), m->vstream); return 0; }
METHOD(smp_GetSampleTimes, 4) { return E_NOTIMPL; }
METHOD(smp_SetSampleTimes, 3) { return E_NOTIMPL; }
METHOD(smp_CompletionStatus, 3) { return 0; }

METHOD(smp_GetSurface, 3)
{
    Movie *m = com_host(ARG(0));
    if (ARG(1)) {
        W32(ARG(1), m->surf);
        guest_call(c, R32(R32(m->surf) + 4), 1, m->surf);      /* AddRef */
    }
    if (ARG(2)) {
        W32(ARG(2), 0); W32(ARG(2) + 4, 0); W32(ARG(2) + 8, m->w); W32(ARG(2) + 12, m->h);
    }
    return 0;
}

METHOD(smp_SetRect, 2) { return E_NOTIMPL; }

/* Update: wait for the next frame's time, decode it into the surface. */
METHOD(smp_Update, 5)
{
    Movie *m = com_host(ARG(0));
    if (m->eof)
        return MS_S_ENDOFSTREAM;
    if (!m->vpid) {
        char size[32];
        snprintf(size, sizeof size, "%dx%d", m->w, m->h);
        char *vargs[] = {"ffmpeg", "-v", "error", "-i", m->path, "-an", "-f", "rawvideo",
                         "-pix_fmt", m->pixfmt, "-s", size, "-", NULL};
        m->vfd = spawn(vargs, &m->vpid);
        if (m->vfd < 0) {
            m->vpid = 0;
            m->eof = 1;
            return MS_S_ENDOFSTREAM;
        }
        m->frame = malloc((size_t)m->w * m->h * m->bpp);
    }
    double due = m->start_ms + (double)m->frames_shown * 1000.0 * m->fps_den / m->fps_num;
    double now = now_ms();
    if (due > now) {
        struct timespec ts = {0, (long)((due - now) * 1e6)};
        nanosleep(&ts, NULL);
    }
    /* catch up if we fell behind: skip frames, keep the soundtrack in time */
    long want = (long)((now_ms() - m->start_ms) * m->fps_num / (1000.0 * m->fps_den));
    if (want < m->frames_shown) want = m->frames_shown;
    size_t fb = (size_t)m->w * m->h * m->bpp;
    do {
        if (!read_full(m->vfd, m->frame, fb)) {
            m->eof = 1;
            return MS_S_ENDOFSTREAM;
        }
        m->frames_shown++;
    } while (m->frames_shown <= want);
    uint32_t desc = galloc(108);
    W32(desc, 108);
    if (!guest_call(c, R32(R32(m->surf) + 25 * 4), 5, m->surf, 0, desc, 1, 0)) {
        uint32_t px = R32(desc + 36);
        int32_t pitch = (int32_t)R32(desc + 16);
        for (int y = 0; y < m->h; y++)
            memcpy(gptr(px + y * pitch), m->frame + (size_t)y * m->w * m->bpp, (size_t)m->w * m->bpp);
        guest_call(c, R32(R32(m->surf) + 32 * 4), 2, m->surf, 0);
    }
    gfree(desc);
    return 0;
}

/* ------------------------------------------------------------------ creation (ole32) */
uint32_t video_cocreate(uint32_t clsid, uint32_t iid, uint32_t ppv)
{
    if (!guid_eq(clsid, 0x49c47ce5u, 0x9ba4, 0x11d0, CLSID_AMMMS_4))
        return 0x80040154u;
    if (ppv) W32(ppv, 0);
    int want = (!headless || getenv("INSANE_VIDEOS")) && option_bool("videos", 1);   /* INSANE_VIDEOS=1: also headless */
    if (!want || deterministic || !ffmpeg_ok()) {
        LOG(LOG_ALWAYS, "video: intro videos skipped (%s)", !option_bool("videos", 1) ? "turned off in the options"
            : !want || deterministic ? "no window" : "ffmpeg not found");
        return 0x80040154u;
    }
    Movie *m = calloc(1, sizeof *m);
    m->refs = 1;
    m->voice = -1;
    m->obj = com_new(&mms_cls, m);
    movie = m;
    W32(ppv, m->obj);
    (void)iid;
    return 0;
}

void video_init(void)
{
    static void (*const mm[])(CPU *) = {
        mms_QueryInterface, mms_AddRef, mms_Release, mms_GetInformation, mms_GetMediaStream,
        mms_EnumMediaStreams, mms_GetState, mms_SetState, mms_GetTime, mms_GetDuration, mms_Seek,
        mms_GetEndOfStreamEventHandle, mms_Initialize, mms_GetFilterGraph, mms_GetFilter,
        mms_AddMediaStream, mms_OpenFile, mms_OpenMoniker, mms_Render};
    static const char *const mn[] = {
        "QueryInterface", "AddRef", "Release", "GetInformation", "GetMediaStream", "EnumMediaStreams",
        "GetState", "SetState", "GetTime", "GetDuration", "Seek", "GetEndOfStreamEventHandle",
        "Initialize", "GetFilterGraph", "GetFilter", "AddMediaStream", "OpenFile", "OpenMoniker", "Render"};
    static void (*const vm[])(CPU *) = {
        vs_QueryInterface, vs_AddRef, vs_Release, vs_GetMultiMediaStream, vs_GetInformation,
        vs_SetSameFormat, vs_AllocateSample, vs_CreateSharedSample, vs_SendEndOfStream, vs_GetFormat,
        vs_SetFormat, vs_GetDirectDraw, vs_SetDirectDraw, vs_CreateSample, vs_GetTimePerFrame};
    static const char *const vn[] = {
        "QueryInterface", "AddRef", "Release", "GetMultiMediaStream", "GetInformation", "SetSameFormat",
        "AllocateSample", "CreateSharedSample", "SendEndOfStream", "GetFormat", "SetFormat",
        "GetDirectDraw", "SetDirectDraw", "CreateSample", "GetTimePerFrame"};
    static void (*const sm[])(CPU *) = {
        smp_QueryInterface, smp_AddRef, smp_Release, smp_GetMediaStream, smp_GetSampleTimes,
        smp_SetSampleTimes, smp_Update, smp_CompletionStatus, smp_GetSurface, smp_SetRect};
    static const char *const sn[] = {
        "QueryInterface", "AddRef", "Release", "GetMediaStream", "GetSampleTimes", "SetSampleTimes",
        "Update", "CompletionStatus", "GetSurface", "SetRect"};
    com_class_init(&mms_cls, "IAMMultiMediaStream", 19, mm, mn);
    com_class_init(&vs_cls, "IDirectDrawMediaStream", 15, vm, vn);
    com_class_init(&smp_cls, "IDirectDrawStreamSample", 10, sm, sn);
    (void)MSPID_4; (void)IID_DDMS_4;
}
