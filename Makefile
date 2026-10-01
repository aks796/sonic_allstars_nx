#---------------------------------------------------------------------------------
# Sonic & SEGA All-Stars Racing -- Nintendo Switch wrapper (32-bit / AArch32)
#
# Ships NO game code and NO game assets: the player's own APK and expansion
# file (main.20.com.sega.ssasr.obb, or the zip it came in), found in
# sd:/switch/sonic_allstars_nx/ by their contents whatever their names, are
# read at run time; the one library is unpacked from the APK on the first launch
# (source/ssr_setup_plan.c); the game's data is read in place (source/ssr_pack.h).
#
# The build is the android32 runtime's (runtime/runtime.mk: devkitARM +
# libnx32 + mesa32 from portlibs32/); ./build.sh runs it in the toolchain
# container. Output: sonicracing_nx.nsp, which the launcher NRO carries
# (launcher/).
#---------------------------------------------------------------------------------
TARGET               := sonicracing_nx
PORT_NPDM_PROGRAM_ID := 0x0100000000001012
# The intro movie (H.264 + AAC in MP4) and the music (MP3): FFmpeg's MOV
# demuxer and H.264 / AAC / MP3 decoders, ffmpeg32's LGPL build (components
# in portlibs32/README.md), copied into portlibs32/. Without them the build
# has no intro and no music (the game runs, silent between its sound effects).
DCR_VIDEO  := $(if $(wildcard portlibs32/lib/libavcodec.a),1,0)
PORT_STAMP := -$(DCR_VIDEO)
ifeq ($(DCR_VIDEO),1)
PORT_LIBS  := -L$(CURDIR)/portlibs32/lib -lavformat -lavcodec -lavutil
endif
# The Xbox 360 edition's SPLIT SCREEN card, built in when
# resources/xbox360/splitscreen_card.png is there (not in the public source:
# made from your own disc, see source/ssr_xcard.c); without it the card is
# read from a disc image on the SD card, if there is one.
ifneq ($(wildcard resources/xbox360/splitscreen_card.png),)
PORT_CFLAGS  := -DDCR_XCARD_BAKED=1
PORT_ASFLAGS := -DDCR_XCARD_BAKED=1
endif
include runtime/runtime.mk

# the wait for a free display buffer, measured (ssr_perf.c)
ifeq ($(DCR_GL_MESA),1)
LDFLAGS += -Wl,--wrap=nwindowDequeueBuffer
endif

# FFmpeg is built with int-sized enums (-fno-short-enums), and so are its
# users here, whose own interfaces (ssr.h) have no enums (hence
# --no-enum-size-warning).
FFMPEG_USERS := $(BUILD)/ssr_media.o $(BUILD)/ssr_video.o
$(FFMPEG_USERS): $(BUILD)/%.o: $(SOURCES)/%.c $(RENDERER_STAMP) | $(BUILD) $(BUILD)/dcr_build.h
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -fno-short-enums -DDCR_VIDEO=$(DCR_VIDEO) -c $< -o $@

# stb_image (public domain) is compiled into ssr_gfx.c; it warns about things
# that are its own, not ours.
$(BUILD)/ssr_gfx.o: CFLAGS += -Wno-sign-compare -Wno-unused-function \
  -Wno-unused-value -Wno-misleading-indentation -Wno-shadow -Wno-implicit-fallthrough \
  -Wno-type-limits -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-array-bounds

.PHONY: check
check:
	@echo "run on the host: python3 runtime/tools/gen_imports.py --libs <apk>/lib/armeabi"
	@echo "                 python3 runtime/tools/gen_imports.py --check"
