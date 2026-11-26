#ifndef MODULE_INSTALL_H
#define MODULE_INSTALL_H

#include <stdint.h>

#define LIVE_MODULE_ID UINT64_C(0x0004013000F1D402)
#define LIVE_MODULE_SD_PATH "luma/sysmodules/0004013000F1D402.cxi"

typedef enum {
	MODULE_CURRENT = 0,
	MODULE_INSTALLED = 1,
	MODULE_INSTALL_ERROR = -1,
	MODULE_INVALID_BUNDLE = -2,
	MODULE_TARGET_CONFLICT = -3
} ModuleInstallResult;

/* Called by the single foreground launcher. errno gives the reason on failure.
 * The .installing and .previous siblings are reserved for this transaction. */
ModuleInstallResult module_install(const char *bundled_cxi,
				   const char *sd_root);

#endif
