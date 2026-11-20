.PHONY: all handheld test
all: handheld

test:
	$(MAKE) -C live/tests/native check

handheld:
	$(MAKE) -C sysmod
	$(MAKE) -C app
	@mkdir -p dist/SD/3ds/3ds-live dist/SD/luma/sysmodules
	cp app/3ds-live.3dsx dist/SD/3ds/3ds-live/3ds-live.3dsx
	cp sysmod/3dslive.cxi dist/SD/luma/sysmodules/0004013000F1D402.cxi
	cp live/config/live.ini.example dist/SD/3ds/3ds-live/live.ini.example
