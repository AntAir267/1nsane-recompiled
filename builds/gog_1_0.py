"""Per-build knowledge for the GOG release of 1NSANE (Game.exe dated 2001-04-20, patch 2).

Addresses only: no code or data from the game lives in this repository.
"""
NAME = 'gog-1.0'
SHA256 = '441d506092289f48ac330238ec98b4b9edb408f65baaf26fd9ab1ccf88b9df34'

# Functions reached only in ways discovery cannot see (found at run time, added here).
EXTRA_ENTRIES = []

# Addresses that look like code pointers but are not function entries.
NOT_ENTRIES = []

# Jump tables discovery cannot bound: jmp address -> (table address, entries)
JUMP_TABLES = {
    0x4b80f4: (0x4b81e8, 4),    # CRT memcpy: byte tail, index -4..-1 from 0x4b81f8
    0x4babd4: (0x4bacc8, 4),    # CRT memmove: same
}

# Jumps into the middle of another function (exception handling, longjmp).
NONLOCAL_JUMPS = [
    0x4b422c,   # C++ EH: continue after a catch block
    0x4b445f,   # SEH: __except handler
    0x4b5081,   # longjmp
]

# Imports that never return.
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'TerminateProcess']

# Fixed-size tables the translator can move to bigger memory, for many mods at once (design
# point 8). Constants inside a table's range in `funcs` (every function if no list is given)
# become its run-time base; the immediates at `limits` equal to `count` become its run-time
# capacity. Unmoved by default.
TABLES = [
    dict(name='idf_archives', base=0x1b1a790, entry=100, count=512,      # mounted .idf names
         funcs=[0x44bf10, 0x44c110], limits=[0x44bf1c]),
    dict(name='idf_entries', base=0x1b26f90, entry=64, count=6000,       # their directory entries
         funcs=[0x44bf10, 0x44c110], limits=[0x44c010, 0x44c018]),
    # the car and map lists, filled at start-up (0x47d8a0) from data\vehicles.cfg and
    # data\scenes.cfg (at most 200 lines each) and then from the Data\Vehicles and Data\Maps
    # folder listings (at most 200 names each)
    dict(name='cars', base=0x1c0e5d8, entry=64, count=200, limits=[0x47ddd1, 0x47de8a]),
    dict(name='maps', base=0x1c457f0, entry=0x60, count=200, limits=[0x47e5a7, 0x47e664]),
    # the cars a network game offers (0x46a3a0: all, or one class; file name, car number)
    dict(name='net_cars', base=0x1ba6fe0, entry=20, count=3200),
    # the car classes (16 x 0x8c at 0x1c0dd18: group letter, count, 32 car numbers, total
    # paint jobs): only the base of the car-number lists moves, so a long class runs into the
    # next class's list instead of its count (mods.c keeps the one list the game reads correct)
    dict(name='class_lists', base=0x1c0dd20, entry=4, count=1),
    # more AI opponents (runtime/opponents.c): the caches of loaded textures, models and the
    # models' textures, which 16 different cars fill; the HUD's opponent table reading a
    # compact copy of the participants it shows
    dict(name='textures', base=0x1af6800, entry=0x4c, count=128, limits=[0x4476f6, 0x448333, 0x448863],
         funcs=[0x447150, 0x4476c0, 0x4487b0, 0x4491a0, 0x4491e0, 0x449dc0]),
    dict(name='models', base=0x1af8e48, entry=0x90, count=500, limits=[0x4498b2, 0x449aa2],
         funcs=[0x449840, 0x449a30, 0x449cf0, 0x449dc0]),
    dict(name='model_textures', base=0x1b0a788, entry=0x4c, count=800, limits=[0x449439],
         funcs=[0x4493e0, 0x449c20, 0x449dc0]),
    dict(name='hud_racers', base=0x1c4d8d0, entry=0x4d8, count=16, funcs=[0x4621c0]),
    # ... and the 16 physics vehicles (0x494240 loads one into a free record; the flag modes'
    # flag is one too) and the list of those in use
    dict(name='vehicles', base=0x2200118, entry=0x1bec, count=16,
         funcs=[0x465b60, 0x468ed0, 0x4692e0, 0x469780, 0x494240, 0x49b0d0, 0x49b3e0]),
    dict(name='vehicle_list', base=0x2200074, entry=4, count=16,
         funcs=[0x465b60, 0x468ed0, 0x4692e0, 0x469780, 0x494240, 0x49b0d0, 0x49b3e0]),
    # car models bigger than the game's scratch buffers (runtime/fancars.c): a mesh's vertices
    # (the car shadow, the meshes that follow the physics body), the shadow's places on its
    # texture and its triangles; moved for one call when a car's model would not fit
    dict(name='model_verts', base=0x21f9ab8, entry=12, count=2048,
         funcs=[0x41a6b0, 0x49bef0, 0x49c180, 0x49c270, 0x49c370]),
    dict(name='shadow_points', base=0xa1b1c0, entry=8, count=2048, funcs=[0x41a6b0]),
    dict(name='shadow_tris', base=0xa0ad68, entry=0x1c, count=2048, funcs=[0x41a6b0]),
]

