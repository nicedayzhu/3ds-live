#include <3ds.h>
#include "status_led_pattern.h"

int32_t status_led_apply(LedState state)
{
    const Handle ptm = *ptmSysmGetSessionHandle();
    if (!ptm) return MAKERESULT(RL_STATUS, RS_INVALIDSTATE, RM_PTM, RD_NOT_INITIALIZED);
    const StatusLedPattern pattern = status_led_pattern(state);
    u32 *command = getThreadCommandBuffer();
    command[0] = IPC_MakeHeader(0x801, sizeof(pattern) / sizeof(u32), 0);
    memcpy(command + 1, &pattern, sizeof(pattern));
    Result rc = svcSendSyncRequest(ptm);
    return R_FAILED(rc) ? rc : (Result)command[1];
}
