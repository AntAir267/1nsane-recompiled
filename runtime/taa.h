/* Temporal anti-aliasing and upscaling (taa.c): the race's main view as vk.c's scene. The
   options themselves ("antialiasing = fsr", "render_scale") are read by vk.c (vk_options). */
#pragma once

void taa_init(void);                  /* after shadows_init: its hooks run inside ours */
