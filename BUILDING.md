# Building from source

You need your own copy of the **GOG release** of 1NSANE. The build translates its Game.exe into
C (gen/) and compiles that with the runtime into one program.

## The game's files

The program looks for the game in its user folder, `game/` inside:

- Linux: `~/.local/share/1nsane-native/`
- Windows: `%APPDATA%\1nsane-native\`

Copy the GOG installation's files there (Game.exe and the Data folder beside it), or unpack the GOG
offline installer without running it, with innoextract:

    innoextract -d unpacked setup_1nsane_1.0_(18948).exe

and copy the files under `unpacked/app` (or wherever Game.exe ended up) into `game/`. Another place
works too: pass `GAME=<folder>` to make and `--game <folder>` to the program.

## Linux

Packages (Debian/Ubuntu names): `build-essential pkg-config python3 python3-venv
libvulkan-dev glslang-tools libfreetype-dev zlib1g-dev`, plus `libarchive13` (fan downloads in RAR
and 7-Zip) and `ffmpeg` (the intro videos) to run it.

1. Python with Capstone for the translator, in the repository's `.venv` (the Makefile uses it):

       python3 -m venv .venv
       .venv/bin/pip install capstone

2. SDL3 and, if you want it, FSR 2: see [third_party/README.md](third_party/README.md).
3. Build:

       make            # or make JOBS=4 to keep the machine usable

   The first run translates Game.exe (a few seconds) and compiles the generated C (a few
   minutes); later builds only redo what changed.
4. Play: `build/1nsane --window` (or `--fullscreen`). Settings are kept in `options.txt` in the
   user folder; the game's own menus change them.

## Windows

With [MSYS2](https://www.msys2.org), in its UCRT64 environment:

    pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf \
              mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone \
              mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-freetype mingw-w64-ucrt-x86_64-zlib \
              mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader \
              mingw-w64-ucrt-x86_64-glslang mingw-w64-ucrt-x86_64-libarchive \
              mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-7zip

Optional extras: `mingw-w64-ucrt-x86_64-lld` for DLSS, `mingw-w64-ucrt-x86_64-vulkan-validation-layers`
for INSANE_VK_VALIDATE=1, `mingw-w64-ucrt-x86_64-innoextract` to unpack the GOG installer. No Vulkan SDK
is needed (`make` is MSYS's own make package). FSR 2 and the optional DLSS SDK go in third_party/ as
its README says. `make` builds `build/1nsane.exe` and copies the helper programs it uses (FFmpeg,
7-Zip, libarchive) and their DLLs beside it, so it also runs outside MSYS2.

## Fan cars and maps

List folders of fan downloads in the user folder's `mods.txt`, one per line, as `cars <folder>`
or `maps <folder>`, and turn on Options > Enhanced Options > FAN CARS & MAPS. Downloads are only
ever read, never run.

## Checks

For changes to the runtime: `tools/lockstep.py` runs the translated game beside the original
code (`make build/1nsane_ref`, with Unicorn: `.venv/bin/pip install unicorn`) and compares their
memory frame by frame;
`make check` runs the function-level check. Both use `--test`, which turns the new options off. They
have been run on Linux; on Windows Unicorn is MSYS2's `mingw-w64-ucrt-x86_64-python-unicorn` (pass
`UNICORN=` its site-packages folder).
