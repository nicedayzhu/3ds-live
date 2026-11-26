# 3DS Live 协议标记

所有报文以 4 字节 ASCII magic 开头，后面是 little-endian `u32` 字段。
这些不是标准缩写，而是内部协议标签：

```
R 16 C
│  │  └─ 报文种类/方向
│  └──── 线协议修订号
└─────── 协议族（3DS Live）
```

## 后缀含义

| 后缀 | 含义 | 示例 |
| --- | --- | --- |
| `C` | Command / Configure，接收端发给掌机的命令 | `R16C`、`R23C`、`R21C` |
| `A` | Acknowledge，确认/宣告 | `R17A`、`R19A`、`R20A` |
| `S` | Status / Support，掌机状态或能力 | `R7ST`、`R9ST`、`R16S`、`R17S`、`R19S`、`R20S`、`R21S` |
| `T` | Totals / Timing，累计或计时快照 | `R17T`、`R19T`、`R21T` |
| `P` | Payload，视频数据分片 | `R10P`、`R19P`、`R3P2` |
| `F` | FEC parity，校验分片 | `R10F`、`R3F2` |
| `N` | Negative request，NACK/补片请求 | `R19N` |
| `D` | Delta / Debug，增量或调试标记 | `R17D`、`R20D`、`R12D` |
| `K` | Key，行复用参考关键帧 | `R17K` |
| `J` | JPEG compact，紧凑 JPEG 头 | `R8J1` |
| `B` | Feedback，接收端回传统计 | `R3FB` |

## 你问到的五个标签

| 标签 | 方向 | 作用 |
| --- | --- | --- |
| `R16C` | PC/Android → 3DS | 主配置命令：session、序列号、fps、码率、combined FEC、JPEG Q；v2 末尾追加屏掩码 |
| `R23C` | PC/Android → 3DS | 只改屏掩码（1=上屏，2=下屏，3=双屏） |
| `R17A` | PC/Android → 3DS | 行复用参考确认：`flags` + `seq`，确认 key/candidate |
| `R19A` | PC/Android → 3DS | 宣告接收端支持 repair（R19N/R19P） |
| `R3FB` | PC/Android → 3DS | 接收端反馈：发送序号、传输时延等，用于拥塞/节奏判断 |

## 媒体与状态

| 标签 | 作用 |
| --- | --- |
| `R3P2` | 单屏 JPEG 数据（每屏 2 条 FEC） |
| `R3F2` | 单屏 FEC 校验分片 |
| `R10P` | 双屏组合 JPEG 数据（4 条 FEC） |
| `R10F` | 双屏组合 FEC 校验分片 |
| `R19P` | 定向补片 |
| `R19N` | 补片请求（NACK） |
| `R7ST` / `R9ST` | 掌机状态（无/有音频字段） |
| `R16S` / `R16Q` | JPEG Q 配置 / Q70 能力 |
| `R17T` | 累计媒体字节 |
| `R19T` | repair/发送诊断 |
| `R21S` / `R21T` | pacing 模式 / 包计时快照 |
| `R12D` | `LIVE_FRAME_TIMING` 调试标记 |

新增或修改标签时必须保持字段顺序、长度和字节序；这些标签同时被
PC（`live/qt`）和 Android（`stream/Wire.kt`）解析。
