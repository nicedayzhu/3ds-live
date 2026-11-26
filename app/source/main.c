#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "launcher_ui.h"
#include "live_launcher.h"
#include "module_id.h"
#include "module_install.h"

/* Only inspect the title ID. The streaming hotkey is owned by the module. */
static bool is_module_process(u32 pid)
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
		if (is_module_process(ids[i]))
			return ids[i];
	return 0;
}

typedef enum { UNCONFIRMED, CURRENT_BUILD, OLD_IDLE, OLD_BUSY } ResidentModule;
static const char status_path[] = "sdmc:/3ds/3ds-live/status.txt";

static ResidentModule resident_status(u32 pid)
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

typedef struct {
	bool enabled;
	u32 pid, announced_pid;
	u64 lookup_at, last_valid;
} ModuleWatch;

static LauncherPage start_live(ModuleWatch *watch, uint32_t *detail)
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
		ResidentModule resident = resident_status(pid);
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
				if (!is_module_process(pid))
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

static void update_module_view(ModuleWatch *watch, LauncherView *view, u64 now)
{
	if (watch->pid && !is_module_process(watch->pid)) {
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

static bool resumed;

static void on_applet_event(APT_HookType event, void *unused)
{
	(void)unused;
	if (event == APTHOOK_ONRESTORE || event == APTHOOK_ONWAKEUP)
		__atomic_store_n(&resumed, true, __ATOMIC_RELEASE);
}

static LauncherLanguage system_language(void)
{
	if (R_FAILED(cfguInit()))
		return LAUNCHER_LANG_EN;
	u8 language = CFG_LANGUAGE_EN;
	Result rc = CFGU_GetSystemLanguage(&language);
	cfguExit();
	return R_SUCCEEDED(rc) && (language == CFG_LANGUAGE_ZH ||
				   language == CFG_LANGUAGE_TW)
		   ? LAUNCHER_LANG_ZH
		   : LAUNCHER_LANG_EN;
}

static const char language_path[] = "sdmc:/3ds/3ds-live/launcher-language.txt";

int main(void)
{
	gfxInitDefault();
	const LauncherLanguage system = system_language();
	LauncherView view = {.page = UI_STARTING,
			     .device = {-1, -1, -1, -1},
			     .language =
				 launcher_language_load(language_path, system)};
	launcher_ui_draw(&view, NULL);
	gspWaitForVBlank();
	aptHookCookie hook;
	aptHook(&hook, on_applet_event, NULL);
	ModuleWatch watch = {0};
	LauncherView previous = view;
	view.page = start_live(&watch, &view.detail);
	launcher_device_init();
	u64 next_module = 0, next_device = 0;
	while (aptMainLoop()) {
		hidScanInput();
		const u32 keys = hidKeysDown();
		if (keys & KEY_START)
			break;
		const bool switch_language = (keys & KEY_X) != 0;
		if (switch_language)
			view.language = view.language == LAUNCHER_LANG_ZH
					    ? LAUNCHER_LANG_EN
					    : LAUNCHER_LANG_ZH;
		const u64 now = svcGetSystemTick();
		const bool restore =
		    __atomic_exchange_n(&resumed, false, __ATOMIC_ACQ_REL);
		const bool sample_module =
		    watch.enabled && (restore || now >= next_module);
		const bool sample_device = restore || now >= next_device;
		if (sample_module) {
			update_module_view(&watch, &view, now);
			next_module = now + SYSCLOCK_ARM11 / 2;
		}
		if (sample_device) {
			view.device = launcher_device_read();
			next_device = now + 2ull * SYSCLOCK_ARM11;
		}
		if (switch_language || sample_module || sample_device) {
			launcher_ui_draw(&view, restore ? NULL : &previous);
			previous = view;
		}
		if (switch_language &&
		    !launcher_language_save(language_path, view.language,
					    system) &&
		    errno == ENOENT) {
			// A CIA can be started before its SD data directory
			// exists.
			mkdir("sdmc:/3ds", 0777);
			mkdir("sdmc:/3ds/3ds-live", 0777);
			launcher_language_save(language_path, view.language,
					       system);
		}
		gspWaitForVBlank();
	}
	if (is_module_process(watch.announced_pid))
		srvPublishToSubscriber(LIVE_LAUNCHER_CLOSED, 0);
	aptUnhook(&hook);
	launcher_device_exit();
	gfxExit();
	return 0;
}
