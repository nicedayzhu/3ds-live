# 2xNomosUni_span_multijpg

Philip Hofmann (Phips / Phhofm) trained this 2× SPAN model with JPEG degradation and published the FP16 and FP32 ONNX models. The model card lists 2,989 NomosUni training images and JPEG quality down to 40. Both models and the adapted copies distributed with 3DS Live are licensed under Creative Commons Attribution 4.0 International. [Author's model card](https://huggingface.co/Phips/2xNomosUni_span_multijpg/blob/ed6111c4cabdaf42b75e6535600d5370cce9e4e2/README.md) · [Author's release](https://github.com/Phhofm/models/releases/tag/2xNomosUni_span_multijpg) · [License](https://creativecommons.org/licenses/by/4.0/) · [Full license text](CC-BY-4.0.txt)

## 3DS Live adaptations

The project exposes the input as `image` and the output as `output`. Both are FP32 RGB tensors in NCHW layout. Input RGB values range from 0 to 1. Batch size is 1, input height is 240 and output height is 480. The width axes are named `width` and `output_width` and the output width is twice the input width. One graph per precision serves both 320- and 400-pixel-wide screens.

The FP32 adaptation changes names and shape annotations. The FP16 adaptation also casts the input to FP16 before inference and the result back to FP32. Both retain the author's trained weights.

| Precision | Original ONNX download | Original SHA-256 | Packaged filename | Packaged SHA-256 |
| --- | --- | --- | --- | --- |
| FP16 | [Author's FP16 export](https://github.com/Phhofm/models/releases/download/2xNomosUni_span_multijpg/2xNomosUni_span_multijpg_fp16_opset17.onnx) | `c5d3c852d42f80d60b48d4cdefdc6c9254a247625b3289eb59f5a2ce3a9327ec` | `span-fp16.onnx` | `c9d47727c43d4eb9c22b5a793ffc99da1203b111755f4c1240d5d6f80e5615a4` |
| FP32 | [Author's FP32 export](https://github.com/Phhofm/models/releases/download/2xNomosUni_span_multijpg/2xNomosUni_span_multijpg_fp32_opset17.onnx) | `4c088922fcb40584a95a1b98009e3042a61ada46f237a9e72e8a2ed127832004` | `span.onnx` | `fea936061ceb637ae7f0a2b1f4603ab45556222884cd8faf2045f49b2d1cba4b` |

## Architecture

Cheng Wan and coauthors introduced SPAN in *Swift Parameter-free Attention Network for Efficient Super-Resolution* at CVPR Workshops 2024. The authors publish their reference implementation under the Apache License 2.0. [Paper](https://openaccess.thecvf.com/content/CVPR2024W/NTIRE/papers/Wan_Swift_Parameter-free_Attention_Network_for_Efficient_Super-Resolution_CVPRW_2024_paper.pdf) · [Reference implementation](https://github.com/hongyuanyu/SPAN/tree/c77a5917759f09e66fbc7124220c5afc5ee221e5) · [Upstream license](https://github.com/hongyuanyu/SPAN/blob/c77a5917759f09e66fbc7124220c5afc5ee221e5/LICENSE.txt) · [Bundled Apache license](SPAN-Apache-2.0.txt)
