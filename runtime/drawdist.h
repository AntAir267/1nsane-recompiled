/* Draw distance (drawdist.c): a longer view in races. */
#pragma once
#include <stdint.h>

void drawdist_init(void);
void drawdist_options(void);              /* the Enhanced page changed the option */
void drawdist_camera(uint32_t args);      /* 0x42a350's arguments: the far plane */
float drawdist_factor(void);              /* the race's view distance factor in force (1 = original) */
struct Surface;
void drawdist_surface_gone(const struct Surface *s);   /* ddraw.c: a surface going (kept far meshes) */
