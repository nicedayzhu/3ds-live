# Portable tests for the packet formats, encoder budgets and discovery lease.
CC ?= cc
OUT := ../../build/native-tests
CPPFLAGS += -I../../../codec/include
CFLAGS += -O2 -std=c11 -Wall -Wextra -Werror
TESTS := test_live test_pacing test_rate test_pair test_repair_cache test_pending_queue test_jpeg_rows test_row_compression test_audio_transport test_discovery test_capture_schedule test_wide_capture test_receiver_feedback test_status_led test_live_config test_live_net_report test_live_control test_live_stats test_live_status test_module_install test_launcher_status test_launcher_language
BINS := $(addprefix $(OUT)/,$(TESTS))

.PHONY: check
check: $(BINS)
	@set -e; for test in $(BINS); do "$$test"; done

$(OUT):
	@mkdir -p $@

$(OUT)/test_live: ../../../codec/source/live_codec.c
$(OUT)/test_wide_capture: ../../../codec/source/live_jpeg_source.c
$(OUT)/test_capture_schedule: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_pacing: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_pacing: ../../../sysmod/source/packet_report.h
$(OUT)/test_capture_schedule: ../../../sysmod/source/capture_schedule.h
$(OUT)/test_pending_queue: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_pending_queue: ../../../sysmod/source/pending_queue.h
$(OUT)/test_row_compression: CPPFLAGS += -I../../../sysmod/source -I../../../third_party/lz4
$(OUT)/test_row_compression: ../../../sysmod/source/row_compression.h ../../../third_party/lz4/lz4.c ../../../third_party/lz4/lz4.h
$(OUT)/test_status_led: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_status_led: ../../../sysmod/source/status_led.h ../../../sysmod/source/status_led_pattern.h
$(OUT)/test_module_install: CPPFLAGS += -I../../../app/source
$(OUT)/test_module_install: ../../../app/source/module_install.c ../../../app/source/module_install.h
$(OUT)/test_launcher_status: CPPFLAGS += -I../../../app/source
$(OUT)/test_launcher_status: ../../../app/source/launcher_status.c ../../../app/source/launcher_status.h
$(OUT)/test_launcher_language: CPPFLAGS += -I../../../app/source
$(OUT)/test_launcher_language: ../../../app/source/launcher_language.c ../../../app/source/launcher_language.h
$(OUT)/test_live_config: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_live_config: ../../../sysmod/source/live_config.c ../../../sysmod/source/live_config.h
$(OUT)/test_live_net_report: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_live_net_report: ../../../sysmod/source/live_net_report_format.c ../../../sysmod/source/live_net_report.h
$(OUT)/test_live_control: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_live_control: ../../../sysmod/source/live_control.c ../../../sysmod/source/live_control.h
$(OUT)/test_live_stats: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_live_stats: ../../../sysmod/source/live_stats.c ../../../sysmod/source/live_stats.h
$(OUT)/test_live_status: CPPFLAGS += -I../../../sysmod/source
$(OUT)/test_live_status: ../../../sysmod/source/live_status.c ../../../sysmod/source/live_status.h
$(OUT)/%: %.c $(wildcard ../../../codec/include/live*.h) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) -o $@
