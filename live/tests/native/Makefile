# Portable tests for the packet formats, encoder budgets and discovery lease.
CC ?= cc
OUT := ../../build/native-tests
CPPFLAGS += -I../../../codec/include
CFLAGS += -O2 -std=c11 -Wall -Wextra -Werror
TESTS := test_live test_pacing test_rate test_pair test_audio_transport test_discovery test_capture_schedule
BINS := $(addprefix $(OUT)/,$(TESTS))

.PHONY: check
check: $(BINS)
	@set -e; for test in $(BINS); do "$$test"; done

$(OUT):
	@mkdir -p $@

$(OUT)/test_live: ../../../codec/source/live_codec.c
$(OUT)/test_capture_schedule: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_capture_schedule: ../../../sysmod/source/capture_schedule.h
$(OUT)/%: %.c $(wildcard ../../../codec/include/live*.h) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) -o $@
