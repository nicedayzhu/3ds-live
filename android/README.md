# 3DS Live — Android 接收端

把 New 3DS 的上下双屏和游戏立体声，经局域网无线投到 Android 手机。接收端协议与 Windows 播放器一致（[PROTOCOL.md](../live/docs/PROTOCOL.md)），掌机端无需改动，可与 PC 二选一连接。

设计依据：[DESIGN.md](../docs/android/DESIGN.md)（评估、架构、里程碑）与 [UI-SPEC.md](../docs/android/UI-SPEC.md)（设计令牌、页面、组件）。

## 架构：Flutter 界面 + Kotlin 收流引擎

```
Flutter (Dart)                          Kotlin (前台服务)
连接 / 直播 / 诊断 / 设置  ←MethodChannel→  StreamEngine
主题、状态、布局、手势     ←EventChannel ←  发现、重组+FEC、行复用、LZ4、音频、纹理
```

- 界面、主题、状态、手势全部在 `lib/`（Flutter）；Kotlin 里没有 Android 界面代码。
- 视频通过 `Texture`（`TextureRegistry.SurfaceProducer`）送入 Flutter：引擎把双屏合成到一张画布，Flutter 只做一次 GPU 采样。
- 音频用 `AudioTrack` 低延迟输出；IMA ADPCM 解码与 32.728 kHz → 48 kHz 重采样在 `AudioPlayer.kt`。
- 前台服务 `StreamService` 保证切到其他 App 时继续接收。

## 目录

| 路径 | 内容 |
| --- | --- |
| `lib/theme/app_theme.dart` | Material 3 主题与设计令牌（与 cia-bridge 同源） |
| `lib/stream/models.dart` | 阶段、设备、统计、预设、设置模型 |
| `lib/stream/live_controller.dart` | 应用状态；事件订阅、演示模式、设置持久化 |
| `lib/stream/stream_bridge.dart` | MethodChannel / EventChannel 契约 |
| `lib/ui/` | 连接页、直播页、画质弹层、诊断页、设置页、关于页与共享组件 |
| `android/app/src/main/kotlin/.../stream/` | 协议与收流引擎（见下表） |
| `test/app_test.dart` | Flutter widget 测试（412×915 视口，7 个） |
| `android/app/src/test/kotlin/...` | JVM 协议测试（LZ4、JPEG 行、重组+FEC、Resolver、码率、整条会话，31 个） |

引擎文件：

| 文件 | 职责 |
| --- | --- |
| `Wire.kt` | 报文标签、长度与小端读写 |
| `Assembler.kt` | R10 合并对重组、XOR 单洞恢复、修复请求触发、1 s 过期 |
| `JpegRows.kt` | R17D/R17K 行解析与还原（15 行、RST 校验、EOI） |
| `JpegHeader.kt` | 由 `codec/include/live_jpeg_wire.h` 生成的 629 字节 JPEGENC 头 |
| `Lz4.kt` | 带字典的 LZ4 block 解码（R20D 下屏压缩） |
| `ScreenResolver.kt` | 引用帧管理、R8J1/R17K/R17D/R20D → 可解码 JPEG |
| `AudioPlayer.kt` | IMA ADPCM 解码、重采样、AudioTrack |
| `VideoSink.kt` | JPEG 解码、双屏合成、Texture、截图 |
| `StreamEngine.kt` | 发现、会话、配置、反馈、诊断窗口 |
| `StreamService.kt` | 前台服务与通知 |
| `MainActivity.kt` | 通道接线、权限/服务启动、设置持久化 |

## 快速验证

```powershell
$adb = 'F:\work\music_online_apk\.toolchains\android-sdk\platform-tools\adb.exe'
& $adb install -r build\app\outputs\flutter-apk\app-debug.apk
& $adb logcat -s 3ds-live AndroidRuntime:E
```

