# tools/raw-recv - PC side of the switch-frame-tap USB transport.
#   make            build raw-view (live viewer) and raw-recv (frame dumper)
#   make test       replay a stream built from real NVENC output through raw-view
# raw-view decodes H.264 (M83 nvstream) when libavcodec is found; without it it
# still shows the raw and packed-4:2:0 streams and says so for H.264 packets.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
BASE     = libusb-1.0 sdl2
H264    := $(shell pkg-config --exists libavcodec libavutil && echo 1)
MP4     := $(shell pkg-config --exists libavformat && echo 1)
ifeq ($(H264)$(MP4),11)
# v0.5: R records to MP4 (libavformat)
VIEW_CFLAGS = -DSFT_H264 -DSFT_MP4 $(shell pkg-config --cflags $(BASE) libavcodec libavutil libavformat)
VIEW_LIBS   = $(shell pkg-config --libs $(BASE) libavcodec libavutil libavformat)
else ifeq ($(H264),1)
VIEW_CFLAGS = -DSFT_H264 $(shell pkg-config --cflags $(BASE) libavcodec libavutil)
VIEW_LIBS   = $(shell pkg-config --libs $(BASE) libavcodec libavutil)
else
VIEW_CFLAGS = $(shell pkg-config --cflags $(BASE))
VIEW_LIBS   = $(shell pkg-config --libs $(BASE))
endif

# zero-copy VAAPI -> OpenGL (zerocopy.h) where libva is found (Linux);
# ZEROCOPY=0 builds without it
ifneq ($(shell uname -s),Darwin)
ifneq ($(ZEROCOPY),0)
ifeq ($(H264)$(shell pkg-config --exists libva && echo 1),11)
VIEW_CFLAGS += -DSFT_ZEROCOPY $(shell pkg-config --cflags libva)
VIEW_LIBS   += $(shell pkg-config --libs libva)
endif
endif
endif

all: raw-view raw-recv

ifeq ($(shell uname -s),Darwin)
CLARITY_OBJ = clarity.o
VIEW_LIBS += -framework Metal -framework QuartzCore
clarity_shader.h: clarity.metal
	xxd -i $< > $@
clarity.o: clarity.m clarity.h clarity_shader.h
	$(CC) $(CFLAGS) -fobjc-arc $(shell pkg-config --cflags sdl2) -c $< -o $@
METAL_SDL_CONFIG ?= sdl2-config
metal-fsr-test: metal-fsr-test.c raw-view.c upscale.h ui.h hud.h menu.h picture.h record.h clarity.h $(CLARITY_OBJ)
	$(CC) $(CFLAGS) -I$(shell $(METAL_SDL_CONFIG) --prefix)/include $(VIEW_CFLAGS) -o /tmp/frame-tap-metal-fsr-test $< $(CLARITY_OBJ) $(shell $(METAL_SDL_CONFIG) --static-libs) $(VIEW_LIBS) -lm
endif

raw-view: raw-view.c record.h menu.h hud.h picture.h update.h sdl_compat.h ../../common/sft_version.h upscale.h upscale_shaders.h font_dejavu.h clarity.h zerocopy.h $(CLARITY_OBJ)
	$(CC) $(CFLAGS) $(VIEW_CFLAGS) -o $@ $< $(CLARITY_OBJ) $(VIEW_LIBS) -lm
	@echo "raw-view built $(if $(filter 1,$(H264)),WITH,WITHOUT) H.264 (libavcodec)"

# v0.4: webcam-mode check (V4L2 + libavcodec)
uvc-check: uvc-check.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libavcodec libavutil) -o $@ $< $(shell pkg-config --libs libavcodec libavutil)

# v0.5: checks a recording (tools/raw-recv/mp4_record_test.sh)
mp4-check: mp4-check.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libavformat libavcodec libavutil) -o $@ $< $(shell pkg-config --libs libavformat libavcodec libavutil)

raw-recv: raw-recv.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libusb-1.0) -o $@ $< $(shell pkg-config --libs libusb-1.0)

test: raw-view
	python3 ../sft_stream_test.py

clean:
	rm -f raw-view raw-recv clarity.o clarity_shader.h

.PHONY: all test clean
