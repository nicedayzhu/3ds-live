.SUFFIXES:
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
include $(DEVKITARM)/3ds_rules

APP_TITLE := 3DS Live
APP_DESCRIPTION := Wireless dual-screen streaming for New Nintendo 3DS (v$(strip $(file <../VERSION)))
APP_AUTHOR := nicedayzhu
APP_ICON := assets/icon.png
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -O2 -Wall -Wextra -Werror -std=gnu11 $(ARCH) -mword-relocations -D__3DS__ -I$(CTRULIB)/include -I../codec/include -I../sysmod/build/3dslive
OBJECTS := $(patsubst source/%.c,build/%.o,$(wildcard source/*.c)) build/launcher_font.o
MODULE := ../sysmod/3dslive.cxi
MODULE_ID := ../sysmod/build/3dslive/module_id.h
EMBEDDED := build/romfs/3ds-live.cxi
PYTHON ?= $(if $(shell command -v python3),python3,$(if $(shell command -v py),py -3,python))

.PHONY: all check-module
all: 3ds-live.3dsx build/3ds-live.cia

check-module:
	$(MAKE) -C ../sysmod LIVE_FRAME_TIMING=0

$(MODULE) $(MODULE_ID): check-module
	@test -f $@

$(EMBEDDED): $(MODULE)
	@mkdir -p $(@D)
	cp $< $@

build/main.o: $(MODULE_ID)
build/launcher_ui.o: CPPFLAGS += -Ibuild
build/launcher_ui.o: build/launcher_logo.h
build/launcher_logo.h: $(APP_ICON) make_launcher_logo.py
	@mkdir -p $(@D)
	$(PYTHON) make_launcher_logo.py $< $@

build/launcher_font.o: source/launcher_font.S assets/launcher-font.bin assets/NotoSansSC-OFL.txt
	@mkdir -p build
	$(CC) $(ARCH) -c $< -o $@

build/%.o: source/%.c Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

3ds-live.elf: $(OBJECTS)
	$(CC) $(ARCH) -specs=3dsx.specs $^ -L$(CTRULIB)/lib -lctru -lm -o $@

3ds-live.smdh: $(APP_ICON) Makefile ../VERSION
	smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $< $@

3ds-live.3dsx: 3ds-live.elf 3ds-live.smdh $(EMBEDDED)
	3dsxtool $< $@ --smdh=3ds-live.smdh --romfs=build/romfs

build/banner.bnr: assets/banner.png assets/banner.wav
	@mkdir -p $(@D)
	bannertool makebanner -i $< -a assets/banner.wav -o $@

build/3ds-live.cia: 3ds-live.elf 3ds-live.smdh build/banner.bnr cia.rsf $(EMBEDDED) cia_version.py ../VERSION
	makerom -f cia -o $@ -rsf cia.rsf -elf $< -icon 3ds-live.smdh -banner build/banner.bnr -exefslogo -ver "$$($(PYTHON) cia_version.py --print ../VERSION)"
	$(PYTHON) cia_version.py $@ ../VERSION

-include $(OBJECTS:.o=.d)
