.SUFFIXES:
include $(DEVKITARM)/3ds_rules

APP_TITLE := 3DS Live
APP_DESCRIPTION := Wireless dual-screen streaming for New Nintendo 3DS
APP_AUTHOR := 3DS Live
APP_ICON := assets/icon.png
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -O2 -Wall -Wextra -Werror -std=gnu11 $(ARCH) -D__3DS__ -I$(CTRULIB)/include -I../codec/include

.PHONY: all
all: 3ds-live.3dsx

build/main.o: source/main.c Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

3ds-live.elf: build/main.o
	$(CC) $(ARCH) -specs=3dsx.specs $< -L$(CTRULIB)/lib -lctru -lm -o $@

3ds-live.smdh: $(APP_ICON) Makefile
	smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $< $@

3ds-live.3dsx: 3ds-live.elf 3ds-live.smdh
	3dsxtool $< $@ --smdh=3ds-live.smdh

-include build/main.d
