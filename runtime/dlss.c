/* DLSS Super Resolution for vk.c's temporal scene (dlss.h): NVIDIA's NGX (third_party/DLSS), built
   only where that SDK is (HAVE_DLSS). The scene's images, jitter and motion vectors are FSR 2's
   (fsr2.h): colour 8-bit (not HDR), motion vectors at the render size and without the jitter
   (previous minus current, scaled to pixels), depth as cleared to 1 (not inverted); the reactive
   mask is DLSS's bias-current-colour mask. RENDER SCALE NATIVE is DLAA. INSANE_DLSS_LOG=1 lets
   NGX log (into the log file, and its own files in <user>/ngx), INSANE_DLSS_JITTER=-1 hands
   DLSS the jitter negated (tests). */
#ifdef HAVE_DLSS
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "dlss.h"
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers.h"          /* (NGX_DLSS_GET_OPTIMAL_SETTINGS) */

void logmsg(unsigned cat, const char *fmt, ...);
#define LOG_ALWAYS_MASK 0x40000000u   /* runtime.h LOG_ALWAYS */

/* the project's own identifier (NGX asks custom engines for a GUID-like one) */
#define PROJECT_ID "6f1d2c3a-8b4e-4f5a-9c7d-1a2b3c4d5e6f"

static VkDevice ngx_dev;
static NVSDK_NGX_Parameter *params;
static NVSDK_NGX_Handle *feature;
static uint32_t sizes[5];
static float jitter_sign = 1.0f;

static void NVSDK_CONV ngx_log(const char *msg, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature from)
{
    (void)level; (void)from;
    char s[512];
    snprintf(s, sizeof s, "%s", msg);
    size_t k = strlen(s);
    while (k && (s[k - 1] == '\n' || s[k - 1] == '\r'))
        s[--k] = 0;
    logmsg(LOG_ALWAYS_MASK, "NGX: %s", s);
}

int dlss_device_extensions(VkPhysicalDevice phys, const char **out, int max, int *needs_bda)
{
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys, &p);
    *needs_bda = 0;
    if (p.vendorID != 0x10de)                       /* NVIDIA */
        return -1;
    unsigned ni = 0, nd = 0;
    const char **inst = NULL, **devx = NULL;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &inst, &nd, &devx)))
        return -1;
    uint32_t na = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &na, NULL);
    VkExtensionProperties *have = calloc(na ? na : 1, sizeof *have);
    vkEnumerateDeviceExtensionProperties(phys, NULL, &na, have);
    int n = 0;
    for (unsigned i = 0; i < nd; i++) {
        if (!strcmp(devx[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
            *needs_bda = 1;                         /* Vulkan 1.2's feature instead */
            continue;
        }
        int found = 0;
        for (uint32_t k = 0; k < na && !found; k++)
            found = !strcmp(have[k].extensionName, devx[i]);
        if (!found || n >= max) {
            logmsg(LOG_ALWAYS_MASK, "DLSS: the device lacks %s", devx[i]);
            free(have);
            return -1;
        }
        out[n++] = devx[i];
    }
    free(have);
    return n;
}

int dlss_init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, const char *user_dir)
{
    const char *e = getenv("INSANE_DLSS_JITTER");
    if (e && atof(e) < 0)
        jitter_sign = -1.0f;
    /* NGX's files (logs) in <user>/ngx; nvngx_dlss next to the program */
    static wchar_t data[1100], exe_dir[1100];
    swprintf(data, sizeof data / sizeof *data, L"%hs/ngx", user_dir);
#ifdef _WIN32
    CreateDirectoryW(data, NULL);
    GetModuleFileNameW(NULL, exe_dir, sizeof exe_dir / sizeof *exe_dir);
    wchar_t *slash = wcsrchr(exe_dir, L'\\');
#else
    char p[1024];
    ssize_t k = readlink("/proc/self/exe", p, sizeof p - 1);
    p[k > 0 ? k : 0] = 0;
    swprintf(exe_dir, sizeof exe_dir / sizeof *exe_dir, L"%hs", p);
    wchar_t *slash = wcsrchr(exe_dir, L'/');
#endif
    if (slash) *slash = 0;
    static const wchar_t *paths[1];
    paths[0] = exe_dir;
    NVSDK_NGX_FeatureCommonInfo info;
    memset(&info, 0, sizeof info);
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    int verbose = getenv("INSANE_DLSS_LOG") != NULL;
    info.LoggingInfo.LoggingCallback = verbose ? ngx_log : NULL;
    info.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data, inst,
                                                              phys, dev, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
                                                              &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        logmsg(LOG_ALWAYS_MASK, "DLSS: NGX cannot start (0x%08x)", (unsigned)r);
        return 0;
    }
    ngx_dev = dev;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&params))) {
        logmsg(LOG_ALWAYS_MASK, "DLSS: no capability parameters");
        dlss_shutdown(dev);
        return 0;
    }
    int avail = 0, old_driver = 0, init_result = 0;
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &old_driver);
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
    if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &avail)) || !avail) {
        logmsg(LOG_ALWAYS_MASK, "DLSS: not available here (%s; result 0x%08x)",
               old_driver ? "the driver is too old" : "no RTX GPU, or nvngx_dlss is not next to the program",
               (unsigned)init_result);
        dlss_shutdown(dev);
        return 0;
    }
    return 1;
}

