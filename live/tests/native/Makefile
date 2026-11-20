# Portable tests for the packet formats, encoder budgets and discovery lease.
CC ?= cc
OUT := ../../build/native-tests
CPPFLAGS += -I../../../codec/include
CFLAGS += -O2 -std=c11 -Wall -Wextra -Werror
TESTS := test_live test_pacing test_rate test_pair test_audio_transport test_discovery test_capture_schedule test_wide_capture test_receiver_feedback test_status_led test_module_install test_launcher_status
BINS := $(addprefix $(OUT)/,$(TESTS))

.PHONY: check
check: $(BINS)
	@set -e; for test in $(BINS); do "$$test"; done

$(OUT):
	@mkdir -p $@

$(OUT)/test_live: ../../../codec/source/live_codec.c
$(OUT)/test_wide_capture: ../../../codec/source/live_jpeg_source.c
$(OUT)/test_capture_schedule: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_capture_schedule: ../../../sysmod/source/capture_schedule.h
$(OUT)/test_status_led: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_status_led: ../../../sysmod/source/status_led.h ../../../sysmod/source/status_led_pattern.h
$(OUT)/test_module_install: CPPFLAGS += -I../../../app/source
$(OUT)/test_module_install: ../../../app/source/module_install.c ../../../app/source/module_install.h
$(OUT)/test_launcher_status: CPPFLAGS += -I../../../app/source
$(OUT)/test_launcher_status: ../../../app/source/launcher_status.c ../../../app/source/launcher_status.h
$(OUT)/%: %.c $(wildcard ../../../codec/include/live*.h) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) -o $@
