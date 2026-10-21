# 1NSANE: Recompiled

An unofficial native PC version of **1NSANE** (Invictus / Codemasters, 2000), the off-road
racer, for **Linux** and **Windows**, made by static recompilation: the game's own Game.exe is
translated into C on your computer and built against a new stand-in for Windows and DirectX 7
on SDL3 and Vulkan. The game plays as it did; everything new is an option, and with every
option off it runs exactly as shipped.

**Bring your own game.** This repository holds only the translator and the runtime, never any of
the game's code or data. You need your own copy of the **GOG release** of 1NSANE (Game.exe and
its data are checked against the GOG files before anything is built).

## What's new

All optional, from the game's own menus (Options > Video Options > More Video Options, and
Options > Enhanced Options; the ones that can change mid-race are in the pause menu too):

- Any resolution, widescreen at the screen's own shape, window or full screen, VSYNC, FRAME LIMIT
- Draw distance up to 8 times the original (AUTO: as far as the camera can see), distance fade
- Ray-traced shadows, ambient occlusion and reflections; volumetric smoke and dust
- Headlights at night that light the road, with shadows, glowing lamps and a VOLUMETRIC option
- Anti-aliasing: 4X supersampling, AMD FSR 2, NVIDIA DLSS (Windows, RTX cards), or TAA, our own
  light temporal upscaler; RENDER SCALE with AUTO; COLOUR PRECISION HIGH
- Up to 15 opponents (16 cars), with points for every place in championships
- Car classes reorganized, a STOCK CARS list, START CAMERA
- Fan cars and maps loaded straight from their downloads (FAN CARS & MAPS), with fixes for the
  common problems in them
- Keyboard and controller together, hot-plugging, rumble, controller button prompts and control
  hints
- Sharp text: FONT draws the game's letters from 1nsaneFont, a font made to match them, at any
  resolution
- Skip the intro videos with any controller button, or turn them off
- The game's cheats as switches (Enhanced Options > Cheats)

## Getting it

- **Players:** a download with everything needed (the translator, a compiler and the runtime, no
  game) is on its way: it asks for your GOG copy, builds the game once, and plays it.
- **Developers:** see [BUILDING.md](BUILDING.md).

Fan cars and maps are kept in a separate repository; the download offers to fetch them.

## Layout

- `tools/` the translator (Python + Capstone): pe.py, x86.py, discover.py, codegen.py, recomp.py;
  fsr2_shaders.py compiles FSR 2's shaders for the build
- `builds/` what is known about each supported Game.exe (addresses only) and the GOG files' hashes
- `runtime/` the Windows/DirectX stand-in the translated code runs on
- `assets/` the controller icons, 1nsaneFont and the car class table
- `third_party/` SDL3, FSR 2 and the DLSS SDK, fetched and built locally (see its README.md;
  never committed)
- `gen/`, `build/` the generated C and the build (from your Game.exe; never committed)

## Credits and licence

See [CREDITS.md](CREDITS.md). This project's own code is released into the public domain
([LICENSE](LICENSE)); the libraries and assets it uses keep their own licences.

1NSANE is a trademark of its owners. This project is not made, endorsed or supported by
Invictus Games, Codemasters or the game's current owners.
