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
CFLAGS_COMMON = -std=gnu11 -g -fno-strict-aliasing -frounding-math -fno-omit-frame-pointer -Iruntime -Igen -I$(SDL3)/include
CFLAGS_GEN = $(CFLAGS_COMMON) -O2 -w -DCPU_FAST_MEM
CFLAGS_RT = $(CFLAGS_COMMON) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
LDFLAGS = -rdynamic $(SDL3)/lib/libSDL3.a -pthread -lm -ldl -lz -lvulkan

RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(patsubst runtime/%.c,build/obj/rt_%.o,$(RT_SRC))
# The code files recomp.py wrote, listed in gen/files.mk once it has run (a fresh tree has none
# yet: make writes the list after translating, then reads it again).
-include gen/files.mk
GEN_SRC = $(GEN_CODE) gen/table.c gen/stubs.c
GEN_OBJ = $(patsubst gen/%.c,build/obj/gen_%.o,$(GEN_SRC))

# Windows (MSYS2 UCRT64, WINDOWS.md): the same sources, with runtime/win standing in for the
# POSIX pieces MinGW lacks (included before each runtime source); SDL3, zlib and the C/C++
# runtimes linked in, Vulkan's loader a system DLL, libarchive loaded at run time.
ifeq ($(OS),Windows_NT)
.DEFAULT_GOAL := all
EXE = .exe
PY = python
GAME = $(subst \,/,$(APPDATA))/1nsane-native/game
SDL3 = /ucrt64
CFLAGS_RT += -Iruntime/win -include runtime/win/compat.h
LDFLAGS = -static $(SDL3)/lib/libSDL3.a -lm -liconv -luser32 -lgdi32 -lwinmm -limm32 \
          -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8 -lz -lpthread \
          $(SDL3)/lib/libvulkan-1.dll.a -Wl,--stack,8388608 -mwindows
RT_OBJ += build/obj/rt_platform_win.o build/obj/rt_platform_win_res.o
# the helper programs and their DLLs beside the program, from MSYS2 (so it runs without MSYS2's shell):
# fan content's archives (libarchive, 7-Zip) and the intro videos (ffmpeg)
HELPERS = /ucrt64/bin/libarchive-13.dll /ucrt64/bin/7z.exe /ucrt64/bin/7z.dll /ucrt64/bin/ffmpeg.exe
build/helpers.stamp: | build/obj
	for x in $(HELPERS); do cp -u $$x build/ && ldd $$x | awk '/ucrt64/{print $$3}' | xargs -r cp -u -t build/; done
	touch $@
