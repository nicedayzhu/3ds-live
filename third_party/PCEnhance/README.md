# Windows 播放器模型与许可

发布包中的 ONNX 模型供 Qt 播放器在本机运行。构建时按 `models.sha256` 核对模型文件；随包的清单和两张 FBCNN 参考图在 `Windows/runtime/models/`。

| 模型 | 用途 | 来源与许可 |
| --- | --- | --- |
| DnCNN3 | 亮度降噪 | [KAIR](https://github.com/cszn/KAIR) · [MIT](KAIR-LICENSE) |
| Real-ESRGAN AnimeVideo-v3（SRVGGNetCompact） | 动画画面超分 | Copyright 2021 Xintao Wang · [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) · [BSD 3-Clause](RealESRGAN-LICENSE) |
| 2xNomosUni_span_multijpg（SPAN） | JPEG 修复与原生 2× 超分，含 FP16 和 FP32 | Philip Hofmann · [模型来源及适配说明](../SPAN/README.md) · [CC BY 4.0](../SPAN/CC-BY-4.0.txt) |
| FBCNN color | 原生分辨率 JPEG 去块 | [FBCNN](https://github.com/jiaxi-jiang/FBCNN) · [Apache 2.0](FBCNN-LICENSE) |
| RIFE 4.25 lite | 上下屏运动补帧 | [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) · [MIT](RIFE-LICENSE) |

标准包使用 ONNX Runtime、DirectML 和 OpenCV，其许可随运行库分发。可选的 TensorRT 引擎与特定 GPU 和运行库版本有关，加速目录附有 NVIDIA 的 TensorRT 和 CUDA 许可。
