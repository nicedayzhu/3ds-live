# 开发与构建

3DS Live 由掌机启动器、后台采集模块和 Windows Qt 播放器组成。安装成品和连接掌机的步骤见[使用指南](../INSTALL.md)。

| 目录 | 职责 |
| --- | --- |
| `app/` | 安装和启动内置系统模块，显示设备与连接状态 |
| `sysmod/` | 响应游戏中的热键，采集双屏和 DSP 混音，通过 UDP 发现播放器并发送音视频 |
| `codec/` | 编解码、分片、纠错、码率控制和发现报文 |
| `live/qt/` | Qt/C++ 播放器、画质增强、诊断、RTSP 和 PC 端 MP4 输出 |
| `live/tests/native/` | 在开发机上运行的 C 测试 |

## 构建掌机程序

准备 devkitARM、libctru、3dsxtool、smdhtool、bannertool、makerom、GNU Make 和带 Pillow 的 Python 3。在已配置 `DEVKITPRO`、`DEVKITARM` 和 `PATH` 的 devkitPro shell 中运行 `make handheld`。Windows 也可以从项目根目录在 PowerShell 中指定 SDK 和 MSYS2 Bash 的实际位置。

```powershell
.\scripts\build-handheld.ps1 -Sdk D:\devkitPro -Bash D:\devkitPro\msys2\usr\bin\bash.exe
```

产物为 `dist/3ds-live.3dsx` 和 `dist/3ds-live.cia`。两者均在 RomFS 中内置本次构建的 CXI，启动器会检查、安装或更新系统模块。构建时从 HOME 菜单图标 `app/assets/icon.png` 生成启动器左上角的 Logo。

## 构建 Windows 绿色版

准备 PowerShell 7、Visual Studio C++ x64、CMake、Qt 6 MSVC x64 和 7-Zip。构建脚本校验固定版本的依赖和模型，编译播放器并运行 Qt 测试。将 `-Models` 指向已有标准包的 `Windows/runtime/models` 或本地模型目录；使用已有构建缓存时可省略。下列路径为示例。

```powershell
.\live\qt\build.ps1 -QtSdk C:\Qt\6.10.3\msvc2022_64 -Models D:\Models\3ds-live
```

从项目内的 `dist/Windows/3DS-Live.exe` 启动播放器。标准包使用 DirectML。要编译可选 NVIDIA 后端，将 `CUDA_PATH` 指向 CUDA SDK、`TensorRT_ROOT` 指向 TensorRT SDK，并传入 `-TensorRT`。使用 `-AccelerationDirectory <目录>` 可部署本机匹配的引擎与运行库。[播放器参数与模型兼容性](../live/qt/README.md)

## 版本和打包

独立项目的公开版本从 **v0.1.0** 起算，以项目根目录的 `VERSION` 为准。历史报告里的 1.2.x 是开发测试时的编号。已有测试机要安装新 CIA 时，安装器比较的是 CIA 内部的更新序号；该序号与公开版本分开维护，以便旧测试版可以直接升级。

在掌机和 PC 产物都准备好后，从项目根目录运行以下命令。脚本在 `dist/` 生成发布 ZIP 和 SHA-256 校验文件，并逐项回读校验 ZIP。包内的 `Windows/` 是绿色版播放器，`SD/` 存放按 SD 卡路径排列的 3DSX 和可选配置模板，`3ds-live.cia` 用于从 HOME 菜单启动，`licenses/` 存放项目和掌机组件的许可证。顶层的 `使用指南.html` 由 `INSTALL.md` 生成，内嵌两张配图，解压后可用浏览器离线阅读。

```powershell
.\scripts\package.ps1
```

本地工作副本可以在 `.git/info/3ds-live-desktop-output` 保留用于恢复发布目录 NTFS 完整性标签的空目录。新建的 `dist/Windows` 仍从父目录继承访问权限，其他工作副本按默认方式创建输出目录。

## 测试与参考

- 开发机上使用本机 C 编译器运行 `make test`，检查协议、编解码和发现租约。Windows 构建脚本会运行播放器的 Qt 测试。
- 两端等待连接时每秒探测一次。掌机向 PC 的媒体端口（默认 UDP 8001）发请求，PC 向掌机的 UDP 8002 发邀请。连续约四秒未收到确认就重新寻找播放器。[协议说明](../live/docs/PROTOCOL.md)
- 真机部署、丢帧和不同码率的记录可用于回归对照；各报告保留当时的输入、软件版本和测量条件。[真机赛车测试](../live/docs/HARDWARE-1.2.8.md) · [掌机源画质](../live/docs/SOURCE-QUALITY.md) · [双屏超分评估](../live/docs/SUPER-RESOLUTION.md)

Git 提交标题使用英文 `type(scope): summary`，例如 `fix(pc): restore video when an AI model fails`。范围使用 `3ds`、`pc` 或 `3ds-pc`。文档和构建改动的类型分别使用 `docs` 和 `build`。
