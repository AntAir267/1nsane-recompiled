/* Headlights (headlights.c): the Enhanced option "headlights". */
#pragma once
#include "vk.h"

void headlights_init(void);
void headlights_options(void);        /* the Enhanced page changed the option (applies at once) */
int headlights_on(void);              /* wanted, and some car's lamps are lit (a night map) */
/* the beams for the view now being drawn (its camera), nearest first; *cap: their summed light's ceiling,
   *air: how much of it the air scatters per metre (VOLUMETRIC; else 0), tint[3]: their colour (green 1) */
int headlights_beams(VkBeam *out, int max, float *cap, float *air, float *tint);
