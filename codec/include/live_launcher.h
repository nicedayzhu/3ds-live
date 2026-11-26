#ifndef LIVE_LAUNCHER_H
#define LIVE_LAUNCHER_H

/* Application-private SRV notifications shared by the launcher and the resident
 * module. Closing the launcher changes the idle indication only. */
#define LIVE_LAUNCHER_OPENED 0x334C4501u
#define LIVE_LAUNCHER_CLOSED 0x334C4502u
#define LIVE_EXIT_IF_IDLE 0x334C4503u

#endif
