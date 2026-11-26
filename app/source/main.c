#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "launcher_module.h"
#include "launcher_ui.h"
#include "live_launcher.h"
#include "module_id.h"
#include "module_install.h"

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
	struct launcher_module_watch watch = {0};
	LauncherView previous = view;
	view.page = launcher_module_start(&watch, &view.detail);
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
			launcher_module_update(&watch, &view, now);
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
	if (launcher_module_running(watch.announced_pid))
		srvPublishToSubscriber(LIVE_LAUNCHER_CLOSED, 0);
	aptUnhook(&hook);
	launcher_device_exit();
	gfxExit();
	return 0;
}
