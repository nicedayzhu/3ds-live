# 3DS Live Android 端 — 评估与架构设计

本文回答两个问题：Windows 播放器的哪些能力值得移植到 Android，以及移植后的应用结构、协议实现方式、性能预算与里程碑。界面规范见 [UI-SPEC.md](UI-SPEC.md)。

- 目标平台：Android 9（API 28）及以上，手机为主，兼容平板/折叠屏横屏。
- 设计基线：与 [cia-bridge](https://github.com/nicedayzhu/cia-bridge)、[game-format-studio](https://github.com/nicedayzhu/game-format-studio) 一致的定制 Material 3（Flutter）视觉，令牌见 UI-SPEC。
- 定位：**口袋里的双屏接收器** —— 手机当第二块屏看 3DS，顺带录制与分享；不是 PC 端的完全替代。

## 1. 结论摘要

| 判断 | 内容 |
| --- | --- |
| 可行性 | **高**。接收路径（UDP 收包、XOR 纠错、行复用还原、LZ4 解压、JPEG 解码、IMA 音频、双屏合成）总负载在手机上属于轻量级；难点在播放渲染、音频低延迟、录制与 Android 生命周期，而非协议。 |
| 首版范围 | 发现配对、双屏投流、声音、画质档位（FPS/Q/带宽）、自适应码率、诊断面板、录制 MP4、截图、前台服务与重连。 |
| 明确不做 | ONNX 画质增强与补帧（FBCNN / DnCNN / SPAN / Real-ESRGAN / RIFE）、TensorRT、RTSP 推流、Windows WLAN 流媒体模式。 |
| 技术形态 | Flutter（Material 3）负责界面与状态；Kotlin 前台服务承载收流引擎；视频经**单张合成 Texture** 进 Flutter，不做逐帧像素搬运。 |
| 与 PC 的互操作 | 完全一致：Android 端就是"PC 接收端"的另一种实现，掌机端无需改动，可与 PC 二选一连接。 |

## 2. PC 功能盘点与移植判定

| PC 功能 | 依赖 | 判定 | Android 方案 |
| --- | --- | --- | --- |
| 双屏接收与显示 | 协议 | **移植** | 上屏 400×240、下屏 320×240，合成单画布后由 `Texture` 呈现 |
| 分屏 / 旧版报文格式 | `R3P2`/`R3F2`、`R15P`/`R15F` | **移植** | R3 分屏格式随合并格式一起实现；R15 旧格式放到 M4 兼容 |
| 播放缓冲 / 延迟控制 | 时间线 | **移植** | 默认 100 ms，可调 60 / 100 / 160 ms；`Image` 队列深度按延迟推导 |
| 自动发现与配对 | `3DLS`/`3DLA`/`3DLP` | **移植** | 广播邀请 + 单播应答 + 4 s 端点租约，语义与 PC 相同 |
| 固定掌机 IP / 监听端口 | 协议 | **移植** | 设置项；固定 IP 时跳过广播直接单播 |
| 帧率 / JPEG 质量 / 带宽档位 | `R6CF`/`R10C`/`R16C` | **移植** | 三档预设 + 自定义（15/20/24/30 FPS，Q0/40/60/70/80，400 k–2 M） |
| 自适应 / 固定带宽上限 | `R3FB` + 窗口统计 | **移植** | 与 PC 相同的降档/升档规则，UI 可见当前上限与原因 |
| JPEG 行复用（R17） | `R17S/K/A/D` | **移植** | 参考帧缓存 4 组、行还原与校验同 PC |
| 选择性修复（R19） | `R19S/A/N/P` | **移植** | 请求上限 16 片，65 ms 响应窗，超时放弃 |
| 下屏 LZ4（R20） | `R20S/A/D` | **移植** | 原生 LZ4 block + 32 KiB 字典；优先用 liblz4（NDK），退化到纯 Kotlin 实现 |
| 诊断（F12） | `R7ST`/`R9ST`/`R17T`/`R19T`/`R21T` | **移植（简化）** | 独立诊断页：窗口对齐的 MB/s、收齐 FPS、Q、丢片、修复、行复用节省、音频覆盖 |
| 录制 MP4 | FFmpeg | **改实现** | `MediaCodec`(H.264) + `MediaMuxer`，音频走 AAC；按合成画布 2× 录制 |
| 截图 | — | **新增** | 直接抓当前合成帧，写入 `Pictures/3DS Live/` |
| RTSP 发布 | FFmpeg | **不做** | 手机端无 OBS 场景；确有需要可后续用 `MediaCodec` + 本地 RTSP 服务补 |
| FBCNN / DnCNN / SPAN / ESRGAN / RIFE | ONNX Runtime + DirectML/TensorRT | **不做** | 模型与 EP 均为 Windows 生态；后续可用 NNAPI/GPU shader 做轻度锐化或 2× 显示缩放 |
| 画面模式（清爽/锐化/2× 超分/补帧倍率） | 本地效果链 | **裁剪** | 保留"原始/轻度锐化"与显示缩放；去掉生成式效果 |
| Windows 专属层 | `WlanStream`、`SIO_UDP_CONNRESET`、`waveOut`、`windows.h` | **替换** | `WifiLock`、`AudioTrack`、`SystemClock` |
| 命令行参数 / `--stats` JSON | CLI | **改形态** | 设置项 + 诊断页"导出 JSON" |
| 多窗口 / 显示器选择 | 桌面 | **不需要** | 用横竖屏与画中画替代 |

**取舍原则**：保留"让直播更稳、更省流量、可回看"的能力（行复用、修复、LZ4、自适应、录制、诊断）；放弃"把手机当工作站"的能力（AI 增强、RTSP、多窗口）。

## 3. 协议接收端设计

Android 端完整扮演 PC 接收端，消息集合与 [PROTOCOL.md](../../live/docs/PROTOCOL.md) 一致。

### 3.1 状态机

```
IDLE ──启动──▶ INVITING ──收到 3DLS──▶ RESERVED ──首帧──▶ LIVE
                   ▲                      │                │
                   │                  4s 无媒体/控制         │
                   └──── LOST ◀──────────┴────────────────┘
LIVE ──掌机停流/离开──▶ HOLDING（保留最后一帧 N 秒）──▶ LOST/IDLE
```

| 状态 | 进入条件 | 退出条件 | UI |
| --- | --- | --- | --- |
| `INVITING` | 用户点"连接"或进入前台 | 收到有效 `3DLS`（应答 `3DLA`） | 搜索动画 + 设备列表 |
| `RESERVED` | 端点与 session 通过校验 | 首对双屏完整 | 等待画面骨架 |
| `LIVE` | 收到完整帧 | 4 s 无音视频 | 直播页 |
| `HOLDING` | 停流 | 用户返回 / 超时 | 冻结最后一帧 + 提示 |
| `LOST` | 租约过期 | 自动重邀或用户手动 | 重连卡片 |

### 3.2 数据路径

```
UDP socket ─▶ 报文分发（tag + 长度 + 端点/session 校验）
   ├─ 3DLS/3DLA：发现
   ├─ R10P/R10F：重组 + XOR 恢复（1168 B/chunk，4 stripe）
   ├─ R17K/R17D：行还原（15 行 RST 校验、changed-row mask）
   ├─ R20D：LZ4 解压 → R17D
   ├─ R9A1：IMA ADPCM 解码 → PCM 48 kHz
   ├─ R7ST/R9ST/R16S/R17T/R19T/R21T：状态与诊断
   └─ R6CF/R10C/R16C/R3FB/R17A/R19N/R20A：出站控制
        │
        ├─ 视频：JPEG → Bitmap → 双屏合成（GPU）→ ImageReader Surface → Flutter Texture
        └─ 音频：PCM → AudioTrack（低延迟，4×10 ms）
```

### 3.3 必须保持的常量

| 常量 | 值 | 用途 |
| --- | --- | --- |
| `CHUNK` | 1168 B | 分片负载 |
| XOR 条带 | 合并 4 / 分屏 2 | FEC 恢复 |
| 单屏上限 | 128 000 B | 报文长度校验 |
| 行数 | 15 行，RST `FF D0+(i&7)` | 行复用还原 |
| LZ4 字典 | 参考帧尾部 ≤32 KiB | 下屏解压 |
| 修复 | ≤16 片、65 ms 响应、90 ms 帧预算 | R19 请求 |
| 音频 | 32 728.498 kHz 立体声，160 样本/块，175 B/条 | IMA 解码 |
| 状态窗口 | 1.5 s 新鲜度、4 s 租约 | 诊断与租约 |

### 3.4 线程与时钟

| 线程 | 职责 | 备注 |
| --- | --- | --- |
| Socket（`Dispatchers.IO` 独立线程） | 收包、分发、出站控制 | `SO_RCVBUF` 4 MB；单次最多 256 个报文 |
| Decode/Compose | 行还原、LZ4、JPEG、合成 | 单线程串行，避免锁争用 |
| Audio | IMA 解码 + `AudioTrack.write` | 10 ms 块，非阻塞写 |
| UI（主线程） | 状态与控件 | 只读快照，不碰网络缓冲 |

时钟统一用 `SystemClock.elapsedRealtimeNanos()`；协议 32 位 PTS 按 PC 的 64 位扩展法处理（`newest + (int32)(raw - newest)`）。播放截止时间与抖动统计也用同一时钟，避免 `System.currentTimeMillis()` 被校时打断。

## 4. 应用架构

```
Flutter (Dart)
  UI 层      连接页 / 直播页 / 画质弹层 / 诊断页 / 设置 / 关于
  状态层      ChangeNotifier + ValueListenableBuilder（cia-bridge 同款轻量方案）
  桥接层      StreamBridge：MethodChannel 命令 + EventChannel 状态流
        |  命令 / 事件（< 1 KB，非像素）
        v
Kotlin 前台服务 StreamService
  |- Discovery       广播 / 单播、租约、session
  |- Receiver        分发、重组、FEC、修复、能力协商
  |- VideoPipeline   行还原 / LZ4 / JPEG / 合成 → Surface
  |- AudioPipeline   IMA → PCM → AudioTrack
  |- Recorder        MediaCodec + MediaMuxer（可选）
  `- Telemetry       诊断窗口、事件日志、导出
        |  NDK（可选，M2 起）
        v
原生库 liblz4（沿用 third_party/lz4）
  行还原与 FEC 留在 Kotlin（可测、无 JNI 边界），只有 LZ4 走 JNI

```

**为什么不在 Flutter/Dart 里收流**：`RawDatagramSocket` 可用，但 30 fps × 双屏的解码、合成与音频调度需要稳定线程与低抖动，Dart 的事件循环会被 UI 帧率牵连；Kotlin 服务还能在应用退到后台时继续收流（前台服务 + 通知）。Flutter 只负责"看"和"控"。

**为什么用单张合成 Texture**：每帧通过平台通道传 RGBA（约 1.5 MB/帧，45 MB/s）不可行；`Texture` 由引擎直接写入 `Surface`，Flutter 只做一次 GPU 采样。布局（上下 / 并排 / 单屏放大）在引擎侧合成，切换布局只改合成参数。

### 4.1 建议目录

```
android/
  lib/                       # Flutter
    main.dart
    theme/app_theme.dart     # 由 cia-bridge 令牌派生（见 UI-SPEC）
    ui/connect_page.dart
    ui/live_page.dart
    ui/quality_sheet.dart
    ui/diagnostics_page.dart
    ui/settings_page.dart
    ui/about_page.dart
    stream/stream_controller.dart   # 状态与事件订阅
    stream/models.dart
  android/app/src/main/kotlin/.../stream/
    StreamService.kt         # 前台服务与生命周期
    Discovery.kt
    Receiver.kt
    Assembler.kt
    Rows.kt                  # R17/R20
    Repair.kt
    AudioDecoder.kt
    VideoComposer.kt
    Recorder.kt
    Telemetry.kt
  test/                      # 协议单测 + widget/golden
```

### 4.2 桥接接口（契约先行）

以下为 Dart 侧方法与事件类型的示意，最终签名以实现为准。

```text
// 命令（Dart → Kotlin）
start({int port = 8001, String? handheldIp, int fps, int rate, int quality, bool adaptive});
stop();
configure({int fps, int rate, int quality, bool adaptive});
startRecording(); stopRecording();
capture(); // 截图
setLayout(LayoutMode stacked | sideBySide | topOnly | bottomOnly);
exportDiagnostics();

// 事件（Kotlin → Dart，EventChannel）
sealed class StreamEvent {}
class StateChanged   implements StreamEvent { StreamPhase phase; String? message; }
class HandheldFound  implements StreamEvent { String ip; int port; int session; }
class StatsTick      implements StreamEvent { StreamStats stats; }
class Diagnostics    implements StreamEvent { Map<String, dynamic> window; }
class RecordingState implements StreamEvent { bool active; String? path; int bytes; }
class ErrorEvent     implements StreamEvent { String code; String message; }
```

`StreamStats` 至少包含：收齐 FPS、上屏 FPS、Q、带宽上限、实收 MB/s、丢片、修复命中、行复用节省、音频覆盖、缓冲延迟。Dart 侧只展示，不改写。

## 5. 性能与内存预算

以默认 30 FPS、Q70、自适应 2 MB/s 为目标：

| 指标 | 预算 | 依据 |
| --- | --- | --- |
| 单帧 JPEG 解码 | ≤2.5 ms/屏 | 400×240 / 320×240，`BitmapFactory` |
| 行还原 + LZ4 | ≤1 ms | 127 KB 上限、15 行 memcpy |
| 合成（GPU） | ≤1 ms | 单 Surface，一次 blit |
| 音频 | 4.9 ms/块 | 160 样本 @32.7 kHz |
| 端到端延迟 | 120–180 ms（目标） | 100 ms 播放缓冲 + 解码 + AudioTrack；PC 实测 110–150 ms |
| 引擎常驻内存 | ≤80 MB | 4 组参考帧 + 6 帧显示队列 + 4 MB socket 缓冲 |
| 前台服务 CPU | 30 fps 时 <15% 单核 | 与 PC 同算法 |

**显示队列**：PC 端最多缓存 64 帧、1 s 跨度，Android 上按延迟裁剪为 `ceil(delay/period)+2` 帧（默认 5 帧），避免 2× 画布下的内存峰值。

## 6. 权限与系统集成

| 项 | 用途 |
| --- | --- |
| `INTERNET`、`ACCESS_NETWORK_STATE`、`ACCESS_WIFI_STATE` | 局域网收发与网络类型判断 |
| `FOREGROUND_SERVICE` + `foregroundServiceType="mediaPlayback"` | 后台持续收流 |
| `POST_NOTIFICATIONS`（13+） | 前台服务通知 |
| `WAKE_LOCK` | 仅"保持屏幕常亮"开启时持锁 |
| `WifiManager.WifiLock(HIGH_PERF)` | 推流期间保持 Wi-Fi 活跃 |
| `MediaStore`（10+ 免存储权限） | 保存录制与截图 |

不申请：定位、相机、麦克风、存储读写（`WRITE_EXTERNAL_STORAGE` 仅在 API <29 且使用旧路径时可选，建议直接走 MediaStore 并放弃旧机型）。

**电池与后台**：首次连接时提示"允许后台活动"，检测到厂商省电策略时给出跳转；关闭屏幕会停止渲染但保留音频（可配置）。

## 7. 里程碑

| 阶段 | 交付 | 预估 |
| --- | --- | --- |
| **M0 骨架** | Flutter 工程、主题、导航、前台服务空壳、设置持久化、桥接契约 | 2–3 天 |
| **M1 能看** | 发现/配对、`R10P/F` 重组 + XOR、JPEG 解码、双屏合成、`Texture`、`AudioTrack`、最后一帧保持 | 1.5–2 周 |
| **M2 稳** | 行复用、R19 修复、R20 LZ4、分屏 R3 兼容、播放缓冲与截止时间、自适应码率、能力协商与重连 | 1.5–2 周 |
| **M3 好用** | 画质面板、诊断页、录制、截图、分享、横屏手势、多掌机记忆 | 1 周 |
| **M4 打磨** | 动效、无障碍、深色校准、中英文、崩溃/日志导出、签名与发布 | 1 周 |

M1 完成即可日常使用；M2 决定"能不能稳定 30 fps"，是体验分水岭。

## 8. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 部分 ROM 限制广播/省电杀后台 | 发现不到掌机、连上就断 | 前台服务 + WifiLock；固定 IP 作为一等公民；检测到广播失败时提示输入 IP |
| UDP 丢包与 `SO_RCVBUF` 默认值偏小 | 高码率下缺片 | 4 MB 缓冲 + 独立收包线程；行复用与 R19 修复优先启用 |
| `Texture` 与合成时序抖动 | 画面撕裂/卡顿 | 引擎内单队列、双缓冲 Surface；渲染与解码解耦 |
| 录制音画不同步 | 回放不可用 | 以音频块与视频帧共用的采集时钟为 PTS，MediaMuxer 统一时间基 |
| 设备碎片化（GPU/解码器） | 个别机型解码失败 | JPEG 用 `BitmapFactory` 软解（尺寸小），合成用 `Canvas` 硬件加速；失败回退软合成 |
| 手机发热/耗电 | 长时间投流掉帧 | 24 FPS / 低带宽预设、屏幕常亮可关、通知栏显示温度无关的帧率降级提示 |
| 无鉴权（与 PC 一致） | 同网段可被投流 | 设置页明示"仅可信局域网"；可选 4 位配对码（后续协议扩展，需两端同步） |

## 9. 测试策略

- **协议单测**（Kotlin/JVM）：用 PC 侧 `live/tests/native/` 的向量构造 fixture，覆盖重组、单洞 XOR 恢复、行还原（含 RST 校验失败）、LZ4 字典边界、修复超时、会话切换清理。
- **黄金图**（Flutter）：沿用 cia-bridge 的做法，`flutter test` 渲染连接页/直播页/设置有/无数据态，浅色/深色，视口 412×915 与 915×412。
- **集成**：用 PC 的 UDP 回放脚本（可用 `--stats` 抓包或自写发送器）驱动 Android 引擎，校验收齐率与延迟。
- **真机矩阵**：至少覆盖 Android 9/12/14、60 Hz 与 120 Hz 屏、Wi-Fi 5 与 6、蓝牙耳机。

## 10. 交付物与后续

本轮交付：本评估、[UI-SPEC.md](UI-SPEC.md)（设计令牌、页面与组件规范）、四个页面的高保真预览（[images/](images/)）与可编辑的 HTML 设计稿（[mockups/](mockups/)）。
下一步建议按 M0 落地：创建 `android/` Flutter 工程、`app_theme.dart`、导航骨架与桥接契约，再进 M1 的收流内核。

## 11. 本地构建环境（2026-11-25 实测）

工程可用的 SDK 都在本机已有目录里，不需要另装：

| 用途 | 路径 | 版本 |
| --- | --- | --- |
| Flutter | `D:\Games\Old_Times\3ds\cia-bridge\.toolchains\flutter-sdk\flutter` | 3.47.9 stable / Dart 3.13.7 / DevTools 2.60.0 |
| Android SDK | `F:\work\music_online_apk\.toolchains\android-sdk` | platform-tools 37.0.1、build-tools 28.0.3/34/35/36、platforms 35/36、NDK 28.2、cmdline-tools latest |
| JDK | `F:\work\music_online_apk\.toolchains\jdk17` | OpenJDK 17.0.20.1 |
| Gradle | `D:\Games\Old_Times\3ds\cia-bridge\.toolchains\gradle-cache` | 9.3.1（wrapper 缓存）；首次构建约下载 1.8 GB 依赖 |
| devkitPro | `D:\Games\Old_Times\3ds\toolchain\sdk\opt\devkitpro` | devkitARM GCC 16.1.0 / binutils 2.46.0 / newlib 4.6.0、libctru 2.7.0、citro3d 1.7.1、citro2d 1.7.0、3dstools 1.3.1 |

Flutter 构建需要显式指定 JDK 与 SDK（系统 `JAVA_HOME` 是 JDK 8，AGP 不能使用）：

```powershell
$env:JAVA_HOME   = 'F:\work\music_online_apk\.toolchains\jdk17'
$env:ANDROID_HOME = $env:ANDROID_SDK_ROOT = 'F:\work\music_online_apk\.toolchains\android-sdk'
& 'D:\Games\Old_Times\3ds\cia-bridge\.toolchains\flutter-sdk\flutter\bin\flutter.bat' build apk --debug
```

`flutter doctor` 在本机报告 Android toolchain 正常（Android SDK 36.0.0、JDK 17），空模板可完成 `assembleDebug`。掌机端构建用仓库自带脚本，需要 MSYS bash：

```powershell
.\scripts\build-handheld.ps1 -Bash 'C:\Program Files\Git\bin\bash.exe'
```

该脚本默认使用上面的 devkitPro SDK，产出 `sysmod/3dslive.cxi`、`app/3ds-live.3dsx` 与 `app/build/3ds-live.cia`；实测干净重建分别得到 248,832 / 657,304 / 566,208 字节，SMDH 版本为 `(v0.1.12)`，与发布包一致。

## 12. 实现状态（2026-11-25）

`android/` 已落地为 Flutter 界面 + Kotlin 收流引擎，`flutter analyze`、6 个 widget 测试、23 个 JVM 协议测试与 `flutter build apk --debug` 全部通过。

| 里程碑 | 状态 | 说明 |
| --- | --- | --- |
| M0 骨架 | 完成 | 工程、主题、导航、通道契约、设置持久化 |
| M1 能看 | 完成（待真机） | 发现、`R10P/F` 重组 + XOR、JPEG 解码、Texture 合成、`AudioTrack` |
| M2 稳 | 完成（待真机） | 行复用、R19 修复、R20 LZ4、`R3P2`/`R3F2` 分屏与自适应码率已实现；`R15P` 旧格式未接 |
| M3 好用 | 部分完成 | 画质弹层、诊断页、截图、横屏与手势、MP4 录制完成；相册/分享入口待做 |
| M4 打磨 | 未开始 | 动效微调、国际化、发布签名 |

已知限制与下一步：

1. **真机联调尚未进行**：本机没有 Android 设备或模拟器，协议层已用 JVM 测试与 liblz4 交叉验证，仍需要用真掌机确认时延与丢包表现。
2. **录制**：已用 `MediaCodec` + `MediaMuxer` 实现（H.264 表面输入 + AAC），Android 10+ 会发布到相册 `Movies/3DS Live`；分享入口待补。
3. **旧格式**：`R3P2`/`R3F2` 已支持，`R15P`/`R15F` 未处理，只影响 0.1.0 之前的旧掌机端。
4. **自适应码率**：已按 3 秒降档、8 秒升档实现（`RateController`），尚未用真机丢包样本校准阈值。
5. **效果**：FBCNN/SPAN/RIFE 不做；如需轻度锐化，可在 `VideoSink` 加 GPU shader。

验收方式：先在掌机就绪画面按 `R + SELECT`，手机点"开始寻找掌机"，确认进入直播页后检查底栏 FPS/Q/带宽与诊断页的收齐 FPS；再切到后台确认通知与声音继续。
