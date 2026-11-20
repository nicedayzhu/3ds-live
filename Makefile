.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all handheld build-handheld test
all: handheld

test:
	$(MAKE) -C live/tests/native check

SD_APP := dist/SD/3ds/3ds-live
SD_MODULE := dist/SD/luma/sysmodules/0004013000F1D402.cxi

handheld: dist/3ds-live.3dsx dist/3ds-live.cia $(SD_APP)/3ds-live.3dsx $(SD_MODULE) $(SD_APP)/live.ini.example

build-handheld:
	$(MAKE) -C app

app/3ds-live.3dsx app/build/3ds-live.cia sysmod/3dslive.cxi: build-handheld
	@test -f $@

dist/3ds-live.3dsx: app/3ds-live.3dsx
dist/3ds-live.cia: app/build/3ds-live.cia
$(SD_APP)/3ds-live.3dsx: app/3ds-live.3dsx
$(SD_MODULE): sysmod/3dslive.cxi
$(SD_APP)/live.ini.example: live/config/live.ini.example

dist/3ds-live.3dsx dist/3ds-live.cia $(SD_APP)/3ds-live.3dsx $(SD_MODULE) $(SD_APP)/live.ini.example:
	@mkdir -p $(@D)
	cp $< $@
