# Build the installed Luma system title with devkitARM, libctru and makerom.
.DEFAULT_GOAL := all
ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM to the devkitARM SDK directory)
endif
include $(DEVKITARM)/3ds_rules

NAME := 3dslive$(if $(filter 1,$(LIVE_FRAME_TIMING)),-timing)
BUILD := build/$(NAME)
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CPPFLAGS := -D__3DS__ -Isource -I../codec/include -I../third_party/lz4 -I$(CTRULIB)/include
ifeq ($(LIVE_FRAME_TIMING),1)
CPPFLAGS += -DLIVE_FRAME_TIMING
endif
CFLAGS := $(ARCH) -g -O2 -Wall -Wextra -std=gnu11 -mword-relocations -fno-strict-aliasing -ffunction-sections -fdata-sections
LDFLAGS := $(ARCH) -specs=3dsx.specs -Wl,--gc-sections -Wl,-Map,$(BUILD)/module.map
SOURCES := main status_led capture live_stream live_timing live_jpeg live_jpeg_source live_audio live_codec convert lz4
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SOURCES)))
vpath %.c source ../codec/source ../third_party/lz4

.PHONY: all clean
all: $(NAME).cxi

$(NAME).cxi: $(BUILD)/module.elf 3dslive.rsf
	makerom -f ncch -rsf 3dslive.rsf -o $@ -elf $<

$(BUILD)/module.elf: $(OBJECTS) $(BUILD)/module_id.o
	$(CC) $(LDFLAGS) $^ -L$(CTRULIB)/lib -lctru -lm -o $@

# The launcher must distinguish the module in memory from the one now on SD,
# including two locally rebuilt modules with the same release version.
$(BUILD)/module_id.h: $(OBJECTS) 3dslive.rsf $(CTRULIB)/lib/libctru.a
	@sha256sum $^ > $@.inputs
	@sha256sum $@.inputs > $@.digest
	@read digest rest < $@.digest && printf '#define LIVE_MODULE_BUILD_ID "%s"\n' "$$digest" > $@.tmp
	@mv $@.tmp $@

$(BUILD)/module_id.o: $(BUILD)/module_id.h
$(BUILD)/module_id.o: CPPFLAGS += -I$(BUILD)

$(BUILD)/live_jpeg.o $(BUILD)/live_jpeg_source.o $(BUILD)/live_codec.o $(BUILD)/lz4.o: CFLAGS += -O3
$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf build 3dslive.cxi 3dslive-timing.cxi

-include $(OBJECTS:.o=.d)