- **没有掌机也想看界面**：用 `--dart-define=THREEDS_MOCK=true` 构建（本仓库已生成 `app-debug-mock.apk`），App 会进入演示模式，设备列表、直播页、诊断、画质弹层都能操作。
- **Android 单独接收**：先确认 PC 播放器已停止；掌机进就绪画面按 `R + SELECT`；App 点“开始寻找掌机”。正常日志依次出现 `started on udp 8001`、`peer=<ip>:<port> session=…`、`phase=live`、`pairs=1 …`。
- **PC 单独接收**：先停掉手机端，运行 `dist/Windows/3DS-Live.exe`，按 `F12` 看对齐窗口。
- **互斥**：两端同时开只有一台拿到画面，另一台 8 秒后出现提示；停掉接收端后，另一台在 4–8 秒内接管。
- **录制**：直播页 ⏺ → 停止，Android 10+ 在相册 `Movies/3DS Live`；PC 用 `Ctrl+R`。

## 与 PC 的互斥

掌机同一时刻只连接一个接收端（协议只有单 peer）。PC 播放器与手机 App 各自都能独立接收，但同时开只有先应答的一方拿到画面；另一台会停在“正在寻找掌机”。切换时先在旧设备停止，掌机 4 秒租约到期后会重新广播。

需要对外直播时不需要 Android 参与：在 PC 上用 OBS 采集播放器窗口（或让播放器 RTSP 推流到 RTSP 服务器，再用 OBS 媒体源拉取）。

## 构建与测试

需要 Flutter 3.47.9（Dart 3.13.7）、Android SDK（platform 36）与 JDK 17：

```powershell
$env:JAVA_HOME    = 'F:\work\music_online_apk\.toolchains\jdk17'
$env:ANDROID_HOME = $env:ANDROID_SDK_ROOT = 'F:\work\music_online_apk\.toolchains\android-sdk'
$flutter = 'D:\Games\Old_Times\3ds\cia-bridge\.toolchains\flutter-sdk\flutter\bin\flutter.bat'

& $flutter analyze
& $flutter test
& $flutter build apk --debug          # build/app/outputs/flutter-apk/app-debug.apk
```

协议单测（JVM，无需设备）：

```powershell
cd android
.\gradlew.bat testDebugUnitTest       # 31 tests
```

## 已实现

- 发现与配对：广播/单播 `3DLP` 邀请、`3DLS`/`3DLA` 应答、4 s 端点租约、固定 IP。
- 收流：`R10P`/`R10F` 合并重组、4 条 XOR 纠错、`R19P` 修复包、1 s 未完成丢弃。
- 画面：`R8J1` 紧凑帧、`R17K` 参考帧、`R17D` 行增量、`R20D` LZ4 下屏；双屏合成到 Texture。
- 音频：`R9A1` IMA ADPCM、48 kHz 输出、静音/音量。
- 配置：`R6CF`/`R10C`/`R16C` 下发布、`R3FB` 反馈、`R17A`/`R19A`/`R20A` 能力应答。
- 诊断：收齐 FPS、上/下屏、掌机上报的容量与质量、丢片、修复、行复用/LZ4 节省、音频覆盖。
- 界面：连接页四态、直播页（竖屏双屏 / 横屏单屏与并排、手势缩放、控件自动隐藏与锁定）、画质弹层、诊断页、设置页、深浅色。
- 设置持久化（SharedPreferences）。
- 录制：`MediaCodec` H.264（从合成画布）+ AAC（解码后的 PCM）写入 MP4，保存到应用私有 Movies 目录。
- 分屏旧格式 `R3P2`/`R3F2` 重组与两条 XOR 纠错；自适应码率按 3 秒降档 / 8 秒升档。

## 尚未实现 / 已知限制

- 录制文件在 Android 10+ 会发布到系统相册 `Movies/3DS Live`；没有内置分享入口，早期版本落在应用私有目录。
- `R15P`/`R15F` 旧格式（0.1.0 前的掌机端）未处理。
- 效果（FBCNN/SPAN/RIFE）按设计不做。
- 本机没有 Android 设备或模拟器，收流链路用 JVM 单测与 liblz4 交叉验证，真机时延与丢包表现仍需实测。

## 与 PC 播放器的差异

| PC | Android |
| --- | --- |
| 窗口、多显示器、RTSP、FFmpeg 录制 | 单全屏、Texture 合成、MediaCodec 录制（待做） |
| ONNX 画质增强与补帧 | 不做 |
| 命令行参数 | 设置页 + 诊断页导出 JSON |
