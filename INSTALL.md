# 3DS Live 使用指南

3DS Live 将 New 3DS、New 3DS XL 或 New 2DS XL 的上下屏和游戏声音传到 Windows x64 电脑。掌机和电脑连接同一路由器，PC 可以用网线。默认端口为 UDP 8001。

## 第一次安装，或从旧版 Recorder / Live 迁移

1. 完整解压发布包。将 **SD 文件夹里面的内容**合并到掌机 SD 卡根目录。复制后应能找到 `/3ds/3ds-live/3ds-live.3dsx` 和 `/luma/sysmodules/0004013000F1D402.cxi`。默认设置已内置，可以直接使用。
2. 旧版如果开启过 **Launch On Startup**，先在旧控制程序关闭它，再完整重启掌机。旧模块与独立版使用相同的直播快捷键。原来的 SD 录像可以保留。
3. 按住 **SELECT** 开机，在已安装的 Luma3DS 配置中勾选 **Enable loading external FIRMs and modules**，按 **START** 保存。每次开机后，在 Homebrew Launcher 打开一次 **3DS Live**。出现就绪提示后按 **START** 退出，进入游戏。
4. 打开电脑上的 **Windows/3DS-Live.exe**，保留旁边的 `runtime` 文件夹。如果 Windows 弹出防火墙提示，允许它在当前专用网络上通信。
5. 在游戏里按 **R + SELECT** 开始投屏，再按一次停止。也可以先按掌机热键，再打开播放器。连接建立后，窗口会显示上下屏和源帧率。

在 Windows 上长期使用时，可将发布包的 `Windows` 文件夹完整复制到 `%LOCALAPPDATA%\Programs\3DS Live`，再从该目录启动 `3DS-Live.exe` 或为它创建开始菜单快捷方式。本机编译产物放在低完整性工作目录时，Windows 外壳可能在每次双击时显示“打开文件 - 安全警告”。

## 更换 Wi-Fi 或 IP

在 Qt 播放器的 **⋯ → 投屏设置 → 投屏连接** 中将 **掌机 IPv4** 留空。独立版默认自动发现，初次打开会沿用旧版的画质、音量状态和码率设置，并重新寻找掌机。两端换地址后会自行重试。默认四秒未收到有效回应，掌机会重新广播寻找播放器。

多台电脑同时打开时，掌机会连接先回应的一台。要换电脑，关闭当前播放器，让另一台继续监听。想固定某台掌机时，可在 Qt 设置里填写它的 IPv4。

双方等待连接时，PC 向掌机的 UDP 8002 发送邀请，掌机也会向 PC 的媒体端口发送发现请求。一直等待连接时，请确认已按热键，两端在同一 IPv4 广播域，媒体端口一致。检查 Windows 防火墙对当前播放器完整路径的入站规则，允许专用网络中本地子网访问 UDP 8001（自定义端口时改用所选端口）。同一路径的显式阻止规则也需排查。访客网络和 AP 隔离会影响发现。

掌机的 `/3ds/3ds-live/status.txt` 可区分待机（`idle`）、寻找播放器（`searching`）、已配对（`streaming`）和启动错误（`error`）。同目录的 `network-status.txt` 记录套接字、发现请求和回应的结果。替换系统模块后需完整重启掌机，再打开 3DS Live 加载新版本。

默认双屏 30 FPS、0.8 MB/s，开启立体声。PC 上可以直接调节画质与码率，按 **F12** 查看诊断，按 **Ctrl+R** 将直播保存为 MP4。其余画质、补帧和 RTSP 选项见随包的 `Windows/runtime/README.md`。

已有适配本机显卡的 TensorRT 加速包时，可将原来的 `Windows/runtime/acceleration` 整个目录复制到新播放器的相同位置。播放时可在性能卡上查看 FBCNN 和 RIFE 实际使用的后端。

需要自定义掌机端口或固定 PC 时，才将 `/3ds/3ds-live/live.ini.example` 复制为同目录的 `live.ini`。自动模式使用 `host=auto`，改端口时两端填相同的值。停止、重新开始直播后生效。
