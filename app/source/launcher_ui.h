#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

#include "launcher_device.h"
#include "launcher_status.h"

typedef enum {
    UI_STARTING, UI_WAIT_STATUS, UI_LIVE, UI_OLD_MODEL, UI_ROMFS, UI_BUNDLE,
    UI_CONFLICT, UI_INSTALL, UI_OLD_BUSY, UI_REBOOT, UI_LAUNCH, UI_MODEL_QUERY
} LauncherPage;

typedef struct {
    LauncherPage page;
    LauncherStatus module;
    LauncherDevice device;
    uint32_t detail;
} LauncherView;

/* Paint both native screens and present them together. Call after gfxInitDefault. */
void launcher_ui_draw(const LauncherView *view);

#endif
