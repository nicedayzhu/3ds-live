#include "launcher_language.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

LauncherLanguage launcher_language_load(const char *path, LauncherLanguage system_language)
{
    FILE *file = fopen(path, "rb");
    if (!file) return system_language;
    char bytes[5];  /* Two-letter code plus optional LF or CRLF; reject anything longer. */
    size_t size = fread(bytes, 1, sizeof bytes, file);
    const bool complete = feof(file) && !ferror(file);
    fclose(file);
    if (!complete) return system_language;
    if (size && bytes[size-1] == '\n') {
        --size;
        if (size && bytes[size-1] == '\r') --size;
    }
    if (size == 2 && memcmp(bytes, "en", 2) == 0) return LAUNCHER_LANG_EN;
    if (size == 2 && memcmp(bytes, "zh", 2) == 0) return LAUNCHER_LANG_ZH;
    return system_language;
}

bool launcher_language_save(const char *path, LauncherLanguage language, LauncherLanguage system_language)
{
    if (language == system_language) return remove(path) == 0 || errno == ENOENT;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    const bool wrote = fputs(language == LAUNCHER_LANG_ZH ? "zh\n" : "en\n", file) >= 0;
    const bool closed = fclose(file) == 0;
    return wrote && closed;
}
