/*
 * Single-consumer writer for network-status.txt.  The socket worker only
 * publishes a formatted snapshot; the status worker does all file I/O so a
 * slow SD card can never stall media sending.
 */
#include "live_net_report.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define LIVE_NET_REPORT_PATH "sdmc:/3ds/3ds-live/network-status.txt"

static LightLock report_lock;
static char pending_report[LIVE_NET_REPORT_BYTES];
static uint32_t pending_length;
static uint32_t pending_version;
static uint64_t retry_at;

void live_net_report_init(void)
{
	LightLock_Init(&report_lock);
}

void live_net_report_queue(const struct live_net_report *snapshot)
{
	char text[LIVE_NET_REPORT_BYTES];
	size_t length;

	if (!snapshot)
		return;

	length = live_net_report_format(text, sizeof(text), snapshot);
	if (!length)
		return;

	LightLock_Lock(&report_lock);
	memcpy(pending_report, text, length);
	pending_version++;
	__atomic_store_n(&pending_length, (uint32_t)length, __ATOMIC_RELEASE);
	LightLock_Unlock(&report_lock);
}

void live_net_report_flush(bool force)
{
	char text[LIVE_NET_REPORT_BYTES];
	uint32_t length, version;
	FILE *file;
	bool ok;

	if (!__atomic_load_n(&pending_length, __ATOMIC_ACQUIRE))
		return;
	if (!force && retry_at && svcGetSystemTick() < retry_at)
		return;

	LightLock_Lock(&report_lock);
	length = __atomic_load_n(&pending_length, __ATOMIC_RELAXED);
	version = pending_version;
	memcpy(text, pending_report, length);
	LightLock_Unlock(&report_lock);

	file = fopen(LIVE_NET_REPORT_PATH, "w");
	ok = false;
	if (file) {
		size_t written = fwrite(text, 1, length, file);

		ok = fclose(file) == 0 && written == length;
	}
	if (!ok) {
		retry_at = svcGetSystemTick() + SYSCLOCK_ARM11;
		return;
	}

	retry_at = 0;
	LightLock_Lock(&report_lock);
	if (version == pending_version)
		__atomic_store_n(&pending_length, 0, __ATOMIC_RELEASE);
	LightLock_Unlock(&report_lock);
}
