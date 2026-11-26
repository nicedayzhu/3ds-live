/*
 * Launcher-side module discovery, installation and status watching.
 */
#ifndef LAUNCHER_MODULE_H
#define LAUNCHER_MODULE_H

#include "launcher_ui.h"

#include <3ds.h>

struct launcher_module_watch {
	bool enabled;
	u32 pid;
	u32 announced_pid;
	u64 lookup_at;
	u64 last_valid;
};

LauncherPage launcher_module_start(struct launcher_module_watch *watch,
				   uint32_t *detail);
void launcher_module_update(struct launcher_module_watch *watch,
			    LauncherView *view, u64 now);
bool launcher_module_running(u32 pid);

#endif /* LAUNCHER_MODULE_H */
