#include "module_install.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <utime.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define new_dir(path) _mkdir(path)
#define chdir _chdir
#define rmdir _rmdir
#define getpid _getpid
#else
#include <unistd.h>
#define new_dir(path) mkdir(path, 0700)
#endif

#define TARGET LIVE_MODULE_SD_PATH
#define STAGE TARGET ".installing"
#define BACKUP TARGET ".previous"
#define SOURCE "embedded.cxi"
#define OTHER "my-settings.ini"
#define SIZE 6144

static void write_file(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(data, 1, len, f) == len && fclose(f) == 0);
}

static void check_file(const char *path, const unsigned char *data, size_t len)
{
    unsigned char actual[SIZE];
    assert(len <= sizeof(actual));
    FILE *f = fopen(path, "rb");
    assert(f && fread(actual, 1, len, f) == len && memcmp(actual, data, len) == 0);
    assert(fgetc(f) == EOF && !ferror(f) && fclose(f) == 0);
}

static ModuleInstallResult run_install(void)
{
    return module_install(SOURCE, "./");
}

int main(void)
{
    char directory[64];
    snprintf(directory, sizeof(directory), "module-install-test-%ld", (long)getpid());
    assert(new_dir(directory) == 0 && chdir(directory) == 0);

    unsigned char old[SIZE] = {0}, next[SIZE];
    memcpy(old + 0x100, "NCCH", 4);
    old[0x104] = SIZE / 512;
    for (unsigned i = 0; i < 8; ++i)
        old[0x400 + i] = old[0x118 + i] = (unsigned char)(LIVE_MODULE_ID >> (i * 8));
    old[0x181] = 4;
    old[0x18d] = 2;
    old[0x18f] = 4;
    old[0x1a0] = 5;
    old[0x1a4] = SIZE / 512 - 5;
    memcpy(next, old, SIZE);
    next[SIZE - 1] = 1; /* Same headers and size, different final byte. */
    const unsigned char personal[] = "this file belongs to the user";
    write_file(OTHER, personal, sizeof(personal));

    /* Invalid source cannot even create the sysmodule directory. */
    struct stat st;
    write_file(SOURCE, old, SIZE - 1);
    assert(run_install() == MODULE_INVALID_BUNDLE && stat("luma", &st) != 0);
    next[0x400] ^= 1;
    write_file(SOURCE, next, SIZE);
    assert(run_install() == MODULE_INVALID_BUNDLE && stat("luma", &st) != 0);
    next[0x400] ^= 1;

    write_file(SOURCE, old, SIZE);
    assert(run_install() == MODULE_INSTALLED);
    check_file(TARGET, old, SIZE);
    assert(stat(BACKUP, &st) != 0);
    struct utimbuf times = { 1000000000, 1000000000 };
    assert(utime(TARGET, &times) == 0 && stat(TARGET, &st) == 0);
    time_t mtime = st.st_mtime;
    assert(run_install() == MODULE_CURRENT && stat(TARGET, &st) == 0 && st.st_mtime == mtime);

    /* Resume a partial stage and preserve the prior module. */
    write_file(SOURCE, next, SIZE);
    write_file(STAGE, next, 127);
    assert(run_install() == MODULE_INSTALLED && stat(STAGE, &st) != 0);
    check_file(TARGET, next, SIZE);
    check_file(BACKUP, old, SIZE);
    assert(run_install() == MODULE_CURRENT);

    /* Simulate loss of power after target -> backup, before stage -> target. */
    assert(remove(BACKUP) == 0 && rename(TARGET, BACKUP) == 0);
    write_file(SOURCE, old, SIZE);
    write_file(STAGE, old, SIZE);
    assert(run_install() == MODULE_INSTALLED);
    check_file(TARGET, old, SIZE);
    check_file(BACKUP, next, SIZE);

    /* Unrecognized occupants of the transaction paths are left intact. */
    write_file(SOURCE, next, SIZE);
    write_file(STAGE, personal, sizeof(personal));
    assert(run_install() == MODULE_TARGET_CONFLICT);
    check_file(STAGE, personal, sizeof(personal));
    assert(remove(STAGE) == 0);
    write_file(BACKUP, personal, sizeof(personal));
    assert(run_install() == MODULE_TARGET_CONFLICT);
    check_file(TARGET, old, SIZE);
    check_file(BACKUP, personal, sizeof(personal));

    /* A structurally valid CXI for another title must also be preserved. */
    old[0x118] ^= 1;
    old[0x400] ^= 1;
    write_file(TARGET, old, SIZE);
    assert(run_install() == MODULE_TARGET_CONFLICT);
    check_file(TARGET, old, SIZE);
    check_file(OTHER, personal, sizeof(personal));

    assert(remove(SOURCE) == 0 && remove(TARGET) == 0 && remove(BACKUP) == 0);
    assert(remove(STAGE) == 0 && remove(OTHER) == 0);
    assert(rmdir("luma/sysmodules") == 0 && rmdir("luma") == 0);
    assert(chdir("..") == 0 && rmdir(directory) == 0);
    puts("Module installation tests passed");
    return 0;
}