void dlss_destroy(void)
{
    if (feature)
        NVSDK_NGX_VULKAN_ReleaseFeature(feature);
    feature = NULL;
    memset(sizes, 0, sizeof sizes);
}

void dlss_shutdown(VkDevice dev)
{
    dlss_destroy();
    if (params)
        NVSDK_NGX_VULKAN_DestroyParameters(params);
    params = NULL;
    if (ngx_dev)
        NVSDK_NGX_VULKAN_Shutdown1(dev);
    ngx_dev = VK_NULL_HANDLE;
}

int dlss_context(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, float mode)
{
    uint32_t mb;
    memcpy(&mb, &mode, 4);
    uint32_t want[5] = {rw, rh, dw, dh, mb};
    if (feature && !memcmp(want, sizes, sizeof sizes))
        return 1;
    if (!params)
        return 0;
    dlss_destroy();
    /* the mode nearest the render scale (each takes sizes around its own) */
    float ratio = mode > 0 ? mode : (float)dw / (float)rw;
    NVSDK_NGX_PerfQuality_Value q = ratio < 1.2f ? NVSDK_NGX_PerfQuality_Value_DLAA
                                  : ratio < 1.6f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                  : ratio < 1.85f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                  : NVSDK_NGX_PerfQuality_Value_MaxPerf;
    NVSDK_NGX_DLSS_Create_Params cp;
    memset(&cp, 0, sizeof cp);
    cp.Feature.InWidth = rw;
    cp.Feature.InHeight = rh;
    cp.Feature.InTargetWidth = dw;
    cp.Feature.InTargetHeight = dh;
    cp.Feature.InPerfQualityValue = q;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(ngx_dev, cb, 1, 1, &feature, params, &cp);
    if (NVSDK_NGX_FAILED(r)) {
        logmsg(LOG_ALWAYS_MASK, "DLSS: cannot create the feature for %ux%u -> %ux%u (0x%08x)", rw, rh, dw, dh, (unsigned)r);
        feature = NULL;
        return 0;
    }
    memcpy(sizes, want, sizeof sizes);
    logmsg(LOG_ALWAYS_MASK, "DLSS: %s, %ux%u -> %ux%u", q == NVSDK_NGX_PerfQuality_Value_DLAA ? "DLAA" :
           q == NVSDK_NGX_PerfQuality_Value_MaxQuality ? "QUALITY" : q == NVSDK_NGX_PerfQuality_Value_Balanced ? "BALANCED"
           : "PERFORMANCE", rw, rh, dw, dh);
    return 1;
}

/* the render sizes DLSS takes for display dw x dh in each mode, as display / render ratios: lo[m] the
   largest size's, hi[m] the smallest's (m: DLAA, QUALITY, BALANCED, PERFORMANCE; 0 where unknown) */
void dlss_ranges(uint32_t dw, uint32_t dh, float lo[4], float hi[4])
{
    static const NVSDK_NGX_PerfQuality_Value q[4] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
                                                     NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf};
    for (int m = 0; m < 4; m++) {
        unsigned ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0;
        float sharp = 0;
        lo[m] = hi[m] = 0;
        if (params && !NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(params, dw, dh, q[m], &ow, &oh, &xw, &xh, &nw, &nh,
                                                                       &sharp)) && xh && nh) {
            lo[m] = (float)dh / (float)xh;
            hi[m] = (float)dh / (float)nh;
        }
    }
}

static NVSDK_NGX_Resource_VK res(const Fsr2Image *i, VkImageAspectFlags aspect, bool rw)
{
    VkImageSubresourceRange r = {aspect, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(i->view, i->image, r, i->format, i->w, i->h, rw);
}

int dlss_dispatch(const Fsr2Dispatch *d)
{
    if (!feature)
        return 0;
    NVSDK_NGX_Resource_VK color = res(&d->color, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK depth = res(&d->depth, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NVSDK_NGX_Resource_VK motion = res(&d->motion, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK bias = res(&d->reactive, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NVSDK_NGX_Resource_VK out = res(&d->output, VK_IMAGE_ASPECT_COLOR_BIT, true);
    NVSDK_NGX_VK_DLSS_Eval_Params e;
    memset(&e, 0, sizeof e);
    e.Feature.pInColor = &color;
    e.Feature.pInOutput = &out;
    e.pInDepth = &depth;
    e.pInMotionVectors = &motion;
    e.pInBiasCurrentColorMask = d->reactive.view ? &bias : NULL;
    e.InJitterOffsetX = jitter_sign * d->jitter_x;
    e.InJitterOffsetY = jitter_sign * d->jitter_y;
    e.InRenderSubrectDimensions.Width = d->render_w;
    e.InRenderSubrectDimensions.Height = d->render_h;
    e.InReset = d->reset != 0;
    e.InMVScaleX = d->mv_scale_x;
    e.InMVScaleY = d->mv_scale_y;
    e.InPreExposure = 1.0f;
    e.InExposureScale = 1.0f;
    e.InFrameTimeDeltaInMsec = d->frame_ms;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(d->cb, feature, params, &e);
    if (NVSDK_NGX_FAILED(r)) {
        static int told;
        if (told++ < 5)
            logmsg(LOG_ALWAYS_MASK, "DLSS: evaluation failed (0x%08x)", (unsigned)r);
        return 0;
    }
    return 1;
}
#endif
