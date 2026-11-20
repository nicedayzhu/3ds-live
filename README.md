# 3DS Live

在 Windows 上用 Qt 观看 New Nintendo 3DS 系列掌机的上下屏和立体声。掌机开始推流后通过局域网寻找播放器，双方交换发现报文，再通过单播传输音视频。播放器可以先于或晚于掌机启动。安装、旧版迁移和自动重连见[使用指南](INSTALL.md)。

## 构建

**掌机**使用 devkitARM、libctru、3dsxtool、smdhtool、makerom 和 GNU Make。在已设置 `DEVKITPRO`、`DEVKITARM` 和 `PATH` 的 devkitPro shell 中运行 `make handheld`。Windows 也可以从 PowerShell 运行以下命令。

```powershell
.\scripts\build-handheld.ps1 -Sdk D:\devkitPro -Bash D:\devkitPro\msys2\usr\bin\bash.exe
```

产物在 `dist/SD`。系统模块和 Homebrew Launcher 启动器均由本项目编译。可选配置模板随 SD 包分发。使用本机的 C 编译器运行 `make test` 可检查协议、编解码和发现租约。

**Windows**使用 PowerShell 7、Visual Studio C++ x64、CMake、Qt 6 MSVC x64 和 7-Zip。标准包使用 DirectML。构建脚本下载并校验固定版本的依赖，校验模型，生成播放器并运行 Qt 测试。模型可取自本项目二进制包中的 `Windows/runtime/models`。在已有本地构建缓存时可省略 `-Models`。[Qt 播放器说明](live/qt/README.md)

```powershell
.\live\qt\build.ps1 -QtSdk C:\Qt\6.10.3\msvc2022_64 -Models D:\Models\3ds-live
.\scripts\package.ps1
```

本机使用时，直接双击项目内的 `dist/Windows/3DS-Live.exe`。掌机 SD 卡所需文件在 `dist/SD`。打包命令将使用指南、许可证、完整 ZIP 和对应的 SHA-256 校验文件写入同一项目的 `dist`，并逐项回读校验 ZIP。

编译可选的 NVIDIA 后端时，将 `CUDA_PATH` 指向 CUDA SDK，`TensorRT_ROOT` 指向 TensorRT SDK，再向 Qt 构建脚本传 `-TensorRT`。如需一并部署本机配套引擎和运行库，传 `-AccelerationDirectory <加速目录>`。通用 ZIP 携带 DirectML 模型，本机兼容的 TensorRT 文件可放在 `Windows/runtime/acceleration/`。

## 源码

| 目录 | 职责 |
| --- | --- |
| `app/` | 启动指定的 Live 系统模块，显示就绪信息 |
| `sysmod/` | 游戏中热键、双屏与 DSP 混音采集、UDP 发现和发送 |
| `codec/` | 直播编解码、分片、纠错、码率及发现报文 |
| `live/qt/` | Qt/C++ 接收、播放、画质增强、诊断、RTSP 和 PC 端 MP4 输出 |
| `live/tests/native/` | 主机可执行的 C 测试 |

双方等待连接时每秒探测一次，PC 向掌机的 UDP 8002 发送邀请，掌机向 PC 的媒体端口（默认 UDP 8001）发送发现请求。连续约四秒未确认就重新寻找播放器。音视频发往选中的单个 PC。[协议说明](live/docs/PROTOCOL.md)

本项目从 `3ds-recorder` 的 Live 分支提交 `927f4ece60f3bae64d6a35d32104e78f50a07308` 提取。JPEG 编码器使用随源附带的 JPEGENC 修订 `f51421bdaba8e098dc82130a17adaae6d61c3377`。原作者许可和 PC 运行库声明一并保留。[项目许可](LICENSE) · [JPEGENC 许可](third_party/JPEGENC/LICENSE) · [模型许可](third_party/PCEnhance/README.md) · [Qt 运行库声明](live/qt/licenses/NOTICE.txt)
