#include "launcher_language.h"
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

int main(void)
{
    char path[64];
    snprintf(path, sizeof path, "launcher-language-test-%ld.txt", (long)getpid());
    assert(launcher_language_load(path, LAUNCHER_LANG_ZH) == LAUNCHER_LANG_ZH);
    assert(launcher_language_save(path, LAUNCHER_LANG_EN, LAUNCHER_LANG_ZH));
    assert(launcher_language_load(path, LAUNCHER_LANG_ZH) == LAUNCHER_LANG_EN);
    assert(launcher_language_save(path, LAUNCHER_LANG_ZH, LAUNCHER_LANG_ZH));
    assert(!fopen(path, "rb"));
    assert(launcher_language_load(path, LAUNCHER_LANG_EN) == LAUNCHER_LANG_EN);
    assert(launcher_language_save(path, LAUNCHER_LANG_ZH, LAUNCHER_LANG_EN));
    assert(launcher_language_load(path, LAUNCHER_LANG_EN) == LAUNCHER_LANG_ZH);

    const struct { const char *text; LauncherLanguage expected; } cases[] = {
        {"zh\r\n", LAUNCHER_LANG_ZH}, {"zh", LAUNCHER_LANG_ZH}, {"z", LAUNCHER_LANG_EN},
        {"", LAUNCHER_LANG_EN}, {"zh-Hans\n", LAUNCHER_LANG_EN}, {"zh\nX", LAUNCHER_LANG_EN}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        FILE *file = fopen(path, "wb");
        assert(file && fputs(cases[i].text, file) >= 0 && fclose(file) == 0);
        assert(launcher_language_load(path, LAUNCHER_LANG_EN) == cases[i].expected);
    }
    assert(launcher_language_save(path, LAUNCHER_LANG_EN, LAUNCHER_LANG_EN));
    assert(launcher_language_save(path, LAUNCHER_LANG_EN, LAUNCHER_LANG_EN));
    puts("Launcher language override and system fallback OK");
    return 0;
}
