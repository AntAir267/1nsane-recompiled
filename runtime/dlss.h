/* DLSS (NVIDIA DLSS Super Resolution through NGX; third_party/DLSS, NVIDIA's SDK licence: built
   only where that SDK is): vk.c's temporal scene resolved by DLSS instead of FSR 2, from the same
   inputs (fsr2.h's Fsr2Dispatch). NGX's link library is built for MSVC: on Windows the program is
   linked with lld and runtime/win/msvc_shim.c supplies what it expects of Microsoft's runtime;
   only NGX's C entry points are used (its parameter object is a C++ class MSVC laid out). */
#pragma once
#include <vulkan/vulkan.h>
#include "fsr2.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Before the device is made: the device extensions DLSS needs, into out (up to max); the count,
   or -1 if DLSS cannot run on this device (not NVIDIA, or an extension is missing).
   *needs_bda: it needs bufferDeviceAddress (Vulkan 1.2's feature, enabled instead of the
   extension of the same name). */
int dlss_device_extensions(VkPhysicalDevice phys, const char **out, int max, int *needs_bda);
/* After: NGX started for the device; 1 if DLSS Super Resolution is available (an RTX GPU, a
   driver new enough, nvngx_dlss next to the program) */
int dlss_init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, const char *user_dir);
/* the feature for these sizes, recorded into cb (made again when they change; the GPU idle).
   mode: 0, the mode nearest display / render; else that ratio's mode, the render size the largest
   the frames' will be (RENDER SCALE AUTO: each evaluation's own size, up to it) */
int dlss_context(VkCommandBuffer cb, uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h,
                 float mode);
void dlss_destroy(void);          /* (the GPU idle) */
/* the render sizes DLSS takes for display dw x dh in each mode (DLAA, QUALITY, BALANCED, PERFORMANCE), as
   display / render ratios: lo the largest size's, hi the smallest's (0: unknown) */
void dlss_ranges(uint32_t dw, uint32_t dh, float lo[4], float hi[4]);
int dlss_dispatch(const Fsr2Dispatch *d);   /* records DLSS into d->cb */
void dlss_shutdown(VkDevice dev); /* (the GPU idle, before the device goes) */
#ifdef __cplusplus
}
#endif
