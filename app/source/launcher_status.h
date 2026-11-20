#ifndef LAUNCHER_STATUS_H
#define LAUNCHER_STATUS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LAUNCHER_READY,
    LAUNCHER_STOPPED,
    LAUNCHER_SEARCHING,
    LAUNCHER_PAIRED,
    LAUNCHER_RECEIVING,
    LAUNCHER_SLEEP,
    LAUNCHER_ERROR
} LauncherState;

typedef struct {
    LauncherState state;
    uint32_t result;
    uint32_t led_result;
} LauncherStatus;

/* A failed or partial read leaves out untouched. active_pid is the live module PID. */
bool launcher_read_status(const char *path, uint32_t active_pid, const char *expected_build,
                          LauncherStatus *out);

#endif
