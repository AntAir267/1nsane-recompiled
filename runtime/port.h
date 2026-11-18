/* port.h: what 1NSANE tells the AntiStatic runtime about its game (third_party/antistatic,
   runtime/win32/port_example.h lists every setting). Values as 1NSANE's own runtime answered
   before the move (master 13812ba), so the game sees the same machine. */
#pragma once

#define PORT_TITLE "1NSANE"
#define PORT_SLUG "1nsane-native"
/* INSANE_KEYS, INSANE_DUMP_FRAMES, ...: every test script uses these names */
#define PORT_ENV "INSANE"

/* the game folder as the program sees it, and its program (command line "C:\1NSANE\Game.exe") */
#define PORT_VROOT "C:\\1NSANE"
#define PORT_START_DIR ""
#define PORT_EXE "Game.exe"
#define PORT_CMDLINE_ARGS ""
#define PORT_VOLUME_LABEL "1NSANE"

/* program memory above Game.exe's image and its moved tables (builds/gog_1_0.py TABLES) */
#define PORT_ARENA_START 0x02400000u

/* GetComputerNameA (1NSANE's runtime answered "INSANE") */
#define PORT_COMPUTER_NAME "INSANE"

/* The D3D7 renderer: the game's fog drawn (1NSANE always drew it), the game's own resolution
   unless options.txt says otherwise. Off-Road's PORT_D3D7_* features stay off. */
#define PORT_DEFAULT_FOG 1
#define PORT_DEFAULT_RESOLUTION "original"

/* --test: --hash covers the threads' stacks too (1NSANE's baselines and lockstep have them), and
   an empty PeekMessage reads the virtual clock only when the thread has a timer (1NSANE's runtime
   read nothing there; reading moves the clock and the race picks other opponents) */
#define PORT_HASH_STACKS 1
#define PORT_POLL_CLOCK_TIMERS_ONLY 1
/* one merged joystick, made with the first controller and gone with the last: every controller
   drives it (the game races with the one joystick it was set up with, so one joystick per
   controller let only the first steer), and a session without a controller has none, as shipped
   (keyboard runs; padgame.c hands a late one to the game's own joystick set-up, claim_joysticks) */
#define PORT_PAD_JOYS -1
/* the fan cars and maps are mods.c's (port_file_extra, port_find_extra in game.c), with the
   option "mods": not the toolkit's <user>/mods overlays (the launcher unpacks the fan pack there) */
#define PORT_NO_BUILTIN_MODS 1
