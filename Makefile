# 1NSANE native: build the runtime and the C generated from the user's own Game.exe.
#   make GAME=<folder with Game.exe>      translate + build build/1nsane
# Python with Capstone (and Unicorn, for the function check): the repository's .venv if there is one
PY ?= $(firstword $(wildcard .venv/bin/python) python3)
GAME ?= $(HOME)/.local/share/1nsane-native/game
BUILD_CFG ?= builds/gog_1_0.py
# Parallel compiles: each takes ~300 MB; keep it modest so tests and other work fit.
JOBS ?= 16
MAKEFLAGS += -j$(JOBS)
CC = gcc
SDL3 ?= third_party/sdl3
# AntiStatic, the shared recompilation toolkit (third_party/antistatic, pinned at a release tag):
# its translator, its runtime core (win32, cpu), its DirectDraw / Direct3D 7 renderer (d3d7) and
# its Windows host (win). runtime/ holds what is this game's: port.h, the game's hooks, menus,
# effects, mods, the intro videos and the upscalers' glue.
ANTISTATIC ?= third_party/antistatic
AS_RT = $(ANTISTATIC)/runtime
CFLAGS_COMMON = -std=gnu11 -g -fno-strict-aliasing -frounding-math -fno-omit-frame-pointer -Iruntime \
                -I$(AS_RT)/win32 -I$(AS_RT)/d3d7 -I$(AS_RT)/cpu -Igen -I$(SDL3)/include
CFLAGS_GEN = $(CFLAGS_COMMON) -O2 -w -DCPU_FAST_MEM
CFLAGS_RT = $(CFLAGS_COMMON) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
LDFLAGS = -rdynamic $(SDL3)/lib/libSDL3.a -pthread -lm -ldl -lz -lvulkan

RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(patsubst runtime/%.c,build/obj/rt_%.o,$(RT_SRC))
AS_SRC = $(wildcard $(AS_RT)/win32/*.c) $(wildcard $(AS_RT)/d3d7/*.c) $(AS_RT)/cpu/fpu.c
AS_OBJ = $(patsubst %.c,build/obj/as_%.o,$(notdir $(AS_SRC)))
AS_H = $(wildcard $(AS_RT)/win32/*.h $(AS_RT)/cpu/*.h $(AS_RT)/d3d7/*.h $(AS_RT)/d3d7/shaders/*.h)
vpath %.c $(AS_RT)/win32 $(AS_RT)/d3d7 $(AS_RT)/cpu
# The code files recomp.py wrote, listed in gen/files.mk once it has run (a fresh tree has none
# yet: make writes the list after translating, then reads it again).
-include gen/files.mk
GEN_SRC = $(GEN_CODE) gen/table.c gen/stubs.c
GEN_OBJ = $(patsubst gen/%.c,build/obj/gen_%.o,$(GEN_SRC))

# Windows (MSYS2 UCRT64, WINDOWS.md): the same sources, with the toolkit's runtime/win standing in
# for the POSIX pieces MinGW lacks (included before each runtime source); SDL3, zlib and the C/C++
# runtimes linked in, Vulkan's loader a system DLL, libarchive loaded at run time. The icon,
# manifest and resource script are 1NSANE's (runtime/win).
ifeq ($(OS),Windows_NT)
.DEFAULT_GOAL := all
EXE = .exe
PY = python
GAME = $(subst \,/,$(APPDATA))/1nsane-native/game
SDL3 = /ucrt64
CFLAGS_RT += -I$(AS_RT)/win -include $(AS_RT)/win/compat.h
LDFLAGS = -static $(SDL3)/lib/libSDL3.a -lm -liconv -luser32 -lgdi32 -lwinmm -limm32 \
          -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lcfgmgr32 -lshell32 -lpsapi -ldbghelp \
          -ldinput8 -lz -lpthread $(SDL3)/lib/libvulkan-1.dll.a -Wl,--stack,8388608 -mwindows
AS_OBJ += build/obj/as_platform.o build/obj/rt_platform_win_res.o
# the helper programs and their DLLs beside the program, from MSYS2 (so it runs without MSYS2's shell):
# fan content's archives (libarchive, 7-Zip) and the intro videos (ffmpeg)
HELPERS = /ucrt64/bin/libarchive-13.dll /ucrt64/bin/7z.exe /ucrt64/bin/7z.dll /ucrt64/bin/ffmpeg.exe
build/helpers.stamp: | build/obj
	for x in $(HELPERS); do cp -u $$x build/ && ldd $$x | awk '/ucrt64/{print $$3}' | xargs -r cp -u -t build/; done
	touch $@
all: build/helpers.stamp
$(RT_OBJ) $(AS_OBJ): $(AS_RT)/win/compat.h
build/obj/as_platform.o: $(AS_RT)/win/platform.c $(AS_RT)/win/*.h $(AS_RT)/win/sys/*.h runtime/port.h $(AS_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@
build/obj/rt_platform_win_res.o: runtime/win/1nsane.rc runtime/win/1nsane.manifest | build/obj
	windres -I runtime/win $< -O coff -o $@
endif

all: build/1nsane$(EXE)

# (the translator rewrites only the files that change and touches gen/.translated at the end of every
# run: that stamp is the rule's target, so an unchanged translation isn't redone at every make)
gen/.translated: $(wildcard $(ANTISTATIC)/translator/*.py) $(BUILD_CFG)
	$(PY) $(ANTISTATIC)/translator/recomp.py $(GAME)/Game.exe $(BUILD_CFG) gen
gen/funcs.h gen/table.c gen/stubs.c: gen/.translated ;
gen/files.mk: gen/.translated
	@echo 'GEN_CODE = $(sort $(wildcard gen/code_*.c))' > $@

build/obj/gen_%.o: gen/%.c gen/funcs.h $(wildcard $(AS_RT)/cpu/*.h) runtime/port.h | build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@

build/obj/rt_%.o: runtime/%.c runtime/*.h $(AS_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@

build/obj/as_%.o: %.c runtime/port.h $(AS_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@

# Shaders: the toolkit's runtime/d3d7/shaders, their SPIR-V headers kept in its repository.

# FSR 2 (third_party/FidelityFX-FSR2, fetched locally, MIT licence; vk.c's temporal anti-aliasing):
# its API and Vulkan backend (C++, as AMD ships them, with runtime/fsr2_compat.h for Linux), its
# shaders compiled into the headers the backend includes (tools/fsr2_shaders.py; AMD's Windows
# compiler is never run), and runtime/fsr2.cpp around them. Without the source, ANTI-ALIASING
# has no FSR.
GLSLANG ?= glslangValidator
FSR2 ?= third_party/FidelityFX-FSR2/src/ffx-fsr2-api
ifneq ($(wildcard $(FSR2)/ffx_fsr2.h),)
FSR2_PERM = build/fsr2/ffx_fsr2_accumulate_pass_permutations.h
FSR2_OBJ = build/obj/fsr2_api.o build/obj/fsr2_assert.o build/obj/fsr2_vk.o build/obj/fsr2_shaders_vk.o build/obj/rt_fsr2.o
CXXFLAGS_FSR2 = -std=c++17 -O2 -g -DFFX_GCC -include runtime/fsr2_compat.h -I$(FSR2) -Ibuild/fsr2 -Iruntime -I$(AS_RT)/d3d7 -w
CFLAGS_RT += -DHAVE_FSR2
LDFLAGS += -lstdc++
EXTRA_OBJ += $(FSR2_OBJ)
$(FSR2_PERM): tools/fsr2_shaders.py | build/obj
	$(PY) tools/fsr2_shaders.py $(FSR2) build/fsr2 $(GLSLANG)
build/obj/fsr2_api.o: $(FSR2)/ffx_fsr2.cpp runtime/fsr2_compat.h | build/obj
	$(CXX) $(CXXFLAGS_FSR2) -c $< -o $@
build/obj/fsr2_assert.o: $(FSR2)/ffx_assert.cpp runtime/fsr2_compat.h | build/obj
	$(CXX) $(CXXFLAGS_FSR2) -c $< -o $@
# (its memory types through fsr2.cpp: it would take AMD's device-coherent ones, which need a feature)
build/obj/fsr2_vk.o: $(FSR2)/vk/ffx_fsr2_vk.cpp runtime/fsr2_compat.h | build/obj
	$(CXX) $(CXXFLAGS_FSR2) -DvkGetPhysicalDeviceMemoryProperties=fsr2_memory_properties -c $< -o $@
build/obj/fsr2_shaders_vk.o: $(FSR2)/vk/shaders/ffx_fsr2_shaders_vk.cpp $(FSR2_PERM) runtime/fsr2_compat.h | build/obj
	$(CXX) $(CXXFLAGS_FSR2) -c $< -o $@
build/obj/rt_fsr2.o: runtime/fsr2.cpp $(AS_RT)/d3d7/fsr2.h runtime/fsr2_compat.h | build/obj
	$(CXX) $(CXXFLAGS_FSR2) -c $< -o $@
endif

# DLSS (third_party/DLSS, NVIDIA's SDK, fetched locally; runtime/dlss.c, vk.c's other upscaler on the
# same inputs as FSR 2). Windows only so far: NGX's link library is built for MSVC, so the program is
# linked with lld (MSYS2's lld package), which reads MSVC objects, and the toolkit's
# runtime/win/msvc_shim.c stands in for what they expect of Microsoft's runtime. nvngx_dlss.dll goes
# next to the program. Without the SDK, ANTI-ALIASING has no DLSS.
DLSS ?= third_party/DLSS
comma := ,
ifeq ($(OS),Windows_NT)
ifneq ($(wildcard $(DLSS)/include/nvsdk_ngx_vk.h),)
CFLAGS_RT += -DHAVE_DLSS -I$(DLSS)/include
LDFLAGS += -fuse-ld=lld $(DLSS)/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib \
           $(addprefix -Wl$(comma)-Xlink=-nodefaultlib:,oldnames msvcprt msvcrt libcmt libcpmt vcruntime ucrt)
AS_OBJ += build/obj/as_msvc_shim.o
build/obj/as_msvc_shim.o: $(AS_RT)/win/msvc_shim.c | build/obj
	$(CC) -O2 -Wall -c $< -o $@
build/nvngx_dlss.dll: $(DLSS)/lib/Windows_x86_64/rel/nvngx_dlss.dll | build/obj
	cp $< $@
all: build/nvngx_dlss.dll
endif
endif

# The button icons (assets/kenney-input-prompts) built into the program
gen/xbox_prompts_png.h: assets/kenney-input-prompts/xbox_prompts.png gen/funcs.h
	$(PY) -c "import sys; d = open(sys.argv[1], 'rb').read(); sys.stdout.buffer.write(b'static const unsigned char xbox_prompts_png[%d] = {%s};\n' % (len(d), ','.join(map(str, d)).encode()))" $< > $@
build/obj/rt_prompts.o: gen/xbox_prompts_png.h

# The fonts of the FONT option (font.c; assets/1nsanefont) built into the program, drawn with FreeType: found
# by pkg-config (MSYS2 on Windows: mingw-w64-ucrt-x86_64-freetype, linked statically); without it FONT keeps
# the game's own letters.
gen/font_1nsane_otf.h: assets/1nsanefont/1nsaneFont.otf gen/funcs.h
	$(PY) -c "import sys; d = open(sys.argv[1], 'rb').read(); sys.stdout.buffer.write(b'static const unsigned char font_1nsane_otf[%d] = {%s};\n' % (len(d), ','.join(map(str, d)).encode()))" $< > $@
gen/font_mirabel_otf.h: assets/1nsanefont/MirabelGothish.otf gen/funcs.h
	$(PY) -c "import sys; d = open(sys.argv[1], 'rb').read(); sys.stdout.buffer.write(b'static const unsigned char font_mirabel_otf[%d] = {%s};\n' % (len(d), ','.join(map(str, d)).encode()))" $< > $@
build/obj/rt_font.o: gen/font_1nsane_otf.h gen/font_mirabel_otf.h
ifneq ($(shell pkg-config --exists freetype2 2>/dev/null && echo yes),)
CFLAGS_RT += -DHAVE_FREETYPE $(shell pkg-config --cflags freetype2)
ifeq ($(OS),Windows_NT)
LDFLAGS += $(shell pkg-config --static --libs freetype2)
else
LDFLAGS += $(shell pkg-config --libs freetype2)
endif
endif

# The car class table (CAR CLASSES REORGANIZED; assets/car_classes.txt, made by tools/car_classes.py)
gen/car_classes_txt.h: assets/car_classes.txt gen/funcs.h
	$(PY) -c "import sys; d = open(sys.argv[1], 'rb').read(); sys.stdout.buffer.write(b'static const char car_classes_txt[] = {%s,0};\n' % ','.join(map(str, d)).encode())" $< > $@
build/obj/rt_classes.o: gen/car_classes_txt.h

build/1nsane$(EXE): $(RT_OBJ) $(AS_OBJ) $(GEN_OBJ) $(EXTRA_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

build/obj:
	mkdir -p $@

# Function-level check against the original code in Unicorn (tests/fncheck.c)
UNICORN ?= $(firstword $(wildcard .venv/lib/python3*/site-packages/unicorn))
build/obj/as_main_nomain.o: $(AS_RT)/win32/main.c runtime/port.h $(AS_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -DRUNTIME_NO_MAIN -c $< -o $@

build/fncheck: tests/fncheck.c $(RT_OBJ) $(filter-out build/obj/as_main.o,$(AS_OBJ)) build/obj/as_main_nomain.o $(GEN_OBJ) build/obj/gen_x87_special.o $(EXTRA_OBJ)
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

# Reference build: the original code in Unicorn on the same runtime (ref/ref_cpu.c)
build/obj/ref_cpu.o: ref/ref_cpu.c runtime/*.h $(AS_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -c $< -o $@

build/1nsane_ref: $(RT_OBJ) $(AS_OBJ) $(GEN_OBJ) build/obj/gen_x87_special.o build/obj/ref_cpu.o $(EXTRA_OBJ)
	$(CC) -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

check: build/fncheck
	build/fncheck --game $(GAME)

clean:
	rm -rf build

.PHONY: all clean check
