# 3DS 端模块结构

本文件记录 `sysmod/`、`app/` 与共享 codec 在 kernel 风格重构后的职责边界。
目标是让协议解析、网络、音频、统计和 launcher 生命周期不再依赖
`live_stream.c` 的文件级状态。

## 模块

| 模块 | 职责 | 主机测试 |
| --- | --- | --- |
| `live_config.c` | `live.ini` 默认值、解析、校验 | `test_live_config` |
| `live_control.c` | R16C/R23C/R21C/R17A/R19A/R19N/R3FB 解析与 ops 分发 | `test_live_control` |
| `live_status.c` | R7ST/R9ST 及 R16S/16Q/17T/17S/20S/19S/19T/21S/21T 布局 | `test_live_status` |
| `live_audio_sender.c` | 音频队列、IMA ADPCM、冗余批次、R9A1 发送 | — |
| `live_net.c` | socket、发现、探测、租约、网络诊断 | — |
| `live_net_report.c` | `network-status.txt` 单消费者写入 | `test_live_net_report` |
| `live_stats.c` | `live-stats.txt` 快照格式化 | `test_live_stats` |
| `live_row.h` | 行复用锚点/计划类型 | — |
| `live_r20.c` | R20 增量 + LZ4 字典 | — |
| `live_repair_packet.c` | R19P 32 字节头 | `test_live_repair_packet` |
| `live_repair_tx.c` | repair credit、空闲时隙发送、packet report | — |
| `launcher_module.c` | 模块发现、安装、状态轮询、页面切换 | — |

## 仍位于 `live_stream.c` 的核心

- 线程生命周期与资源分配（`live_start` / `live_stop`）；
- capture 快照、编码路径（single/pair）；
- 发送队列、FEC、repair、pacing；
- row key/candidate 协商与发布；
- 状态机对上述模块的编排。

这些部分共享 pending 队列、repair arena、row anchor 和编码工作区，尚未收敛到
单一 `struct live_stream`。后续若继续拆 `live_encoder` / `live_sender`，应先定义
该状态结构，再把模块改为显式接收指针。

## 风格与检查

```powershell
pwsh -File scripts/check-3ds-style.ps1
```

检查范围：`sysmod/`、`app/`、`codec/`。规则见 `docs/CODING_STYLE.md`。

## 验证

```powershell
# 3DS
pwsh -File scripts/build-handheld.ps1 -Bash F:\Tools\msys64\usr\bin\bash.exe

# 主机协议测试
make -C live/tests/native all
# Windows 下用 scripts/verify-all.ps1 统一构建/运行

# PC
pwsh -File live/qt/build.ps1 -QtSdk .toolchains/qt/6.10.3/msvc2022_64

# Android
cd android
flutter analyze
cd android && ./gradlew testDebugUnitTest
```

2026-11-26 的重构批次结果（`scripts/verify-all.ps1`）：

- 3DS 交叉编译通过；
- kernel style 75 个文件通过；
- native tests 23/23 通过；
- PC 客户端构建通过；
- Android analyze + unit test 通过。

当前 `live_stream.c` 约 2120 行，已完成：

- `control()` / `control_receive()` / `send_status_burst()` 分离；
- `encoder()` / `encoder_bottom_worker()` / `encoder_read_plan()` / `encoder_snapshot_raw()`；
- `encode_pair()` → `pair_requantize()` / `pair_publish()`；
- `sender()` / `sender_prepare()` / `sender_purge_pending()` / `sender_select_key()` / `sender_copy_panels()` / `sender_send_frame()`；
- `live_start()` / `stream_reset()` / `stream_open_resources()`；
- `live_stop()` / `stream_join_threads()` / `stream_close_resources()` / `stream_write_stats()`。

尚未完成：掌机实机回归、`live_encoder` / `live_sender` 文件级拆分、
`struct live_stream` 状态收敛。
