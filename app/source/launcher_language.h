#ifndef LAUNCHER_LANGUAGE_H
#define LAUNCHER_LANGUAGE_H

#include <stdbool.h>

typedef enum { LAUNCHER_LANG_EN, LAUNCHER_LANG_ZH } LauncherLanguage;

LauncherLanguage launcher_language_load(const char *path, LauncherLanguage system_language);
/* Selecting the system language clears any override, including on later boots. */
bool launcher_language_save(const char *path, LauncherLanguage language, LauncherLanguage system_language);

#endif
