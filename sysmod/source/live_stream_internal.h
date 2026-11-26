/*
 * Shared state for the 3DS stream pipeline.
 *
 * One instance lives in live_stream.c.  Encoder and sender translation units
 * include this header and operate on that instance; no subsystem keeps its
 * own copy of these fields.
 */
#ifndef LIVE_STREAM_INTERNAL_H
#define LIVE_STREAM_INTERNAL_H

#include "live_audio_sender.h"
#include "live_config.h"
#include "live_net.h"
#include "live_r20.h"
#include "live_repair_tx.h"
#include "live_row.h"
#include "live_timing.h"
#include "pending_queue.h"
#include "packet_report.h"
#include "row_compression.h"
#include "live_jpeg.h"
#include "live_jpeg_source.h"
#include "live_pair.h"
#include "live_pacing.h"
#include "live_receiver_feedback.h"
#include "live_row_refresh.h"

#include <3ds.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_JPEG LIVE_JPEG_MAX_BYTES

struct stream_counters {
	u32 attempts[2], jpeg_frames[2], max_us[2], max_bytes[2];
	u32 convert_count[2], convert_max_us[2];
	u64 sum_us[2], convert_sum_us[2], changed_mcus[2], converted_mcus[2];
	u64 snapshot_sum_us;
	u32 snapshot_count, snapshot_max_us;
	u32 source_reuse[2], last_source[2], frames[2];
	u32 replaced, errors, expired, oversize, retries, pairs;
	u32 send_calls, send_max_us, last_encode_us[2];
	u32 feedback_count, config_changes, rate_pending_kept,
	    rate_encoding_kept;
	u64 send_sum_us, video_wire_bytes, compact_saved_bytes;
	u32 compact_frames, compact_fallbacks;
	u32 manual_render_failures, manual_wire_drops;
};

struct stream_settings {
	bool enabled;
	bool compact_jpeg;
	bool audio_enabled;
	bool combined_fec;
	u32 fps;
	u32 rate;
	u32 config_epoch;
	u32 config_sequence;
	unsigned quality_target;
	unsigned quality;
	unsigned spare;
	u32 screen_mask;
	unsigned pacing_mode;
};

struct stream_buffers {
	LiveJpegSource *raw_sources[2];
	LiveJpegFrame *raw_working[2];
	size_t raw_bytes[2];
	_Alignas(LZ4_stream_t) u8 encoded[2][MAX_JPEG];
	u8 pending_bytes[2 * MAX_JPEG];
	u8 send_arena[2 * MAX_JPEG];
};

struct stream_runtime {
	bool raw_valid[2];
	bool soc_ready;
	bool receiver_video;
	bool capture_paused;
	volatile bool stopping;
	u32 raw_pts[2];
	u32 plan_pts[2];
	u32 plan_stamp;
	u32 plan_mask;
	u32 sequence;
	u32 session;
	u64 started;
	LightLock lock;
	LightLock raw_locks[2];
	LightEvent events[3];
	Thread worker;
	Thread encoders[2];
	s32 encoder_cores[2];
	u32 job_kind;
	u32 job_quality;
	u32 job_done;
	u32 job_size[2];
	u32 job_changed[2];
	u32 job_us[2];
	LivePayloadControl payload_control;
	LivePacketReport packet_report;
	u64 last_status_tick;
	u64 last_control_tick;
	u64 send_active_ticks;
	u32 repair_requests;
	u64 repair_interest;
	u64 repair_peer_at;
	u64 last_repair_poll;
	LiveReceiverFeedback receiver_feedback;
	int last_errno;
};

struct row_state {
	struct row_anchor *banks;
	struct row_anchor *base;
	struct row_anchor *candidate;
	struct row_anchor *probe;
	LiveRowTrial trial;
	u64 probe_after;
	bool enabled;
	unsigned sends;
	u64 last_key;
	u64 last_ack;
	u64 bytes_saved;
	u32 keys;
	u32 repeats;
	u32 deltas;
	u32 acks;
};

struct control_state {
	int deferred_size;
	struct sockaddr_in deferred_from;
	u8 deferred_bytes[64];
};

struct live_stream {
	struct stream_buffers buffers;
	struct row_state row;
	struct control_state control;
	struct stream_runtime runtime;
	struct stream_settings settings;
	struct stream_counters counters;
	struct live_net net;
	struct live_audio_sender audio;
	struct live_r20 r20;
	struct live_repair_tx repair_tx;
	LiveRepairHistory repair_history;
	LivePendingQueue pending;
};

extern struct live_stream stream;

bool live_stream_stopped(void);
bool live_stream_capture_paused(void);
void live_stream_discard_pending(unsigned slot, LiveTimingOutcome why);
u32 live_stream_pair_budget(u32 byte_rate, u32 hz, bool combined,
			    unsigned target_q);
void live_stream_configure(u32 hz, u32 bps, bool fec, unsigned target_q);

#endif /* LIVE_STREAM_INTERNAL_H */