# Functions the runtime may hook (Enhanced options, menus, mods): hook_<name> is a function
# pointer, NULL unless set, run at the function's entry. It may adjust the arguments and return
# 0 (the function then runs), or run the original itself (F_<addr>_orig) and/or do the call's
# work and return 1. Translated builds only: the reference build runs the original code.
HOOKS = {
    0x42a350: 'perspective',    # builds a camera's projection from (..., near, far, fov, aspect)
    0x44add0: 'input_poll',     # reads keyboard, mouse and joysticks into the pressed-codes array
    0x44c110: 'idf_open',       # opens a game file (name, &size): archive members first, then loose files
    0x44d660: 'menu_input',     # builds the menus' up/down/left/right/enter/escape state
    0x44db70: 'layout_load',    # loads controller layout N (Data\N.ctl) at start-up; 1 = found
    0x44dc00: 'layout_save',    # saves controller layout N to Data\N.ctl
    0x477020: 'define_axis',    # the menus' "press/move the control for ..." for an axis entry
    0x441cb0: 'control_axis',   # one axis of the layout in use -> its control value (a race's controls)
    0x44ddc0: 'camera_set',     # (camera, player): the race view's camera (start camera, camera.c)
    0x47cff0: 'menu_vars',      # links the menu variable table into a menu being loaded
    0x46cd00: 'game_start',     # a game starts: resets the wheel-scale cheat among others
    0x4661c0: 'race_frame',     # draws a race frame: the world, name labels, HUD, pause menu
    0x462c10: 'race_dashboard', # the speedometer cluster (bottom right)
    0x463bd0: 'race_hud',       # minimap, opponent table, checkpoint panel, lap counter
    0x4759b0: 'menu_draw',      # draws a menu's objects
    0x474cc0: 'menu_anim',      # moves a menu's objects in/out (and draws them)
    # mods (mods.c): start-up, which lists the car and map folders into its stack frame; the
    # championship file, read once the lists are complete and before the car classes are
    # built; the championship's vehicle choice, which reads the championship class's car list
    0x47d8a0: 'startup',
    0x47d280: 'champdef',
    0x479000: 'vehicle_prev',
    0x479180: 'vehicle_next',
    0x479310: 'myvehicle_prev',
    0x4794b0: 'myvehicle_next',
    0x47b200: 'champ_vehicle',
    # ... and a frame of the menus (input, then the menu's actions): jumps in the long car and
    # map lists (picker.c)
    0x4759f0: 'menu_frame',
    # button prompts (prompts.c): a text glyph's quad, drawing the queued quads, the font going
    # away, the texts looked up by name, a binding's name, an axis entry's name, a menu
    # variable's text
    0x429d80: 'text_quad',
    0x42a020: 'quads_flush',
    0x4812d0: 'font_unload',
    0x4708e0: 'text_lookup',
    0x470cb0: 'key_name',
    0x47d010: 'axis_name',
    0x470d60: 'mv_text',
    # draw distance and distance fade (drawdist.c): the world drawn round a camera (range), a
    # fog being made, the terrain's vertex buffers being made, the object box set up (range), an
    # object put in the frame's lists, the terrain drawn (range); an object mesh's piece chosen,
    # an instance added to a batch, a batch drawn (slot, -1 all, -2 none), an instance's lights
    # set before its draw (the two kinds)
    0x465950: 'world_draw',
    0x412b00: 'fog_new',
    0x401580: 'terrain_buffers',
    0x4182d0: 'object_box',
    0x4185d0: 'object_draw',    # draws one object, its model picked by distance from its type's list
    0x402a90: 'terrain_draw',
    0x409390: 'mesh_piece',
    0x40aea0: 'batch_add',
    0x40a770: 'batch_draw',
    0x429b40: 'slot_unlit',
    0x429980: 'slot_lit',
    0x41a670: 'object_shadow',  # an object's shadow worked out and drawn
    0x418d40: 'models_draw',    # the frame's object models drawn (after the terrain, before the water)
    # ray-traced shadows (shadows.c): a car's shadow painted into its texture (each race frame); the
    # frame's see-through model parts drawn, sorted by depth (lattices, glass: no depth writes)
    0x41a6b0: 'car_shadow',
    0x419280: 'see_through',
    0x463000: 'mirror_view',    # the rear-view mirror over the race view (drawn after it)
    # ... the water drawn (its colour pass, then its waves): ray-traced shadows and ambient
    # occlusion start from its surface
    0x406370: 'water_draw',
    # ... the sky dome drawn round the camera (and the sun after it): what ray-traced reflections
    # that meet nothing show
    0x41c7a0: 'sky_draw',
    # temporal anti-aliasing (taa.c): the skid marks drawn (decals on the ground, as still as it);
    # the in-car view's cockpit drawn after the world (dashboard, gauges: 0x4661c0 calls it when
    # 0x1bc71bc is set), kept in the scene so it meets the steering wheel's depth
    0x41f7e0: 'skid_draw',
    0x462550: 'cockpit_draw',
    # volumetric smoke and dust (smoke.c): a node of the see-through parts' tree drawn (0x419280's,
    # sorted by depth): a puff of smoke, dust or spray there is taken for its volume, not drawn
    0x419300: 'see_through_node',
    # more AI opponents (opponents.c): the model and texture caches being emptied, the race's
    # AI opponents being picked, a race's scene pools being set up, the HUD's opponent table,
    # the results lists (laps, Pathfinder, the other modes)
    0x449dc0: 'caches_empty',
    0x45e500: 'opponents_pick',
    0x484dd0: 'link_pool',
    0x4621c0: 'hud_table',
    0x456da0: 'results_race',
    0x4585f0: 'results_path',
    0x459de0: 'results_gates',
    # ... the drivers being read (data\drivers.cfg), the player setup's CHANGE MODEL (next, back)
    0x44ea50: 'drivers_load',
    0x47a200: 'model_next',
    0x47a250: 'model_prev',
    # ... a flat quad and a text line being drawn (the HUD opponent table's dots and names)
    0x41bbe0: 'flat_quad',
    0x481380: 'text_draw',
    # ... a championship event being set up (opponents, length, location from champdef.cfg)
    0x47b020: 'champ_event',
    # ... what is locked being worked out (stock cars only marks the fan cars after it)
    0x46e690: 'unlocks',
    # ... a row of Vehicle > Browse, a game type's saved settings being taken up
    0x4721c0: 'car_row',
    0x44ca10: 'settings_load',
    # force feedback (ifc21.c): the game's settings read from the registry at start-up
    # (ForceFeedBack among them); the player's car's steering force for the effects, per physics
    # step (with a keyboard layout and FORCE FEEDBACK on, the game's own push on the steering);
    # force feedback starting on the layout's steering joystick
    0x467d10: 'registry_load',
    0x441c30: 'ff_steer',
    0x44b460: 'ff_start',
    # fan cars the game cannot cope with as they are (fancars.c): models bigger than the game's
    # scratch buffers (the car shadow is car_shadow above): a mesh following the car's physics
    # body, read whole into the scratch vertices and written back (skinned to the body's points
    # two ways, vertex by vertex from its parts or its points)
    0x49bef0: 'mesh_skin_nodes',
    0x49c370: 'mesh_skin_points',
    0x49c180: 'mesh_parts',
    0x49c270: 'mesh_points',
    # ... and an air intake under the car (fancars.c): a car's .vhx physics points read (its
    # wheels' bottom), a car loaded into a physics record (its .vht's intake)
    0x493d70: 'vhx_load',
    0x494240: 'vehicle_load',
    # ... and parts off the body in the vehicle preview (fancars.c): a loaded car placed on the
    # ground (1: a vehicle screen's preview, 0: a race)
    0x49b750: 'vehicle_place',
    # ... and a part tied to points the car doesn't have or to points in a line (fancars.c): a
    # part's place kept relative to three physics points, as the car loads
    0x4940d0: 'attach_offset',
    # ... and the countdown's hold for a car whose handbrake pushes (fancars.c): a car's controls
    # taken for its next physics steps
    0x49f9b0: 'vehicle_controls',
}

