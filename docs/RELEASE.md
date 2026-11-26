# 发布流程

正常发版不需要修改或新增 GitHub Actions workflow。

## 1. 提升版本

```powershell
pwsh -File scripts/bump-version.ps1 0.1.14
```

会同步：

- 根目录 `VERSION`
- `android/pubspec.yaml` 的 `version: x.y.z+build`
- `android/lib/version.dart` 的开发默认值

## 2. 提交并打标签

```powershell
git add -A
git commit -m "release: 0.1.14"
git tag -a v0.1.14 -m "3DS Live 0.1.14"
git push origin main
git push origin v0.1.14
```

推送 `v0.1.14` 后，`.github/workflows/release.yml` 自动：

- 构建 3DS、Windows、Android；
- 组装 Windows/SD/CIA；
- 生成校验和；
- 创建 GitHub Release。

不需要改 `release.yml` 里的版本号，也不用手工上传包。

## 3. 模型版本

Windows 构建需要 ONNX 模型。它从 Actions 仓库变量 `MODELS_TAG` 指向的 Release 下载模型：

```text
MODELS_TAG=v0.1.13
```

默认是 `v0.1.13`。只有模型文件变化时才改这个变量：

```text
Settings → Secrets and variables → Actions → Variables → MODELS_TAG
```

普通应用版本升级不用动它。

更长期可以把模型放到独立的 `ci-models` Release，避免依赖应用 Release 的 Windows 资产。
