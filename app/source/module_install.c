#if !defined(_WIN32) && !defined(__3DS__)
#define _POSIX_C_SOURCE 200809L
#endif
#include "module_install.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

typedef enum { FILE_ERROR = -1, FILE_MISSING, FILE_MODULE, FILE_OTHER } ModuleFile;
typedef enum { READ_ERROR = -1, DIFFERENT, IDENTICAL, PREFIX } Comparison;

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Check the NCCH ID and media-unit bounds before replacing any installed file.
 * These plaintext system modules are made with a 512-byte media unit. */
static ModuleFile inspect_module(const char *path, bool allow_partial)
{
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT ? FILE_MISSING : FILE_ERROR;
    if (!S_ISREG(st.st_mode)) return FILE_OTHER;
    FILE *in = fopen(path, "rb");
    if (!in) return FILE_ERROR;
    unsigned char h[0x408];
    size_t got = fread(h, 1, sizeof(h), in);
    bool error = ferror(in);
    fclose(in);
    if (error) return FILE_ERROR;
    if (got < 0x200 || memcmp(h + 0x100, "NCCH", 4) != 0) return FILE_OTHER;
    const uint64_t id = read_le32(h + 0x118) | ((uint64_t)read_le32(h + 0x11c) << 32);
    const uint32_t units = read_le32(h + 0x104);
    const uint32_t exefs = read_le32(h + 0x1a0), exefs_size = read_le32(h + 0x1a4);
    if (id != LIVE_MODULE_ID || h[0x18e] != 0 || !(h[0x18d] & 2) || !(h[0x18f] & 4) ||
        (got == sizeof(h) && memcmp(h + 0x118, h + 0x400, 8) != 0) ||
        read_le32(h + 0x180) != 0x400 || exefs < 5 || exefs >= units ||
        exefs_size == 0 || exefs_size > units - exefs) return FILE_OTHER;
    const uint64_t declared = (uint64_t)units * 512;
    const uint64_t actual = (uint64_t)st.st_size;
    return actual == declared || (allow_partial && actual < declared) ? FILE_MODULE : FILE_OTHER;
}

/* PREFIX means that the right-hand file ended before the left-hand file.
 * A partially written staging file can be safely recognized on the next run. */
static Comparison compare_files(const char *left, const char *right)
{
    FILE *a = fopen(left, "rb");
    if (!a) return READ_ERROR;
    FILE *b = fopen(right, "rb");
    Comparison result = READ_ERROR;
    if (b) {
        unsigned char x[2048], y[2048];
        for (;;) {
            size_t nx = fread(x, 1, sizeof(x), a), ny = fread(y, 1, sizeof(y), b);
            if (ferror(a) || ferror(b)) break;
            if (ny > nx || memcmp(x, y, ny) != 0) { result = DIFFERENT; break; }
            if (ny < nx) { result = PREFIX; break; }
            if (nx < sizeof(x)) { result = IDENTICAL; break; }
        }
        fclose(b);
    }
    fclose(a);
    return result;
}

static bool copy_module(const char *source, const char *staged)
{
    FILE *in = fopen(source, "rb");
    if (!in) return false;
    FILE *out = fopen(staged, "wb");
    bool ok = out != NULL;
    if (out) {
        unsigned char data[4096];
        size_t n;
        while ((n = fread(data, 1, sizeof(data), in)) != 0) {
            if (fwrite(data, 1, n, out) != n) { ok = false; break; }
        }
        if (ferror(in) || fflush(out) != 0) ok = false;
#ifdef _WIN32
        if (ok && _commit(_fileno(out)) != 0) ok = false;
#else
        if (ok && fsync(fileno(out)) != 0) ok = false;
#endif
        if (fclose(out) != 0) ok = false;
    }
    fclose(in);
    return ok;
}

static bool make_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        errno = ENOTDIR;
        return false;
    }
    if (errno != ENOENT) return false;
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

static ModuleInstallResult conflict(void)
{
    errno = EEXIST;
    return MODULE_TARGET_CONFLICT;
}

