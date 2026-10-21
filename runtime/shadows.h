/* Ray-traced sun shadows and ambient occlusion (shadows.c): the Enhanced options "shadows" and
   "ambient_occlusion". */
#pragma once

void shadows_init(void);              /* after drawdist_init: its hooks run inside ours */
void shadows_options(void);           /* the Enhanced page changed the option (applies at once) */
int shadows_available(void);          /* the GPU can trace rays */