all: build/helpers.stamp
$(RT_OBJ): runtime/win/compat.h
build/obj/rt_platform_win.o: runtime/win/platform.c runtime/win/*.h runtime/win/sys/*.h runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@
build/obj/rt_platform_win_res.o: runtime/win/1nsane.rc runtime/win/1nsane.manifest | build/obj
	windres -I runtime/win $< -O coff -o $@
endif

all: build/1nsane$(EXE)

gen/funcs.h: $(filter-out tools/fsr2_shaders.py tools/car_classes.py,$(wildcard tools/*.py)) $(BUILD_CFG)
	$(PY) tools/recomp.py $(GAME)/Game.exe $(BUILD_CFG) gen
gen/table.c gen/stubs.c: gen/funcs.h ;
gen/files.mk: gen/funcs.h
	@echo 'GEN_CODE = $(sort $(wildcard gen/code_*.c))' > $@

build/obj/gen_%.o: gen/%.c gen/funcs.h runtime/cpu.h | build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@

build/obj/rt_%.o: runtime/%.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@

# Shaders: SPIR-V headers are generated with glslang and kept in the repository, so building
# needs glslang only after a shader changed.
SHADERS = $(wildcard runtime/shaders/*.vert runtime/shaders/*.frag)
# draw.frag three times: as it is, with the ray-traced shadows' extra outputs (vk.c's main view),
# and with ray-traced reflections (its reflective draws again after the view); the draw shaders
# again with the motion vectors of temporal anti-aliasing's scene (motion.glsl); the volumetric
# smoke's applying again with that scene's masks
SPV_H = $(addsuffix .spv.h,$(SHADERS)) runtime/shaders/draw.frag.shadow.spv.h runtime/shaders/draw.frag.refl.spv.h \
        runtime/shaders/draw.frag.motion.spv.h runtime/shaders/draw.frag.shadow.motion.spv.h \
        runtime/shaders/draw.vert.motion.spv.h runtime/shaders/tnl.vert.motion.spv.h \
        runtime/shaders/draw.frag.refl.motion.spv.h runtime/shaders/smoke.frag.motion.spv.h
SH_INC = runtime/shaders/shadow.glsl runtime/shaders/rt.glsl runtime/shaders/refl.glsl runtime/shaders/motion.glsl \
         runtime/shaders/smoke.glsl
GLSLANG ?= glslangValidator
ifneq ($(shell command -v $(GLSLANG) 2>/dev/null),)
runtime/shaders/%.spv.h: runtime/shaders/% $(SH_INC)
	$(GLSLANG) --target-env vulkan1.3 --vn spv_$(subst .,_,$(notdir $<)) -o $@ $<
runtime/shaders/draw.frag.shadow.spv.h: runtime/shaders/draw.frag $(SH_INC)
	$(GLSLANG) --target-env vulkan1.3 -DSHADOW_OUTPUTS --vn spv_draw_frag_shadow -o $@ $<
runtime/shaders/draw.frag.refl.spv.h: runtime/shaders/draw.frag $(SH_INC)
	$(GLSLANG) --target-env vulkan1.3 -DRT_REFLECT --vn spv_draw_frag_refl -o $@ $<
runtime/shaders/draw.frag.motion.spv.h: runtime/shaders/draw.frag $(SH_INC)
	$(GLSLANG) --target-env vulkan1.3 -DMOTION --vn spv_draw_frag_motion -o $@ $<
runtime/shaders/draw.frag.shadow.motion.spv.h: runtime/shaders/draw.frag $(SH_INC)
	$(GLSLANG) --target-env vulkan1.3 -DSHADOW_OUTPUTS -DMOTION --vn spv_draw_frag_shadow_motion -o $@ $<
runtime/shaders/draw.frag.refl.motion.spv.h: runtime/shaders/draw.frag $(SH_INC)
	$(GLSLANG) --target-env vulkan1.3 -DRT_REFLECT -DMOTION --vn spv_draw_frag_refl_motion -o $@ $<
runtime/shaders/smoke.frag.motion.spv.h: runtime/shaders/smoke.frag $(SH_INC)
	$(GLSLANG) --target-env vulkan1.3 -DMOTION --vn spv_smoke_frag_motion -o $@ $<
runtime/shaders/%.vert.motion.spv.h: runtime/shaders/%.vert $(SH_INC)
	$(GLSLANG) --target-env vulkan1.3 -DMOTION --vn spv_$*_vert_motion -o $@ $<
endif
build/obj/rt_vk.o: $(SPV_H)
shaders: $(SPV_H)

# FSR 2 (third_party/FidelityFX-FSR2, fetched locally, MIT licence; vk.c's temporal anti-aliasing):
# its API and Vulkan backend (C++, as AMD ships them, with runtime/fsr2_compat.h for Linux), its
# shaders compiled into the headers the backend includes (tools/fsr2_shaders.py; AMD's Windows
# compiler is never run), and runtime/fsr2.cpp around them. Without the source, ANTI-ALIASING
# has no FSR.
FSR2 ?= third_party/FidelityFX-FSR2/src/ffx-fsr2-api
ifneq ($(wildcard $(FSR2)/ffx_fsr2.h),)
FSR2_PERM = build/fsr2/ffx_fsr2_accumulate_pass_permutations.h
FSR2_OBJ = build/obj/fsr2_api.o build/obj/fsr2_assert.o build/obj/fsr2_vk.o build/obj/fsr2_shaders_vk.o build/obj/rt_fsr2.o
CXXFLAGS_FSR2 = -std=c++17 -O2 -g -DFFX_GCC -include runtime/fsr2_compat.h -I$(FSR2) -Ibuild/fsr2 -Iruntime -w
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
build/obj/rt_fsr2.o: runtime/fsr2.cpp runtime/fsr2.h runtime/fsr2_compat.h | build/obj
	$(CXX) $(CXXFLAGS_FSR2) -c $< -o $@
endif

# DLSS (third_party/DLSS, NVIDIA's SDK, fetched locally; runtime/dlss.c, vk.c's other upscaler on the
# same inputs as FSR 2). Windows only so far: NGX's link library is built for MSVC, so the program is
# linked with lld (MSYS2's lld package), which reads MSVC objects, and runtime/win/msvc_shim.c stands
# in for what they expect of Microsoft's runtime. nvngx_dlss.dll goes next to the program. Without
# the SDK, ANTI-ALIASING has no DLSS.
DLSS ?= third_party/DLSS
comma := ,
ifeq ($(OS),Windows_NT)
ifneq ($(wildcard $(DLSS)/include/nvsdk_ngx_vk.h),)
CFLAGS_RT += -DHAVE_DLSS -I$(DLSS)/include
LDFLAGS += -fuse-ld=lld $(DLSS)/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib \
           $(addprefix -Wl$(comma)-Xlink=-nodefaultlib:,oldnames msvcprt msvcrt libcmt libcpmt vcruntime ucrt)
RT_OBJ += build/obj/rt_msvc_shim.o
build/obj/rt_msvc_shim.o: runtime/win/msvc_shim.c | build/obj
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

build/1nsane$(EXE): $(RT_OBJ) $(GEN_OBJ) $(EXTRA_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

build/obj:
	mkdir -p $@

# Function-level check against the original code in Unicorn (tests/fncheck.c)
UNICORN ?= $(firstword $(wildcard .venv/lib/python3*/site-packages/unicorn))
build/obj/rt_main_nomain.o: runtime/main.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -DRUNTIME_NO_MAIN -c $< -o $@

build/fncheck: tests/fncheck.c $(filter-out build/obj/rt_main.o,$(RT_OBJ)) build/obj/rt_main_nomain.o $(GEN_OBJ) build/obj/gen_x87_special.o $(EXTRA_OBJ)
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

# Reference build: the original code in Unicorn on the same runtime (ref/ref_cpu.c)
build/obj/ref_cpu.o: ref/ref_cpu.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -c $< -o $@

build/1nsane_ref: $(RT_OBJ) $(GEN_OBJ) build/obj/gen_x87_special.o build/obj/ref_cpu.o $(EXTRA_OBJ)
	$(CC) -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

check: build/fncheck
	build/fncheck --game $(GAME)

clean:
	rm -rf build

.PHONY: all clean check shaders
