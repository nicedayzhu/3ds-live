/*
 * Host tests for the media/control wire parser.  No libctru state is needed:
 * the ops table records what live_stream would apply.
 */
#include "live_control.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct capture *ctx;

struct capture {
	struct live_control_config config;
	unsigned config_calls;
	uint32_t mask;
	unsigned mask_calls;
	unsigned pacing;
	unsigned pacing_calls;
	unsigned ack_r20;
	unsigned repair_peer;
	unsigned repair_requests;
	unsigned row_flags;
	uint32_t row_sequence;
	unsigned row_acks;
	uint32_t feedback_sent;
	uint32_t feedback_transit;
	unsigned feedback_calls;
};

static void on_configure(const struct live_control_config *config, uint64_t now)
{
	struct capture *c = ctx;
	(void)now;

	c->config = *config;
	c->config_calls++;
}

static void on_mask(uint32_t mask, uint64_t now)
{
	struct capture *c = ctx;
	(void)now;

	c->mask = mask;
	c->mask_calls++;
}

static void on_pacing(unsigned mode, uint64_t now)
{
	struct capture *c = ctx;
	(void)now;

	c->pacing = mode;
	c->pacing_calls++;
}

static void on_r20(uint64_t now)
{
	struct capture *c = ctx;
	(void)now;

	c->ack_r20++;
}

static void on_repair_peer(uint64_t now)
{
	struct capture *c = ctx;
	(void)now;

	c->repair_peer++;
}

static void on_repair_request(const uint8_t *msg, int size, uint64_t now)
{
	struct capture *c = ctx;
	(void)now;

	(void)msg;
	(void)size;
	(void)now;
	c->repair_requests++;
}

static void on_rows(uint32_t flags, uint32_t sequence, uint64_t now)
{
	struct capture *c = ctx;
	(void)now;

	c->row_flags = flags;
	c->row_sequence = sequence;
	c->row_acks++;
}

static void on_feedback(uint32_t sent, uint32_t transit, uint64_t now)
{
	struct capture *c = ctx;
	(void)now;

	c->feedback_sent = sent;
	c->feedback_transit = transit;
	c->feedback_calls++;
}

static const struct live_control_ops ops = {
    .configure = on_configure,
    .set_mask = on_mask,
    .set_pacing = on_pacing,
    .ack_r20 = on_r20,
    .repair_peer = on_repair_peer,
    .repair_request = on_repair_request,
    .ack_rows = on_rows,
    .feedback = on_feedback,
};

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void test_config(void)
{
	struct capture c = {0};
	uint8_t msg[32] = "R16C";
	const uint64_t now = 1234;

	ctx = &c;

	put32(msg + 4, 0x12345678);
	put32(msg + 8, 7);
	put32(msg + 12, 24);
	put32(msg + 16, 1500000);
	put32(msg + 20, 1);
	msg[24] = 40;
	put32(msg + 28, 2);

	assert(live_control_handle(msg, sizeof(msg), now, 3, true, 60, &ops));
	assert(c.config_calls == 1);
	assert(c.config.sequence == 7);
	assert(c.config.fps == 24);
	assert(c.config.rate == 1500000);
	assert(c.config.combined_fec);
	assert(c.config.quality == 40);
	assert(c.config.mask == 2);
}

static void test_invalid_config_is_consumed(void)
{
	struct capture c = {0};
	uint8_t msg[32] = "R16C";
	const uint64_t now = 1234;

	ctx = &c;

	put32(msg + 8, 7);
	put32(msg + 12, 35); /* not a supported capture rate */
	put32(msg + 16, 2000000);
	put32(msg + 20, 1);
	msg[24] = 40;
	put32(msg + 28, 3);

	assert(live_control_handle(msg, sizeof(msg), now, 3, true, 60, &ops));
	assert(c.config_calls == 0);
}

static void test_mask_and_pacing(void)
{
	struct capture c = {0};
	uint8_t mask[12] = "R23C";
	uint8_t pacing[16] = "R21C";
	const uint64_t now = 1234;

	ctx = &c;

	put32(mask + 8, 1);
	assert(live_control_handle(mask, sizeof(mask), now, 3, true, 60, &ops));
	assert(c.mask_calls == 1);
	assert(c.mask == 1);

	put32(pacing + 8, 1);
	put32(pacing + 12, 1);
	assert(live_control_handle(pacing, sizeof(pacing), now, 3, true, 60,
				   &ops));
	assert(c.pacing_calls == 1);
	assert(c.pacing == 1);
}

static void test_rows_repair_and_feedback(void)
{
	struct capture c = {0};
	uint8_t rows[16] = "R17A";
	uint8_t repair[16] = "R19N";
	uint8_t feedback[28] = "R3FB";
	const uint64_t now = 1234;

	ctx = &c;

	put32(rows + 8, 1);
	put32(rows + 12, 0x10203040);
	assert(live_control_handle(rows, sizeof(rows), now, 3, true, 60, &ops));
	assert(c.row_acks == 1);
	assert(c.row_flags == 1);
	assert(c.row_sequence == 0x10203040);

	assert(live_control_handle(repair, sizeof(repair), now, 3, true, 60,
				   &ops));
	assert(c.repair_requests == 1);

	put32(feedback + 8, 11);
	put32(feedback + 16, 22);
	assert(live_control_handle(feedback, sizeof(feedback), now, 3, true, 60,
				   &ops));
	assert(c.feedback_calls == 1);
	assert(c.feedback_sent == 11);
	assert(c.feedback_transit == 22);
}

static void test_unknown_tag(void)
{
	struct capture c = {0};
	const uint8_t msg[12] = "NOPE";
	const uint64_t now = 1234;

	ctx = &c;

	assert(!live_control_handle(msg, sizeof(msg), now, 3, true, 60, &ops));
	assert(c.config_calls == 0 && c.mask_calls == 0);
}

int main(void)
{
	test_config();
	test_invalid_config_is_consumed();
	test_mask_and_pacing();
	test_rows_repair_and_feedback();
	test_unknown_tag();
	puts("test_live_control: ok");
	return 0;
}
