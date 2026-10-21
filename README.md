# 1NSANE: Recompiled

An unofficial native PC version of **1NSANE** (Invictus / Codemasters, 2000), the off-road
racer, for **Linux** and **Windows**, made by static recompilation: the game's own Game.exe is
translated into C on your computer and built against a new stand-in for Windows and DirectX 7
on SDL3 and Vulkan. The game plays as it did; everything new is an option, and with every
option off it runs exactly as shipped.

> **Alpha.** This is an early test version. It has only been tested on the author's own two
> computers (listed under [Tested on](#tested-on)); expect problems on others, and please report
> them in Issues.

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

- **Players (Linux):** download the AppImage from
  [Releases](https://github.com/AntAir267/1nsane-recompiled/releases), make it executable and open
  it. It holds everything needed but the game (the translator, a compiler and the runtime): it
  asks for your GOG copy (the GOG installer, or the folder it installed), checks it, builds the game
  on your computer once (about a minute), and plays it; after that it opens straight into the game.
  It needs 64-bit Linux from 2020 or later (glibc 2.31), a graphics driver with Vulkan 1.3, and
  about 1.5 GB free; the intro videos play when ffmpeg is installed. `tools/release/linux.sh` makes it.
- **Players (Windows):** download the zip from
  [Releases](https://github.com/AntAir267/1nsane-recompiled/releases), unzip it anywhere and open
  `1nsane-recompiled.exe` in its folder; it does the same as the Linux one. It needs 64-bit
  Windows 10 or 11, a graphics driver with Vulkan 1.3, and about 2 GB free. The program isn't
  signed, so Windows may warn about it first (More info, then Run anyway).
- **Developers:** see [BUILDING.md](BUILDING.md).

Fan cars and maps are kept in a separate repository
([1nsane-recompiled-fan-pack](https://github.com/AntAir267/1nsane-recompiled-fan-pack)); the
download offers to fetch them.

## Tested on

Only these two computers so far:

- **Linux:** GPD Win Max 2 (2025) handheld laptop: AMD Ryzen AI 9 HX 370 with Radeon 890M
  graphics, 32 GB RAM; Ubuntu 26.04.1 LTS, Linux 7.0, KDE Plasma 6.6 (Wayland), Mesa 26.2.3
  (RADV). Played at 2560x1440. The Linux AppImage has been tested here only.
- **Windows:** desktop PC: AMD Ryzen 7 5800X3D, NVIDIA GeForce RTX 4070 (driver 617.14), 4K HDR
  display; Windows 11 Pro 25H2 (build 26200). The Windows download has been tested here only.

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
