# 3DS Live Windows 播放器

完整解压 Windows x64 发布包，双击 `3DS-Live.exe`。移动播放器时将同级的 `runtime` 文件夹一起复制。

## 第一次投屏

1. 将 New 3DS 和 PC 接入同一局域网。PC 可用网线或 Wi-Fi。Windows 询问网络访问时，允许播放器在当前专用网络接收入站 UDP。
2. 打开播放器。在掌机上打开 **3DS Live**，看到就绪提示后，留在启动器按一次 **R + SELECT**，播放器会自动发现并显示双屏画面。要进入游戏时，按 **START** 关闭启动器。也可以进入游戏后再按 **R + SELECT** 开始投屏。两端可以按任意顺序启动。
3. 在 **⋯ → 投屏设置 → 投屏连接** 可查看端口。默认是 UDP **8001**，掌机 IPv4 留空即可自动发现。当路由器重新分配地址时，程序会重新配对，期间窗口保留最后一帧。

若有多台 PC 开着播放器，掌机会选先回复的一台。要切换，在这台 PC 关闭播放器，保持目标 PC 的播放器打开，等待原连接超时。若有多台掌机，可在播放器中填写指定掌机的 IPv4。这个设置会固定接收来源，清空后恢复自动发现。

如果一直显示等待连接，先检查掌机是否已开始推流、两端的媒体端口是否一致。PC 默认在 UDP 8001 接收掌机发现请求和音视频，并从同一端口向掌机的 UDP 8002 发送发现邀请。检查 Windows 防火墙中当前播放器完整路径的入站规则，以及路由器的访客网络或无线客户端隔离。端口被占时可关闭占用程序，再在菜单选择 **重新监听**。局域网广播需要两台设备处于同一广播域。

## 观看与输出

- **FPS / MB/s** 调整两屏共同的目标帧率和总带宽上限。默认双屏各 30 FPS、0.8 MB/s。以此设置推流时，如果丢包使上限自动降到 0.60 MB/s，两屏一起改为 24 FPS，每对 JPEG 可分到更多带宽。链路恢复到 0.65 MB/s 后即可回到 30 FPS。手动选择 0.60 MB/s 时仍使用所选帧率，掌机持续无法发出画面时才会降低帧率。
- **⋯ → 画质** 可切换原始、轻度增强、DnCNN、FBCNN 或 Real-ESRGAN。**补帧** 支持关闭、DIS 光流或 RIFE。原生分辨率为上屏 400×240、下屏 320×240。标准包的 AI 模式使用 DirectML，需要兼容的 DirectX 12 显卡。
- **H** 隐藏或显示工具栏，**F11** 切换全屏，**Ctrl+,** 打开设置。底栏 **ⓘ** 可查看各屏实收与呈现帧率，**F12** 查看诊断和最近 60 秒的卡顿记录。
- **Ctrl+R** 将当前预览画面和游戏音频录为本地 MP4。播放器静音只影响 PC 扬声器。RTSP 输出需要一个已经运行的 RTSP 服务，在投屏连接设置中填写发布 URL 即可。OBS 的媒体源可以打开同一个 URL。

## 画质和补帧的兼容性

选择 AI 效果后，播放器在后台用实际模型检查推理。通用包使用 DirectML，兼容的 AMD、Intel、NVIDIA 显卡都可以尝试。本机配有相应引擎时，FBCNN 和 RIFE 也可以使用 TensorRT。准备画质模型期间可以先看原始画面。

如果当前画质无法运行，播放器会切到轻度增强；RIFE 无法运行时会切到 CPU 的 DIS 光流。失败的选项会在本次运行中标为不可用。可到 **投屏设置 → 画面与补帧** 查看原因、重新检测，或更换 RIFE 精度后点 **应用**。手动选择的模式会保留，下次启动时重新检查。

推流时，若本机推理连续三个采样窗口超过实际收帧间隔，同时有待处理画面被跳过，设置中会出现 **使用建议设置**。可以只降低遇到瓶颈的效果，也可以保持当前画质。此建议不会调整掌机码率。

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

命令行录像需提供新文件名。`--stats` 追加逐秒统计，`--trace` 创建逐帧跟踪文件。排查上屏停顿时，按会话和 `top_pts_us` 识别新的采样，再用 `choice` 对齐选帧与绘制事件。

- `arrive_top` 表示上屏分片已组齐，`raw` 表示解码图像已进入播放队列。
- `select` 记录播放器选择新上屏的时间，`paint` 记录 Qt 窗口画布完成本次绘制的时间。同一上屏采样可能随不同网络帧号重复到达。
- 关闭补帧、播放原始画质时，播放器会在采样间隔突然拉长后提前显示已解码的新图，再逐帧回到音频时钟。提前量最多为 45 毫秒，同时受播放缓冲一半的限制。

## 从源码构建

在项目根目录使用 PowerShell 7，准备 Visual Studio 的 x64 C++ 工具链、Qt 6 MSVC x64、CMake 和 7-Zip。将 `-Models` 指向标准发布包的 `runtime\models`，或使用带模型的 Qt 源码包。

```powershell
pwsh -File live/qt/build.ps1 -QtSdk C:/Qt/6.10.3/msvc2022_64 -Models C:/3ds-live-models
```

脚本校验模型和 SDK，运行 Qt 测试并生成可分发的播放器。使用 `-DependencyCache` 可复用已解压的开发库，`-OutputDirectory` 可指定输出目录，`-Zip` 可同时打包。附带的第三方许可证位于 `runtime/licenses`。