static ModuleInstallResult install(const char *source, const char *target,
                                   const char *stage, const char *previous)
{
    ModuleFile current = inspect_module(target, false);
    if (current == FILE_ERROR) return MODULE_INSTALL_ERROR;
    if (current == FILE_OTHER) return conflict();
    bool restored = false;
    if (current == FILE_MISSING) {
        ModuleFile backup = inspect_module(previous, false);
        if (backup == FILE_ERROR) return MODULE_INSTALL_ERROR;
        if (backup == FILE_OTHER) return conflict();
        if (backup == FILE_MODULE) {
            /* Power loss between moving the old module aside and activating the new one. */
            if (rename(previous, target) != 0) return MODULE_INSTALL_ERROR;
            current = FILE_MODULE;
            restored = true;
        }
    }
    if (current == FILE_MODULE) {
        Comparison comparison = compare_files(source, target);
        if (comparison == READ_ERROR) return MODULE_INSTALL_ERROR;
        if (comparison == IDENTICAL) return restored ? MODULE_INSTALLED : MODULE_CURRENT;
    }

    ModuleFile scratch = inspect_module(stage, true);
    if (scratch == FILE_ERROR) return MODULE_INSTALL_ERROR;
    Comparison staged = DIFFERENT;
    if (scratch != FILE_MISSING) {
        staged = compare_files(source, stage);
        if (staged == READ_ERROR) return MODULE_INSTALL_ERROR;
        if (scratch == FILE_OTHER && staged != PREFIX) return conflict();
    }
    if (staged != IDENTICAL) {
        if (!copy_module(source, stage)) return MODULE_INSTALL_ERROR;
        if (compare_files(source, stage) != IDENTICAL) { errno = EIO; return MODULE_INSTALL_ERROR; }
    }

    if (current == FILE_MODULE) {
        ModuleFile backup = inspect_module(previous, false);
        if (backup == FILE_ERROR) return MODULE_INSTALL_ERROR;
        if (backup == FILE_OTHER) return conflict();
        if (backup == FILE_MODULE && remove(previous) != 0) return MODULE_INSTALL_ERROR;
        if (rename(target, previous) != 0) return MODULE_INSTALL_ERROR;
    }
    if (rename(stage, target) != 0) {
        int failure = errno;
        if (current == FILE_MODULE && rename(previous, target) != 0) return MODULE_INSTALL_ERROR;
        errno = failure;
        return MODULE_INSTALL_ERROR;
    }
    if (compare_files(source, target) != IDENTICAL) {
        /* Keep the backup if either rollback rename fails. */
        if (rename(target, stage) != 0) return MODULE_INSTALL_ERROR;
        if (current == FILE_MODULE && rename(previous, target) != 0) return MODULE_INSTALL_ERROR;
        errno = EIO;
        return MODULE_INSTALL_ERROR;
    }
    return MODULE_INSTALLED;
}

ModuleInstallResult module_install(const char *bundled_cxi, const char *sd_root)
{
    ModuleFile bundle = inspect_module(bundled_cxi, false);
    if (bundle != FILE_MODULE) {
        if (bundle == FILE_OTHER) errno = EINVAL;
        return MODULE_INVALID_BUNDLE;
    }
    /* The foreground launcher is the sole writer of these three paths. */
    const size_t root_len = strlen(sd_root);
    const size_t room = root_len + sizeof("/" LIVE_MODULE_SD_PATH ".installing");
    char *target = malloc(room * 3);
    if (!target) return MODULE_INSTALL_ERROR;
    char *stage = target + room, *previous = stage + room;
    const char *slash = root_len && sd_root[root_len-1] == '/' ? "" : "/";
    snprintf(target, room, "%s%sluma", sd_root, slash);
    ModuleInstallResult result = MODULE_INSTALL_ERROR;
    if (make_dir(target)) {
        strcat(target, "/sysmodules");
        if (make_dir(target)) {
            snprintf(target, room, "%s%s%s", sd_root, slash, LIVE_MODULE_SD_PATH);
            strcpy(stage, target);
            strcat(stage, ".installing");
            strcpy(previous, target);
            strcat(previous, ".previous");
            result = install(bundled_cxi, target, stage, previous);
        }
    }
    free(target);
    return result;
}
