# 3DS Live Windows 播放器

完整解压 Windows x64 发布包，双击 `3DS-Live.exe`。移动播放器时将同级的 `runtime` 文件夹一起复制。

## 第一次投屏

1. 将 New 3DS 和 PC 接入同一局域网。PC 可用网线或 Wi-Fi。Windows 询问网络访问时，允许播放器在当前专用网络接收入站 UDP。
2. 打开播放器，在掌机上按 **R + SELECT**。两端可以按任意顺序启动。掌机开始直播后，播放器会自动发现并显示双屏画面。
3. 在 **⋯ → 投屏设置 → 投屏连接** 可查看端口。默认是 UDP **8001**，掌机 IPv4 留空即可自动发现。当路由器重新分配地址时，程序会重新配对，期间窗口保留最后一帧。

若有多台 PC 开着播放器，掌机会选先回复的一台。要切换，在这台 PC 关闭播放器，保持目标 PC 的播放器打开，等待原连接超时。若有多台掌机，可在播放器中填写指定掌机的 IPv4。这个设置会固定接收来源，清空后恢复自动发现。

如果一直显示等待连接，先检查掌机是否已开始推流、两端的 UDP 端口是否一致，再检查 Windows 入站规则和路由器的访客网络或无线客户端隔离。端口被占时可关闭占用程序，再在菜单选择 **重新监听**。局域网广播需要两台设备处于同一广播域。

## 观看与输出

- **FPS / MB/s** 调整掌机的目标帧率和带宽上限。默认双屏各 30 FPS、0.8 MB/s。播放器分别呈现已经收齐的上下屏，并根据实际收包情况调节码率。
- **⋯ → 画质** 可切换原始、轻度增强、DnCNN、FBCNN 或 Real-ESRGAN。**补帧** 支持关闭、DIS 光流或 RIFE。原生分辨率为上屏 400×240、下屏 320×240。标准包的 AI 模式使用 DirectML，需要兼容的 DirectX 12 显卡。
- **H** 隐藏或显示工具栏，**F11** 切换全屏，**Ctrl+,** 打开设置。底栏 **ⓘ** 可查看各屏实收与呈现帧率，**F12** 查看诊断和最近 60 秒的卡顿记录。
- **Ctrl+R** 将当前预览画面和游戏音频录为本地 MP4。播放器静音只影响 PC 扬声器。RTSP 输出需要一个已经运行的 RTSP 服务，在投屏连接设置中填写发布 URL 即可。OBS 的媒体源可以打开同一个 URL。

## 命令行

从播放器所在目录运行。省略 `--sender` 会自动发现。需要指定某台设备时才加 `--sender <掌机IPv4>`。

```powershell
.\3DS-Live.exe --preview
.\3DS-Live.exe --preview --fps 30 --rate 800000 --enhance fbcnn-detail
.\3DS-Live.exe --preview --enhance fast-detail --interpolate rife --rife-precision fp16
.\3DS-Live.exe --preview --rtsp rtsp://127.0.0.1:8554/live
.\3DS-Live.exe --preview --seconds 30 --record game.mp4
.\3DS-Live.exe --preview --debug --stats session.jsonl --trace frames.jsonl
```

命令行录像需提供新文件名。`--stats` 追加逐秒统计，`--trace` 创建逐帧跟踪文件。

## 从源码构建

在项目根目录使用 PowerShell 7，准备 Visual Studio 的 x64 C++ 工具链、Qt 6 MSVC x64、CMake 和 7-Zip。将 `-Models` 指向标准发布包的 `runtime\models`，或使用带模型的 Qt 源码包。

```powershell
pwsh -File live/qt/build.ps1 -QtSdk C:/Qt/6.10.3/msvc2022_64 -Models C:/3ds-live-models
```

脚本校验模型和 SDK，运行 Qt 测试并生成可分发的播放器。使用 `-DependencyCache` 可复用已解压的开发库，`-OutputDirectory` 可指定输出目录，`-Zip` 可同时打包。附带的第三方许可证位于 `runtime/licenses`。
