#include "launcher_device.h"
#include <3ds.h>

static bool have_ac, have_ptm;

void launcher_device_init(void)
{
    have_ac = R_SUCCEEDED(acInit());
    have_ptm = R_SUCCEEDED(ptmuInit());
}

LauncherDevice launcher_device_read(void)
{
    LauncherDevice device = { -1, -1, -1, -1 };
    u32 state;
    if (have_ac && R_SUCCEEDED(ACU_GetStatus(&state))) {
        if (state == 1) device.wifi = 0;
        if (state == 3) {
            device.wifi = 1;
            const u8 bars = osGetWifiStrength();
            if (bars <= 3) device.strength = bars;
        }
    }
    u8 value;
    if (have_ptm && R_SUCCEEDED(PTMU_GetBatteryLevel(&value)) && value <= 5)
        device.battery = value;
    if (have_ptm && R_SUCCEEDED(PTMU_GetBatteryChargeState(&value)) && value <= 1)
        device.charging = value;
    return device;
}

void launcher_device_exit(void)
{
    if (have_ptm) ptmuExit();
    if (have_ac) acExit();
    have_ac = have_ptm = false;
}
