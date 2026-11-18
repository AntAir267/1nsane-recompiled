/* Game controllers in 1NSANE (padgame.c): the ready-made "XBOX CONTROLLER" layout, choosing it when
   a controller is connected, menu navigation; the toolkit's pad.c (pad.h) makes every controller
   the joystick the game sees. */
#pragma once
#include <stdint.h>

void padgame_init(void);    /* after the controllers present at start are attached */
void padgame_exit(void);    /* at exit, before the settings are saved */
/* per frame (from pad_frame, through port_pad_frame); INSANE_PAD_LOG=1 logs the game's control
   values each frame */
void padgame_frame(unsigned long frame);
/* the code of layout button b (b >= 0) or GameKeys entry k on the controller (pad 1) or the
   keyboard (pad 0): with a controller connected each device keeps its own bindings in races */
uint32_t padgame_binding(int b, int k, int pad);

/* prompts.c: button prompts in the game's texts and control hints in races */
void prompts_init(void);
