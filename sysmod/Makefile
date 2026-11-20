# Build the installed Luma system title with devkitARM, libctru and makerom.
.DEFAULT_GOAL := all
ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM to the devkitARM SDK directory)
endif
include $(DEVKITARM)/3ds_rules

NAME := 3dslive$(if $(filter 1,$(LIVE_FRAME_TIMING)),-timing)
BUILD := build/$(NAME)
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CPPFLAGS := -D__3DS__ -Isource -I../codec/include -I$(CTRULIB)/include
ifeq ($(LIVE_FRAME_TIMING),1)
CPPFLAGS += -DLIVE_FRAME_TIMING
endif
CFLAGS := $(ARCH) -g -O2 -Wall -Wextra -std=gnu11 -mword-relocations -fno-strict-aliasing -ffunction-sections -fdata-sections
LDFLAGS := $(ARCH) -specs=3dsx.specs -Wl,--gc-sections -Wl,-Map,$(BUILD)/module.map
SOURCES := main capture live_stream live_timing live_jpeg live_jpeg_source live_audio live_codec convert
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SOURCES)))
vpath %.c source ../codec/source

.PHONY: all clean
all: $(NAME).cxi

$(NAME).cxi: $(BUILD)/module.elf 3dslive.rsf
	makerom -f ncch -rsf 3dslive.rsf -o $@ -elf $<

$(BUILD)/module.elf: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -L$(CTRULIB)/lib -lctru -lm -o $@

$(BUILD)/live_jpeg.o $(BUILD)/live_jpeg_source.o $(BUILD)/live_codec.o: CFLAGS += -O3
$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf build 3dslive.cxi 3dslive-timing.cxi

-include $(OBJECTS:.o=.d)

