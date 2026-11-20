#ifndef LAUNCHER_TEXT_H
#define LAUNCHER_TEXT_H

#include "launcher_ui.h"

typedef struct {
    const char *title, *description, *key, *action;
} LauncherStateText;

typedef struct {
    LauncherStateText live[LAUNCHER_ERROR + 1];
    LauncherStateText startup[UI_MODEL_QUERY + 1];
    const char *status, *device, *switch_to;
    const char *battery, *battery_unknown, *charging;
    const char *wifi, *connected, *disconnected, *unknown;
    const char *led, *led_blue, *led_amber, *led_green, *led_red, *led_off, *led_failed;
    const char *error_code, *close, *in_game;
} LauncherText;

extern const LauncherText launcher_text[];
#endif
