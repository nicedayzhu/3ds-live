#include <3ds.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "live_launcher.h"
#include "module_id.h"
#include "module_install.h"

/* Only inspect the title ID. The streaming hotkey is owned by the module. */
static u32 running_pid(void)
{
    u32 ids[64];
    s32 count = 0;
    if (R_FAILED(svcGetProcessList(&count, ids, 64))) return 0;
    for (s32 i = 0; i < count && i < 64; ++i) {
        Handle process;
        if (R_FAILED(svcOpenProcess(&process, ids[i]))) continue;
        s64 title = 0;
        Result rc = svcGetProcessInfo(&title, process, 0x10001);
        svcCloseHandle(process);
        if (R_SUCCEEDED(rc) && (u64)title == LIVE_MODULE_ID) return ids[i];
    }
    return 0;
}

typedef enum { UNCONFIRMED, CURRENT_BUILD, OLD_IDLE, OLD_BUSY } ResidentModule;
static const char status_path[] = "sdmc:/3ds/3ds-live/status.txt";

static ResidentModule resident_status(u32 pid)
{
    FILE *status = fopen(status_path, "r");
    if (!status) return UNCONFIRMED;
    char line[96];
    unsigned long reported = 0;
    bool matches = false, active = false, idle = false, can_exit = false;
    while (fgets(line, sizeof line, status)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "pid=", 4) == 0) sscanf(line + 4, "%lu", &reported);
        if (strcmp(line, "build=" LIVE_MODULE_BUILD_ID) == 0) matches = true;
        if (strcmp(line, "exit_if_idle=1") == 0) can_exit = true;
        if (strncmp(line, "state=", 6) == 0) {
            idle = strcmp(line + 6, "idle") == 0 || strcmp(line + 6, "error") == 0;
            active = idle || strcmp(line + 6, "streaming") == 0 ||
                     strcmp(line + 6, "searching") == 0 || strcmp(line + 6, "sleep") == 0;
        }
    }
    fclose(status);
    if (reported != pid || !active) return UNCONFIRMED;
    if (matches) return CURRENT_BUILD;
    if (!can_exit) return UNCONFIRMED;
    return idle ? OLD_IDLE : OLD_BUSY;
}

static bool start_live(void)
{
    bool new_model = false;
    if (R_FAILED(APT_CheckNew3DS(&new_model)) || !new_model) {
        puts("3DS Live needs a New 3DS or New 2DS.");
        return false;
    }
    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        printf("Cannot open the launcher RomFS (%08" PRIX32 ").\n", (uint32_t)rc);
        return false;
    }
    ModuleInstallResult install = module_install("romfs:/3ds-live.cxi", "sdmc:/");
    int install_errno = errno;
    romfsExit();
    if (install == MODULE_INVALID_BUNDLE) {
        puts("Cannot read the bundled streaming module.\nCopy or reinstall this launcher.");
        return false;
    }
    if (install == MODULE_TARGET_CONFLICT) {
        puts("Unexpected file in /luma/sysmodules/.\nCheck the module and its backup on the SD:\n"
             LIVE_MODULE_SD_PATH);
        return false;
    }
    if (install == MODULE_INSTALL_ERROR) {
        printf("Cannot install the module on SD.\n%s\n", strerror(install_errno));
        return false;
    }
    if (install == MODULE_INSTALLED) puts("Streaming module installed on SD.");
    u32 pid = running_pid();
    if (pid) {
        // A status update briefly truncates the file. Allow the writer to finish.
        ResidentModule resident = resident_status(pid);
        for (int retry = 0; resident == UNCONFIRMED && retry < 5; ++retry) {
            svcSleepThread(50000000);
            resident = resident_status(pid);
        }
        if (resident == CURRENT_BUILD) {
            puts("3DS Live is already running.");
            return true;
        }
        if (resident == OLD_BUSY) {
            puts("The previous module is still active.\nStop streaming and reopen 3DS Live\nto use the installed update.");
            return false;
        }
        if (resident == OLD_IDLE && R_SUCCEEDED(srvPublishToSubscriber(LIVE_EXIT_IF_IDLE, 0))) {
            for (int retry = 0; pid && retry < 20; ++retry) {
                svcSleepThread(100000000);
                pid = running_pid();
            }
        }
        if (pid) {
            puts("To load the installed module, save your\ngame, reboot, then reopen 3DS Live.");
            return false;
        }
    }
    /* ns:s is included in Luma's Homebrew Launcher service list. */
    rc = nsInit();
    bool fresh_status = false;
    if (R_SUCCEEDED(rc)) {
        // On a new boot the OS can reuse a PID recorded on SD before shutdown.
        fresh_status = unlink(status_path) == 0 || errno == ENOENT;
        rc = NS_LaunchTitle(LIVE_MODULE_ID, PMLAUNCHFLAG_LOAD_DEPENDENCIES, &pid);
        nsExit();
    }
    if (R_FAILED(rc)) {
        printf("Could not start 3DS Live (%08" PRIX32 ").\n"
               "Enable loading external FIRMs and\nmodules in Luma, then reboot.\n", (uint32_t)rc);
        return false;
    }
    for (int retry = 0; fresh_status && retry < 30; ++retry) {
        svcSleepThread(100000000);
        u32 actual = running_pid();
        if (actual && (!pid || pid == actual) && resident_status(actual) == CURRENT_BUILD) {
            puts("3DS Live is ready.");
            return true;
        }
    }
    puts("Launch requested. Read the status file\nin /3ds/3ds-live/ if streaming fails.");
    return false;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\x1b[1;1H3DS Live\n\n");
    bool ready = start_live();
    if (ready) {
        srvPublishToSubscriber(LIVE_LAUNCHER_OPENED, 0);
        puts("\nOpen 3DS-Live.exe on your computer.\n"
             "Use the same LAN on both devices.\n\n"
             "When ready, press R+SELECT here to stream.\n"
             "Press R+SELECT again to stop.\n"
             "\nNotification LED\n"
             "Blue: ready      Amber pulse: waiting\n"
             "Green: PC receiving video   Off: stopped");
    }
    puts("\nSTART closes this launcher.");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    if (ready) srvPublishToSubscriber(LIVE_LAUNCHER_CLOSED, 0);
    gfxExit();
    return 0;
}
