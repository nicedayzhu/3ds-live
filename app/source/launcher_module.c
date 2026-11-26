/*
 * Module discovery, installation and status polling for the launcher.
 */
#include "launcher_module.h"

#include "live_launcher.h"
#include "module_id.h"
#include "module_install.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Only inspect the title ID. The streaming hotkey is owned by the module. */
bool launcher_module_running(u32 pid)
{
	Handle process;
	if (!pid || R_FAILED(svcOpenProcess(&process, pid)))
		return false;
	s64 title = 0;
	Result rc = svcGetProcessInfo(&title, process, 0x10001);
	svcCloseHandle(process);
	return R_SUCCEEDED(rc) && (u64)title == LIVE_MODULE_ID;
}

static u32 running_pid(void)
{
	u32 ids[64];
	s32 count = 0;
	if (R_FAILED(svcGetProcessList(&count, ids, 64)))
		return 0;
	for (s32 i = 0; i < count && i < 64; ++i)
		if (launcher_module_running(ids[i]))
			return ids[i];
	return 0;
}

enum resident_module { UNCONFIRMED, CURRENT_BUILD, OLD_IDLE, OLD_BUSY };
static const char status_path[] = "sdmc:/3ds/3ds-live/status.txt";

static enum resident_module resident_status(u32 pid)
{
	FILE *status = fopen(status_path, "r");
	if (!status)
		return UNCONFIRMED;
	char line[96];
	unsigned long reported = 0;
	bool matches = false, active = false, idle = false, can_exit = false;
	while (fgets(line, sizeof line, status)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "pid=", 4) == 0)
			sscanf(line + 4, "%lu", &reported);
		if (strcmp(line, "build=" LIVE_MODULE_BUILD_ID) == 0)
			matches = true;
		if (strcmp(line, "exit_if_idle=1") == 0)
			can_exit = true;
		if (strncmp(line, "state=", 6) == 0) {
			idle = strcmp(line + 6, "idle") == 0 ||
			       strcmp(line + 6, "error") == 0;
			active = idle || strcmp(line + 6, "streaming") == 0 ||
				 strcmp(line + 6, "searching") == 0 ||
				 strcmp(line + 6, "sleep") == 0;
		}
	}
	fclose(status);
	if (reported != pid || !active)
		return UNCONFIRMED;
	if (matches)
		return CURRENT_BUILD;
	if (!can_exit)
		return UNCONFIRMED;
	return idle ? OLD_IDLE : OLD_BUSY;
}

LauncherPage launcher_module_start(struct launcher_module_watch *watch,
				   uint32_t *detail)
{
	bool new_model = false;
	Result rc = APT_CheckNew3DS(&new_model);
	if (R_FAILED(rc)) {
		*detail = (u32)rc;
		return UI_MODEL_QUERY;
	}
	if (!new_model)
		return UI_OLD_MODEL;
	rc = romfsInit();
	if (R_FAILED(rc)) {
		*detail = (u32)rc;
		return UI_ROMFS;
	}
	ModuleInstallResult install =
	    module_install("romfs:/3ds-live.cxi", "sdmc:/");
	int install_errno = errno;
	romfsExit();
	if (install == MODULE_INVALID_BUNDLE)
		return UI_BUNDLE;
	if (install == MODULE_TARGET_CONFLICT)
		return UI_CONFLICT;
	if (install == MODULE_INSTALL_ERROR) {
		*detail = (u32)install_errno;
		return UI_INSTALL;
	}
	u32 pid = running_pid();
	if (pid) {
		// A status update briefly truncates the file. Allow the writer
		// to finish.
		enum resident_module resident = resident_status(pid);
		for (int retry = 0; resident == UNCONFIRMED && retry < 5;
		     ++retry) {
			svcSleepThread(50000000);
			resident = resident_status(pid);
		}
		if (resident == CURRENT_BUILD) {
			watch->enabled = true;
			watch->pid = pid;
			return UI_WAIT_STATUS;
		}
		if (resident == OLD_BUSY)
			return UI_OLD_BUSY;
		if (resident == OLD_IDLE &&
		    R_SUCCEEDED(srvPublishToSubscriber(LIVE_EXIT_IF_IDLE, 0))) {
			for (int retry = 0; pid && retry < 20; ++retry) {
				svcSleepThread(100000000);
				if (!launcher_module_running(pid))
					pid = running_pid();
			}
		}
		if (pid) {
			// A late status from the current build can still
			// confirm this process.
			watch->enabled = resident == UNCONFIRMED;
			watch->pid = pid;
			return UI_REBOOT;
		}
	}
	/* ns:s is included in Luma's Homebrew Launcher service list. */
	rc = nsInit();
	if (R_SUCCEEDED(rc)) {
		// On a new boot the OS can reuse a PID recorded on SD before
		// shutdown.
		watch->enabled = unlink(status_path) == 0 || errno == ENOENT;
		rc = NS_LaunchTitle(LIVE_MODULE_ID,
				    PMLAUNCHFLAG_LOAD_DEPENDENCIES, &pid);
		nsExit();
	}
	if (R_FAILED(rc)) {
		watch->enabled = false;
		*detail = (u32)rc;
		return UI_LAUNCH;
	}
	watch->pid = pid;
	return UI_WAIT_STATUS;
}

void launcher_module_update(struct launcher_module_watch *watch,
			    LauncherView *view, u64 now)
{
	if (watch->pid && !launcher_module_running(watch->pid)) {
		watch->pid = watch->announced_pid = 0;
		watch->last_valid = 0;
	}
	if (!watch->pid && now >= watch->lookup_at) {
		watch->pid = running_pid();
		watch->lookup_at = now + 2ull * SYSCLOCK_ARM11;
	}
	if (launcher_read_status(status_path, watch->pid, LIVE_MODULE_BUILD_ID,
				 &view->module)) {
		watch->last_valid = now;
		view->page = UI_LIVE;
		if (watch->announced_pid != watch->pid &&
		    R_SUCCEEDED(
			srvPublishToSubscriber(LIVE_LAUNCHER_OPENED, 0)))
			watch->announced_pid = watch->pid;
	} else if (view->page == UI_LIVE &&
		   (!watch->last_valid ||
		    now - watch->last_valid >= SYSCLOCK_ARM11)) {
		// A concurrent write can produce a short file for one poll. If
		// reports stay unreadable, stop displaying the previous live
		// state.
		view->page = UI_WAIT_STATUS;
	}
}
