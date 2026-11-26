# GitHub Actions 构建与发布

## 工作流

- `.github/workflows/ci.yml`：push / PR 运行协议测试、kernel style、3DS 构建和 Android APK。
- `.github/workflows/release.yml`：`v*.*.*` 标签触发正式发布，也可以手动输入已有标签重建。

## Windows 后端

GitHub 托管 runner 使用 **DirectML/CPU** 后端，不包含 TensorRT plan。Release 页面的
Windows 包就是这一标准版本。

TensorRT 版依赖本机 CUDA/TensorRT 和为本机 GPU 生成的 plan，由本地构建：

```powershell
.\live\qt\build.ps1 -QtSdk .toolchains\qt\6.10.3\msvc2022_64 `
  -Models D:\path\to\models `
  -TensorRT -AccelerationDirectory D:\path\to\acceleration
```

需要 TensorRT 版本时联系作者 [@nicedayzhu](https://github.com/nicedayzhu)，不通过
GitHub Actions 发布第二包。

## 版本与模型

正常发版只需运行 `scripts/bump-version.ps1` 并推送标签，见 [RELEASE.md](RELEASE.md)。

Windows 构建从仓库变量 `MODELS_TAG` 指向的 Release 获取模型，默认 `v0.1.13`；只有模型变化时才更新该变量。

## 发布产物

每个 Release 包含：

- `3DS-Live-<version>-Windows-x64-New3DS.zip`（DirectML/CPU）
- `3DS-Live-<version>-SD.zip`
- `3DS-Live-<version>-Android-arm64.apk`
- `3ds-live.3dsx`
- `3ds-live.cia`
- `checksums-<version>.txt`

## 本地验证

```powershell
.\live\qt\build.ps1 -QtSdk .toolchains\qt\6.10.3\msvc2022_64 `
  -Models D:\path\to\models
```

启动 `3DS-Live.exe`，F12 性能页会显示 DirectML 后端。
