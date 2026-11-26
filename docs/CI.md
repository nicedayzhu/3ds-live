# GitHub Actions 构建与发布

## 工作流

- `.github/workflows/ci.yml`：push / PR 运行协议测试、kernel style、3DS 构建和 Android APK。
- `.github/workflows/release.yml`：`v*.*.*` 标签触发正式发布，也可以手动输入已有标签重建。

发布流水线只产出一个 Windows 包。TensorRT 与 DirectML 使用同一个 `3ds-live-client.dll`：
有 TensorRT plan 时走 TensorRT，否则回退 DirectML。不要再上传第二个 Windows 包。

## 为什么需要自托管 runner

GitHub 托管的 `windows-latest` 没有 NVIDIA GPU、CUDA 和 TensorRT SDK，只能构建 DirectML 版本。
TensorRT 构建需要：

- TensorRT 10 的头文件、`nvinfer_10.dll`；
- CUDA 12.9 头文件、`cudart_static.lib`、`cudart64_12.dll`；
- 已为本机 GPU 生成的 `fbcnn-fp16-*.plan` / `rife-*.plan`。

因此 Windows 构建默认使用 `windows-latest`（DirectML），也可以切换到一台安装了上述环境的
Windows 自托管 runner。

## 配置自托管 TensorRT runner

1. 在 GitHub 仓库 `Settings → Actions → Runners → New self-hosted runner` 下载 Windows runner，
   给它加上 `self-hosted`、`windows`、`tensorrt` 标签。
2. runner 机器安装 Visual Studio 2022 C++、Qt 6.8+、CUDA 12.9、TensorRT 10，并准备
   `runtime/acceleration` 目录（DLL + plan）。
3. 在 GitHub 仓库 `Settings → Secrets and variables → Actions → Variables` 设置：

```text
WINDOWS_RUNNER=["self-hosted","windows","tensorrt"]
TENSORRT_ROOT=D:\path\to\TensorRT
ACCELERATION_DIR=D:\path\to\acceleration
CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9
```

`CUDA_PATH` 也可以直接配在 runner 机器环境变量里。发布工作流会自动：

```powershell
.\live\qt\build.ps1 -QtSdk ... -SkipTests -Models ... `
  -TensorRT -AccelerationDirectory $env:ACCELERATION_DIR
```

产出的唯一 Windows zip 已包含 TensorRT DLL、CUDA runtime 和 plans。

## 本地验证

```powershell
.\live\qt\build.ps1 -QtSdk .toolchains\qt\6.10.3\msvc2022_64 `
  -Models D:\path\to\models `
  -TensorRT -AccelerationDirectory D:\path\to\acceleration
```

启动包内 `3DS-Live.exe`，在 F12 性能页确认 FBCNN 后端为 `TensorRT`；没有 plan 或 GPU 时会显示
DirectML 回退，不会导致应用不可用。

## 没有自托管 runner 时

保留 `windows-latest` 作为兼容构建即可，但该包只有 DirectML，不适合作为性能发布包。
不要手工把第二个 TensorRT zip 追加到同一个 Release；应配置 runner 后重新跑 Release 工作流。