# Literals the runtime may change: at the listed instructions, the immediate, shift count or
# address scale equal to `value` becomes rc_<name>.
CONSTS = {
    # more AI opponents (opponents.c): the link nodes 0x4854d0 hands out (400, ~33 a car); the
    # text size of the results lists' rows (1.4 x 1.5)
    'link_limit': dict(value=0x190, insns=[0x4854d0]),
    'results_row_width': dict(value=0x3fb33333, insns=[0x456f1a, 0x457128, 0x45874e, 0x459f14]),
    'results_row_height': dict(value=0x3fc00000, insns=[0x456f15, 0x457123, 0x458749, 0x459f0f]),
    # the end of the vehicle records (the free-record search, the reset of all)
    'vehicles_end': dict(value=0x221bfd8, insns=[0x4942cc]),
    'vehicles_end_field': dict(value=0x221bff8, insns=[0x49b434]),
}

# Runtime helpers done in place at their call sites (same results and memory, faster):
INLINE = {
    0x4b34f0: 'ftol',           # MSVC _ftol: float to integer, called ~850 places, ~17% of a race frame
}

# Game data the runtime reads or fills (controllers: runtime/padgame.c; car and map lists:
# runtime/mods.c): emitted as GAME_<NAME> in gen/image.h.
DATA = {
    'layout_count': 0x155cb10,      # controller layouts loaded (Data\0.ctl, 1.ctl, ...; at most 8)
    'layout': 0x155cb18,            # the layout in use: 6 axes of 28 bytes, 8 button codes (200 bytes)
    'layouts': 0x155ccc0,           # the loaded layouts, 200 bytes each
    'layout_names': 0x4c5034,       # their names, 32 bytes each
    'layout_selected': 0x155d434,   # the one SELECT CONFIG shows
    'controls': 0x155caf8,          # what the layout gives: steer, accelerate, brake, handbrake, head
                                    # turn, head rise (full = 0x7f0000)
    'game_keys': 0x4c81ec,          # 24 game-control codes (pause, camera, map, ...)
    'driving': 0x155ccb8,           # 1 while the controls drive the car, 0 in menus
    'race_state': 0x1c54d4c,        # 3 = race over, waiting for Enter for the results
    'race_mode': 0x1c54d04,         # nonzero while a race (or replay) runs, 0 in the main menus
    'menu_nav': 0x1c119ac,          # menu up, down, left, right, enter, escape: bit 0 held, bit 7 used
    'pressed': 0x1b1a3b8,           # pressed codes: 256 keys, 4 mouse buttons, 24 per joystick
    # car and map lists (mods.c)
    'car_count': 0x1bf7dac,     # cars in the car list
    'map_count': 0x1bc74d0,     # maps in the map list
    'champ': 0x1c3a120,         # the championship chosen in the menu (0-4)
    'champs': 0x1c3c4cc,        # 5 championship records of 0x7f0 bytes: car group letter, class
    # more AI opponents (opponents.c)
    'participants': 0x1c4d8d0,      # 16 records of 0x4d8 bytes (+0x380 place in the standings order)
    'participant_count': 0x1c4248c, # cars in the race
    'hud_player': 0x1c54d7c,        # the participant the HUD is about
    'hud_marked': 0x4c7f30,         # a participant the HUD's opponent table highlights
    'opponents': 0x4c7f78,          # MV_OPPONENTNUM's value: AI opponents of the next race
    'opponents_var': 0x4c91a0,      # MV_OPPONENTNUM {link, name, value, type, min, max, ...}
    'opponents_max': 0x4c8f10,      # its maximum as shipped (8; ML_CHAT's too)
    'race_settings': 0x1ba6898,     # the four game types' race settings, 0x78 bytes (+0x70 opponents)
    'grid': 0x4c8438,               # start spots: 16 x {x, y, z} in 16.16 fixed, 13 filled
    'colours': 0x4c7f38,            # 16 participant colours (ARGB), 8 filled
    'link_free': 0x1caac28,         # free list of the link nodes (0x78 bytes: +0x70 prev, +0x74 next)
    'texture_count': 0x1af67fc,     # textures cached (table TABLES textures)
    'model_count': 0x1af8e00,       # models cached (TABLES models)
    'model_texture_count': 0x1af67f8,   # model textures cached (TABLES model_textures)
    'vehicle_count': 0x21f9650,     # physics vehicles in use (TABLES vehicles, vehicle_list)
    'results_step': 0x4c8364,       # float: the results list's row spacing (0.12)
    'drivers': 0x1bd95f0,           # the drivers (data\drivers.cfg): records of 0x344 bytes, room for 32
    'driver_count': 0x1c54bb4,      # drivers loaded (8)
    'standings': 0x1bc81a0,         # the standings order: n x {score, participant}
    'game_mode': 0x4c7f20,          # 0 free roam .. 7 destruction zone
    'points': 0x1c42458,            # points[mode * 16 + cars][place] (ints; filled for 2-8 cars)
    'hud_left': 0x1bfc514,          # float: where the next HUD panel goes (the opponent table's left)
    'hud_top': 0x1c0c918,           # float: the HUD panels' top
    'font_glyphs': 0x4cdf24,        # the text font's glyphs (0x14 bytes each, +8 width)
    'font_width': 0x4cdf1c,         # float: the text's width scale
    'font_size': 0x1ca5e18,         # float: the font's size
    'text_unit': 0x4cdf0c,          # float: text width to screen units
    'race_clock': 0x1c54d50,        # the race's time
    'race_target': 0x1c54d44,       # the score that wins (gates, laps, points)
    'race_gates': 0x1c0db34,        # gates on the course
    'race_rules': 0x1c54e4c,        # the game mode's handlers (+0x14: the race is over)
    'champ_opponents': 0x1c3c800,   # each championship event's opponents: [5 x 0x7f0][column 8][scene 8]
    'car_chosen': 0x1bf394c,        # the player's car: its car list index,
    'car_paint': 0x1bf3950,         # ... its paint job,
    'car_name': 0x1bf3958,          # ... its file name
    'classes': 0x1c0dd18,           # car classes, 16 x 0x8c: letter, count, 32 cars, +0x88 paint jobs
    'class_count': 0x1bf22c8,       # car classes in use
}
