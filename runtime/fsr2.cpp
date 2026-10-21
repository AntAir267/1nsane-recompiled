/* FSR 2 for vk.c's scene (fsr2.h): AMD's FidelityFX Super Resolution 2.2.1 API and Vulkan backend
   (third_party/FidelityFX-FSR2, MIT licence), with the shaders tools/fsr2_shaders.py compiled. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsr2.h"
#include "ffx_fsr2.h"
#include "vk/ffx_fsr2_vk.h"

extern "C" void logmsg(unsigned cat, const char *fmt, ...);
#define LOG_ALWAYS_MASK 0x40000000u   /* runtime.h LOG_ALWAYS */

static FfxFsr2Context ctx;
static void *scratch;
static int have;
static uint32_t sizes[5];

static void message(FfxFsr2MsgType type, const wchar_t *msg)
{
    char s[512];
    snprintf(s, sizeof s, "%ls", msg);
    logmsg(LOG_ALWAYS_MASK, "FSR 2 %s: %s", type == FFX_FSR2_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

extern "C" void fsr2_destroy(void)
{
    if (have)
        ffxFsr2ContextDestroy(&ctx);
    have = 0;
    free(scratch);
    scratch = NULL;
}

extern "C" int fsr2_context(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, uint32_t max_rw,
                            uint32_t max_rh, uint32_t dw, uint32_t dh, int dynamic)
{
    uint32_t want[5] = {max_rw, max_rh, dw, dh, (uint32_t)dynamic};
    if (have && !memcmp(want, sizes, sizeof sizes))
        return 1;
    fsr2_destroy();
    size_t n = ffxFsr2GetScratchMemorySizeVK(phys);
    scratch = calloc(1, n);
    if (!scratch)
        return 0;
    FfxFsr2ContextDescription d;
    memset(&d, 0, sizeof d);
    FfxErrorCode e = ffxFsr2GetInterfaceVK(&d.callbacks, scratch, n, phys, gdpa);
    if (e != FFX_OK) {
        logmsg(LOG_ALWAYS_MASK, "FSR 2: no Vulkan interface (%d)", (int)e);
        return 0;
    }
    d.device = ffxGetDeviceVK(dev);
    d.maxRenderSize.width = max_rw;
    d.maxRenderSize.height = max_rh;
    d.displaySize.width = dw;
    d.displaySize.height = dh;
    d.flags = getenv("INSANE_FSR_CHECK") ? FFX_FSR2_ENABLE_DEBUG_CHECKING : 0;
    if (getenv("INSANE_FSR_AUTOEXP"))
        d.flags |= FFX_FSR2_ENABLE_AUTO_EXPOSURE;
    if (dynamic)
        d.flags |= FFX_FSR2_ENABLE_DYNAMIC_RESOLUTION;
    d.fpMessage = message;
    e = ffxFsr2ContextCreate(&ctx, &d);
    if (e != FFX_OK) {
        logmsg(LOG_ALWAYS_MASK, "FSR 2: cannot create a context (%d)", (int)e);
        free(scratch);
        scratch = NULL;
        return 0;
    }
    memcpy(sizes, want, sizeof sizes);
    have = 1;
    logmsg(LOG_ALWAYS_MASK, "FSR 2: context for up to %ux%u -> %ux%u%s", max_rw, max_rh, dw, dh,
           dynamic ? ", dynamic resolution" : "");
    return 1;
}

static FfxResource res(const Fsr2Image *i, const wchar_t *name, FfxResourceStates state)
{
    if (!i->image || !i->view) {
        FfxResource r;
        memset(&r, 0, sizeof r);
        return r;
    }
    return ffxGetTextureResourceVK(&ctx, i->image, i->view, i->w, i->h, i->format, name, state);
}

extern "C" int fsr2_dispatch(const Fsr2Dispatch *d)
{
    if (!have)
        return 0;
    FfxFsr2DispatchDescription p;
    memset(&p, 0, sizeof p);
    p.commandList = ffxGetCommandListVK(d->cb);
    p.color = res(&d->color, L"color", FFX_RESOURCE_STATE_COMPUTE_READ);
    p.depth = res(&d->depth, L"depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    p.motionVectors = res(&d->motion, L"motion", FFX_RESOURCE_STATE_COMPUTE_READ);
    p.reactive = res(&d->reactive, L"reactive", FFX_RESOURCE_STATE_COMPUTE_READ);
    p.transparencyAndComposition = res(&d->tc, L"tc", FFX_RESOURCE_STATE_COMPUTE_READ);
    p.output = res(&d->output, L"output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    p.jitterOffset.x = d->jitter_x;
    p.jitterOffset.y = d->jitter_y;
    p.motionVectorScale.x = d->mv_scale_x;
    p.motionVectorScale.y = d->mv_scale_y;
    p.renderSize.width = d->render_w;
    p.renderSize.height = d->render_h;
    p.enableSharpening = d->sharpness >= 0.0f;
    p.sharpness = d->sharpness >= 0.0f ? d->sharpness : 0.0f;
    p.frameTimeDelta = d->frame_ms;
    p.preExposure = 1.0f;
    p.reset = d->reset != 0;
    p.cameraNear = d->cam_near;
    p.cameraFar = d->cam_far;
    p.cameraFovAngleVertical = d->fov_y;
    p.viewSpaceToMetersFactor = d->metres > 0 ? 1.0f / d->metres : 1.0f;
    FfxErrorCode e = ffxFsr2ContextDispatch(&ctx, &p);
    if (e != FFX_OK) {
        static int told;
        if (told++ < 5)
            logmsg(LOG_ALWAYS_MASK, "FSR 2: dispatch failed (%d)", (int)e);
        return 0;
    }
    return 1;
}

/* the backend's view of the memory types (built with vkGetPhysicalDeviceMemoryProperties named this): AMD's
   device-coherent and uncached types left out (they need a device feature it does not ask for; it takes the
   last type with any of the wanted properties) */
extern "C" void fsr2_memory_properties(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties *p)
{
    vkGetPhysicalDeviceMemoryProperties(phys, p);
    for (uint32_t i = 0; i < p->memoryTypeCount; i++)
        if (p->memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))
            p->memoryTypes[i].propertyFlags = 0;
}

extern "C" int fsr2_phase_count(uint32_t render_w, uint32_t display_w)
{
    return ffxFsr2GetJitterPhaseCount((int32_t)render_w, (int32_t)display_w);
}
