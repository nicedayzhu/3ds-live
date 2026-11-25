# Android 设计稿

`docs/android/` 里的两份规范用到的界面预览都在这里生成：

| 文件 | 内容 |
| --- | --- |
| `connect.html` … `diagnostics.html` | 可在浏览器直接打开的静态稿（HTML/CSS，使用 UI-SPEC 的令牌） |
| `generate_mockups.py` | 生成上面四个 HTML |
| `render_png.py` | 用 Pillow 按同一套几何与令牌渲染 `../images/*.png` |
| `../images/*.png` | 412×915 @2x 预览图，供 Markdown 引用 |

修改设计时先在 `generate_mockups.py` 或 `render_png.py` 里改，再同步 `../UI-SPEC.md` 的令牌与尺寸。两套渲染共用同一组数值，不要只改其中一边。

```powershell
cd docs/android/mockups
python generate_mockups.py   # 更新 HTML
python render_png.py         # 更新 PNG
```

这些是设计稿，不是应用代码；真正的 Flutter 工程按 [DESIGN.md](../DESIGN.md) 的 M0 建立。
