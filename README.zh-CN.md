# LozeeAim

Language: [English](README.md) | 简体中文

LozeeAim 是一个面向 Windows 的 C++20 计算机视觉叠加层与本地自动化研究项目。本仓库仅用于授权测试、本地实验和学习导向的工程研究。

## 范围

此仓库包含 Win32 `SendInput` 鼠标后端。

仓库不包含模型权重、运行时 DLL 或 TensorRT engine cache。

## 功能

- 基于 DirectX 11 的透明叠加层和 ImGui UI。
- DXGI 桌面捕获流程。
- ONNX Runtime 推理，支持 DirectML、CPU 和可选 TensorRT。
- YOLO 模型热重载。
- 目标选择、目标预测和可配置的瞄准平滑算法。
- NN 轨迹数据采集、训练、ONNX 导出和测试叠加层。
- 中文与 English UI 语言选项。

## 软件截图

![LozeeAim 关于页](docs/images/lozeeaim-about.png)

## 环境要求

- Windows 10/11 x64。
- Visual Studio 2022 或更新版本，包含 MSVC C++20 工具链。
- Windows SDK。
- OpenCV 4.12.x。
- ONNX Runtime 1.23.x 或兼容版本。
- 可选：CUDA、cuDNN 和 TensorRT，用于 TensorRT 推理。

## 构建配置

Visual Studio 项目读取以下环境变量：

```text
OPENCV_ROOT=C:\path\to\opencv
ONNXRUNTIME_ROOT=C:\path\to\onnxruntime
TENSORRT_ROOT=C:\path\to\TensorRT
CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.x
```

然后执行构建：

```powershell
msbuild LozeeAim.slnx /p:Configuration=Release /p:Platform=x64
```

运行时 DLL 不会提交到仓库。请使用应用内依赖管理器，或根据各依赖项许可证要求，将必要运行时文件放在可执行文件旁边。

## 单文件分发

单文件发布需要明确保留第三方许可证信息。应用可以作为一个主可执行文件分发，但模型文件和 GPU/运行时二进制文件可能仍需保持外置，除非你拥有相应的再分发权利，并且打包方式能够保留许可证声明。

如果你选择在启动时嵌入或释放运行时文件，请确保用户仍可访问 `THIRD_PARTY_NOTICES.zh-CN.md`，不要把依赖许可证隐藏在二进制文件内部。

## 模型

YOLO ONNX 模型放置于：

```text
models\yolo\
```

NN 轨迹模型放置于：

```text
models\nn\
```

模型文件会被 Git 故意忽略。

## 负责任使用

仅在你拥有明确授权的环境中使用本项目。维护者不会为滥用、规避或未经授权的部署提供支持。

## 许可证

除非文件另有说明，本仓库中的项目代码使用 MIT License。第三方组件保留其各自许可证。参见 `THIRD_PARTY_NOTICES.zh-CN.md`。

法律效力以英文 `LICENSE` 文件为准。
