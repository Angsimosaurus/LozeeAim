# 第三方声明

Language: [English](THIRD_PARTY_NOTICES.md) | 简体中文

本仓库中的源代码会集成若干第三方组件。每个组件均受其自身许可证和分发条款约束。

## Dear ImGui

- Project: Dear ImGui
- Use: Immediate mode GUI and DirectX 11 / Win32 backend integration
- Upstream: https://github.com/ocornut/imgui
- License: MIT License

## OpenCV

- Project: OpenCV
- Use: Image preprocessing and matrix operations
- Upstream: https://github.com/opencv/opencv
- License: Apache License 2.0

## ONNX Runtime

- Project: ONNX Runtime
- Use: ONNX model inference
- Upstream: https://github.com/microsoft/onnxruntime
- License: MIT License

## DirectML

- Project: Microsoft DirectML
- Use: DirectML execution provider runtime dependency
- Upstream: https://www.nuget.org/packages/Microsoft.AI.DirectML
- License: See Microsoft package license terms

## NVIDIA CUDA, cuDNN, and TensorRT

- Projects: CUDA Toolkit, cuDNN, TensorRT
- Use: Optional TensorRT inference acceleration
- Upstream: https://developer.nvidia.com/
- License: NVIDIA license terms apply. These binaries are not included in this repository.

## Windows SDK and DirectX 11

- Project: Microsoft Windows SDK
- Use: Win32, DXGI, D3D11, and system APIs
- License: Microsoft SDK terms apply

## 模型文件

YOLO 和 NN 模型文件不包含在本仓库中。用户需要自行确保所使用的模型文件具备相应的使用和分发权利。
