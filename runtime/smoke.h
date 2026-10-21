/* Volumetric smoke and dust (smoke.c): the Enhanced option "smoke". */
#pragma once

void smoke_init(void);                /* after shadows_init (its hooks run inside ours), before taa_init */
void smoke_options(void);             /* the Enhanced page changed the option (applies at once) */
int smoke_available(void);            /* the GPU draws the 3D world (not the software renderer) */
