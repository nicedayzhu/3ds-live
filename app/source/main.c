#include <3ds.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LIVE_MODULE_ID UINT64_C(0x0004013000F1D402)
#define LIVE_MODULE_FILE "sdmc:/luma/sysmodules/0004013000F1D402.cxi"

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

static bool module_reported_ready(u32 pid)
{
    FILE *status = fopen("sdmc:/3ds/3ds-live/status.txt", "r");
    if (!status) return false;
    char line[64];
    unsigned long reported = 0;
    bool ready = false;
    while (fgets(line, sizeof line, status)) {
        if (strncmp(line, "pid=", 4) == 0) sscanf(line + 4, "%lu", &reported);
        if (strncmp(line, "state=idle", 10) == 0) ready = true;
    }
    fclose(status);
    return ready && reported == pid;
}

static void start_live(void)
{
    bool new_model = false;
    if (R_FAILED(APT_CheckNew3DS(&new_model)) || !new_model) {
        puts("3DS Live needs a New 3DS or New 2DS.");
        return;
    }
    if (running_pid()) {
        puts("3DS Live is already running.");
        return;
    }
    if (access(LIVE_MODULE_FILE, R_OK) != 0) {
        puts("Streaming module missing. Copy the\ncontents of SD/ to your SD card.");
        return;
    }
    /* ns:s is included in Luma's Homebrew Launcher service list. */
    Result rc = nsInit();
    u32 pid = 0;
    if (R_SUCCEEDED(rc)) {
        rc = NS_LaunchTitle(LIVE_MODULE_ID, PMLAUNCHFLAG_LOAD_DEPENDENCIES, &pid);
        nsExit();
    }
    if (R_FAILED(rc)) {
        printf("Could not start 3DS Live (%08" PRIX32 ").\n"
               "Enable loading external FIRMs and\nmodules in Luma, then reboot.\n", (uint32_t)rc);
        return;
    }
    for (int retry = 0; retry < 30; ++retry) {
        svcSleepThread(100000000);
        u32 actual = running_pid();
        if (actual && (!pid || pid == actual) && module_reported_ready(actual)) {
            puts("3DS Live is ready.");
            return;
        }
    }
    puts("Launch requested. Read the status file\nin /3ds/3ds-live/ if streaming fails.");
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\x1b[1;1H3DS Live\n\n");
    start_live();
    puts("\nOpen 3DS-Live.exe on your computer.\n"
         "Use the same LAN on both devices.\n\n"
         "When ready, press R+SELECT here to stream.\n"
         "Press R+SELECT again to stop.\n"
         "START closes this launcher to let you play.");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
