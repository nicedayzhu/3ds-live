#ifndef LAUNCHER_DEVICE_H
#define LAUNCHER_DEVICE_H

/* Unavailable readings are -1. Battery is a hardware level, not a percentage.
 */
typedef struct {
	int wifi;     /* 0 disconnected, 1 connected */
	int strength; /* 0..3 bars when connected */
	int battery;  /* 0..5 */
	int charging; /* 0 no, 1 yes */
} LauncherDevice;

void launcher_device_init(void);
LauncherDevice launcher_device_read(void);
void launcher_device_exit(void);

#endif
