/* The letters' font (font.c): the Enhanced option "font". */
#pragma once
#include <stdint.h>

void font_init(void);
void font_options(void);               /* the Enhanced page changed the option (applies at once) */
void font_frame(void);                 /* once a frame (game thread): marks the game's letter picture */
/* The picture drawn from the chosen font, for the renderer: size x size alpha, then as many of its
   shade (the letters' grey, darker at their edges as the game's), and a number that changes when it
   does; NULL with the game's own picture (font = original). */
const uint8_t *font_atlas(int *size, uint32_t *gen);
