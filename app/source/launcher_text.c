#include "launcher_text.h"
#include <stddef.h>

const LauncherText launcher_text[] = {
    [LAUNCHER_LANG_EN] = {
        .live = {
            [LAUNCHER_READY] = {"Ready to stream", "Open 3DS Live on a PC on the same network.", "R + SELECT", "Start streaming"},
            [LAUNCHER_STOPPED] = {"Stream stopped", "You can restart here or in your game.", "R + SELECT", "Restart stream"},
            [LAUNCHER_SEARCHING] = {"Looking for a PC", "Open the PC player on the same network.", "R + SELECT", "Stop streaming"},
            [LAUNCHER_PAIRED] = {"PC connected", "Waiting for the PC to confirm video reception.", "R + SELECT", "Stop streaming"},
            [LAUNCHER_RECEIVING] = {"Streaming", "The PC is receiving both screens.", "R + SELECT", "Stop streaming"},
            [LAUNCHER_SLEEP] = {"Console asleep", "Streaming resumes when the console wakes.", "Next", "Open the lid"},
            [LAUNCHER_ERROR] = {"Stream failed", "Check Wi-Fi, then try streaming again.", "R + SELECT", "Retry stream"}
        },
        .startup = {
            [UI_STARTING] = {"Starting up", "Checking the SD card and streaming module.", NULL, NULL},
            [UI_WAIT_STATUS] = {"Waiting for module", "The module has yet to report its status.", "Status file", "/3ds/3ds-live/status.txt"},
            [UI_OLD_MODEL] = {"Unsupported model", "This console cannot run the streaming module.", "Supported", "New 3DS and New 2DS models"},
            [UI_ROMFS] = {"Launcher read error", "The launcher could not open its resources.", "Next", "Copy the 3DSX or reinstall the CIA"},
            [UI_BUNDLE] = {"Invalid module", "The bundled streaming module is incomplete.", "Next", "Copy the 3DSX or reinstall the CIA"},
            [UI_CONFLICT] = {"Module conflict", "Check the backup in /luma/sysmodules/.", "File", "0004013000F1D402.cxi"},
            [UI_INSTALL] = {"SD card write failed", "Could not write the module to the SD card.", "Next", "Check free space and write access"},
            [UI_OLD_BUSY] = {"Update pending", "The previous module is still streaming.", "Next", "Stop streaming and reopen 3DS Live"},
            [UI_REBOOT] = {"Restart required", "The previous module is still running.", "Next", "Save your game and restart the 3DS"},
            [UI_LAUNCH] = {"Module start failed", "Check external module loading in Luma.", "Next", "Save, check settings, then restart"},
            [UI_MODEL_QUERY] = {"Model check failed", "The console did not report its model.", "Next", "Close and reopen the launcher"}
        },
        .status = "Stream status", .device = "Device info", .switch_to = "中文",
        .battery = "Battery", .battery_unknown = "Battery unknown", .charging = "Charging",
        .wifi = "Wi-Fi", .connected = "Connected", .disconnected = "Disconnected", .unknown = "Unknown",
        .led = "Notification LED", .led_blue = "Solid blue", .led_amber = "Pulsing amber",
        .led_green = "Solid green", .led_red = "Flashing red", .led_off = "Off", .led_failed = "Update failed",
        .error_code = "Error code", .close = "Close launcher", .in_game = "You can start or stop streaming in-game."
    },
    [LAUNCHER_LANG_ZH] = {
        .live = {
            [LAUNCHER_READY] = {"准备就绪", "在同一网络的电脑上打开 3DS Live。", "R + SELECT", "开始推流"},
            [LAUNCHER_STOPPED] = {"推流已停止", "可以留在这里，也可以回到游戏。", "R + SELECT", "重新开始推流"},
            [LAUNCHER_SEARCHING] = {"正在寻找电脑", "请打开 PC 播放器，并连接同一局域网。", "R + SELECT", "停止推流"},
            [LAUNCHER_PAIRED] = {"电脑已连接", "正在等待电脑确认收到完整画面。", "R + SELECT", "停止推流"},
            [LAUNCHER_RECEIVING] = {"正在投屏", "电脑已确认收到双屏画面。", "R + SELECT", "停止推流"},
            [LAUNCHER_SLEEP] = {"掌机已休眠", "开盖后会重新连接播放器。", "下一步", "开盖唤醒掌机"},
            [LAUNCHER_ERROR] = {"推流启动失败", "请检查无线连接，再尝试开启推流。", "R + SELECT", "重试推流"}
        },
        .startup = {
            [UI_STARTING] = {"正在启动", "正在检查 SD 卡和内置直播模块。", NULL, NULL},
            [UI_WAIT_STATUS] = {"等待模块回应", "尚未读到当前直播模块的状态。", "状态文件", "/3ds/3ds-live/status.txt"},
            [UI_OLD_MODEL] = {"机型不支持", "当前掌机无法运行直播模块。", "支持机型", "New 3DS 或 New 2DS 系列"},
            [UI_ROMFS] = {"启动器读取失败", "无法打开启动器内的资源。", "下一步", "重新复制 3DSX 或安装 CIA"},
            [UI_BUNDLE] = {"模块文件无效", "启动器内的直播模块不完整。", "下一步", "重新复制 3DSX 或安装 CIA"},
            [UI_CONFLICT] = {"检测到文件冲突", "请核对 /luma/sysmodules/ 中的文件和备份。", "模块文件", "0004013000F1D402.cxi"},
            [UI_INSTALL] = {"模块写入失败", "无法将直播模块安装到 SD 卡。", "下一步", "检查 SD 卡空间和写入状态"},
            [UI_OLD_BUSY] = {"等待旧版停流", "新版模块已安装，旧版仍在使用中。", "下一步", "停流后重新打开启动器"},
            [UI_REBOOT] = {"需要重启掌机", "新版模块已安装，旧版尚未退出。", "下一步", "保存游戏进度后重启掌机"},
            [UI_LAUNCH] = {"模块未能启动", "请确认 Luma 已开启外部模块加载。", "下一步", "保存进度，检查设置后重启"},
            [UI_MODEL_QUERY] = {"无法识别机型", "系统未能返回掌机型号。", "下一步", "退出后重新打开启动器"}
        },
        .status = "直播状态", .device = "设备信息", .switch_to = "English",
        .battery = "电量", .battery_unknown = "电量未知", .charging = "充电中",
        .wifi = "无线网络", .connected = "已连接", .disconnected = "未连接", .unknown = "未知",
        .led = "通知灯", .led_blue = "蓝灯常亮", .led_amber = "琥珀色呼吸",
        .led_green = "绿灯常亮", .led_red = "红灯短闪", .led_off = "熄灭", .led_failed = "设置失败",
        .error_code = "错误代码", .close = "退出启动器", .in_game = "退出后仍可在游戏里开始或停止推流。"
    }
};
