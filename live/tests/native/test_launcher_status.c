#include "launcher_status.h"
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

static const char build[] = "abcdef";
static char path[64];

static void report(const char *state, const char *light, const char *tail)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "pid=99\r\nbuild=%s\r\nexit_if_idle=1\r\nstate=%s\r\n"
            "result=fffffffe\r\nled=%s\r\n%s", build, state, light, tail);
    assert(fclose(file) == 0);
}

int main(void)
{
    snprintf(path, sizeof path, "launcher-status-test-%ld.txt", (long)getpid());
    const struct { const char *state, *led; LauncherState expected; } cases[] = {
        {"idle", "ready", LAUNCHER_READY}, {"idle", "off", LAUNCHER_STOPPED},
        {"searching", "waiting", LAUNCHER_SEARCHING}, {"streaming", "waiting", LAUNCHER_PAIRED},
        {"streaming", "receiving", LAUNCHER_RECEIVING}, {"sleep", "off", LAUNCHER_SLEEP},
        {"error", "error", LAUNCHER_ERROR}, {"stopped", "off", LAUNCHER_STOPPED}
    };
    LauncherStatus status = {0};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        report(cases[i].state, cases[i].led, "led_result=00000000\r\n");
        assert(launcher_read_status(path, 99, build, &status));
        assert(status.state == cases[i].expected && status.result == UINT32_C(0xfffffffe));
        assert(status.led_result == 0);
    }
    const LauncherState previous = status.state;
    assert(!launcher_read_status(path, 98, build, &status));
    assert(!launcher_read_status(path, 99, "old", &status));
    assert(!launcher_read_status(path, 0, build, &status));
    report("streaming", "receiving", "led_result=0000");
    assert(!launcher_read_status(path, 99, build, &status));
    report("streaming", "receiving", "led_result=00000000\nstate=idle\n");
    assert(!launcher_read_status(path, 99, build, &status));
    report("streaming", "unknown", "led_result=00000000\n");
    assert(!launcher_read_status(path, 99, build, &status));
    assert(status.state == previous && status.result == UINT32_C(0xfffffffe));
    assert(remove(path) == 0);
    assert(!launcher_read_status(path, 99, build, &status));
    puts("Launcher status validation OK");
    return 0;
}
