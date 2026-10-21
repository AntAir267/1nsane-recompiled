/* FSR 2 (AMD FidelityFX Super Resolution 2.2.1, MIT licence: third_party/README.md): temporal
   anti-aliasing and upscaling of vk.c's scene. fsr2.cpp wraps its C++ API and Vulkan backend
   (third_party/FidelityFX-FSR2, built only where that source is; vk.c says FSR is unavailable
   otherwise). The same inputs would serve other temporal upscalers (DLSS). */
#pragma once
#include <vulkan/vulkan.h>

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;              /* (depth: any depth format; colour: what FSR is told) */
    uint32_t w, h;
} Fsr2Image;

typedef struct {
    VkCommandBuffer cb;
    /* render size, in SHADER_READ_ONLY_OPTIMAL: the colour (jittered), depth, motion vectors
       (previous minus current position; scaled by mv_scale to pixels), the reactive and the
       transparency-and-composition masks (views of one image may serve; view 0: none); the output
       at the display size, in GENERAL (storage) */
    Fsr2Image color, depth, motion, reactive, tc, output;
    uint32_t render_w, render_h;
    float jitter_x, jitter_y;     /* render pixels: the scene is drawn moved by this */
    float mv_scale_x, mv_scale_y;
    float frame_ms;
    float sharpness;              /* RCAS 0..1; below 0 none */
    float cam_near, cam_far, fov_y;    /* game units, radians (vertical) */
    float metres;                 /* game units per metre */
    int reset;                    /* the camera jumped: no history */
} Fsr2Dispatch;

#ifdef __cplusplus
extern "C" {
#endif
/* a context for these sizes (made again when they change; the GPU must be idle then); 0 if FSR
   cannot run. dynamic: the render size changes from frame to frame (up to max_rw x max_rh) */
int fsr2_context(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, uint32_t max_rw, uint32_t max_rh,
                 uint32_t dw, uint32_t dh, int dynamic);
void fsr2_destroy(void);          /* (the GPU idle) */
int fsr2_dispatch(const Fsr2Dispatch *d);   /* records FSR's passes into d->cb */
int fsr2_phase_count(uint32_t render_w, uint32_t display_w);
#ifdef __cplusplus
}
#endif
