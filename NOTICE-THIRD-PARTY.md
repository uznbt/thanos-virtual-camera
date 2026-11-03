# Third-party notices

This project uses third-party components with the following licenses and attribution.

## U²-Net-P (`models/u2netp.onnx`)

- Model: U²-Net-P ONNX (`4.7M` parameters, approx. 4.4 MB).
- License: Apache-2.0.
- Source: `Heliosoph/u2net-onnx` on Hugging Face, republishing the official ONNX checkpoints of `xuebinqin/U-2-Net`.
- Copyright: Xuebin Qin, Zichen Zhang, Chenyang Huang, Masood Dehghan, Osmar R. Zaiane and Martin Jägersand — U²-Net: Going Deeper with Nested U-Structure for Salient Object Detection.
- Model license text: `models/u2netp-LICENSE.md`.

## ONNX Runtime v1.31.1

- Native shared library used at runtime; downloaded and SHA-256-verified during build.
- License: Apache-2.0.
- Source: `microsoft/onnxruntime` releases.

## stb_image_write.h

- Used to encode preview frames as PNG inside the native backend.
- License: public domain (Sean Barrett).
- Included source: `native/src/stb_image_write.h`.

## Chromium / Electron

- Desktop UI framework and shell.
- Electron: MIT.
- Chromium: BSD-compatible licenses and third-party notices distributed with Electron.

The native backend replaced the previous Python/MediaPipe implementation in packaged builds.
