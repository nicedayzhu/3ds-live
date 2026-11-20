# 致谢与第三方许可

独立项目由 **nicedayzhu** 维护。感谢 **BolgarMaxym97** 开发并以 MIT 许可公开 **3ds-recorder**。本项目沿用了它的启动器内嵌系统模块设计、双屏帧缓冲 DMA 采集和 DSP 立体声采集实现。nicedayzhu 的双屏直播分支在此基础上增加了网络投屏；独立项目取自该分支的开发快照 `927f4ece60f3bae64d6a35d32104e78f50a07308`。[维护者](https://github.com/nicedayzhu) · [上游 3ds-recorder](https://github.com/BolgarMaxym97/3ds-recorder) · [Live 分支](https://github.com/nicedayzhu/3ds-recorder/tree/feature/dual-screen-live) · [项目 MIT 许可](../LICENSE)

| 项目 | 在 3DS Live 中的用途 |
| --- | --- |
| Luma3DS、devkitPro / libctru | Luma3DS 加载掌机后台模块，devkitARM 和 libctru 用于构建掌机程序和调用系统服务。[Luma3DS](https://github.com/LumaTeam/Luma3DS) · [libctru](https://github.com/devkitPro/libctru) |
| JPEGENC | 掌机 JPEG 编码器使用 Larry Bank（BitBank Software）的实现。随源修订为 `f51421bdaba8e098dc82130a17adaae6d61c3377`。[JPEGENC](https://github.com/bitbank2/JPEGENC) · [Apache 2.0 许可](../third_party/JPEGENC/LICENSE) |
| Noto Sans SC | 掌机启动器从这款字体生成中英文点阵字形。[字体来源](https://github.com/google/fonts/tree/main/ofl/notosanssc) · [字体许可](../app/assets/NotoSansSC-OFL.txt) |
| Qt、FFmpeg、OpenCV | 分别用于 Windows 播放器界面、媒体输出和图像处理。随包声明位于 `Windows/runtime/licenses/`。[Qt 运行库声明](../live/qt/licenses/NOTICE.txt) · [FFmpeg](https://ffmpeg.org/) · [OpenCV](https://opencv.org/) |
| ONNX Runtime、DirectML | Windows 通用包通过它们在兼容显卡上运行画质增强和补帧模型。[ONNX Runtime](https://onnxruntime.ai/) · [DirectML](https://github.com/microsoft/DirectML) |
| KAIR / DnCNN、FBCNN、Real-ESRGAN、Practical-RIFE | 分别提供降噪、JPEG 去块、超分和运动补帧模型。[作者仓库与各模型许可](../third_party/PCEnhance/README.md) |
| SPAN、NomosUni multiJPG | 实时 2× 超分采用 Cheng Wan 等人提出的 SPAN 架构，使用 Philip Hofmann 针对 JPEG 压缩训练的权重。[SPAN 论文](https://openaccess.thecvf.com/content/CVPR2024W/NTIRE/papers/Wan_Swift_Parameter-free_Attention_Network_for_Efficient_Super-Resolution_CVPRW_2024_paper.pdf) · [权重、许可及本项目的适配](../third_party/SPAN/README.md) |
| Azahar | 用于检查掌机启动器的中英文布局和设备状态显示。指南的启动器状态截图取自模拟器。[Azahar](https://github.com/azahar-emu/azahar) |

文档中的游戏画面展示投屏效果，游戏名称、角色和美术的权利归各自权利人所有。
