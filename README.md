# Thanos Virtual Camera

Virtual camera for **Linux** and **Windows**. It removes people from your webcam output with a Thanos-style particle effect, or replaces/blurs your background. Use it as the camera in **Google Meet**, **Zoom**, **Microsoft Teams**, or any app that can select a virtual camera.

![icon](assets/icon.png)

## Features

- **Remove person**: person pixels dissolve into dust-like particles and disappear.
- **Virtual background**: keep the person and replace the room.
- **Background blur**: keep the person and blur the room.
- **Desktop control window**:
  - `1` Normal
  - `2` Remove
  - `3` Virtual background
  - `4` Blur
  - `B` Capture background
- **HTTP control API** on `http://127.0.0.1:8765`
- Linux virtual camera through `v4l2loopback` (`/dev/video10`)
- Windows virtual camera through **OBS Virtual Camera**
- Meet/Teams/Zoom integration through browser/Electron control or keyboard extension

## Requirements

### Linux
- `v4l2loopback`
- `/dev/video10`

```bash
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="VirtualCam" exclusive_caps=1
```

### Windows
Install **OBS Studio** and make sure **OBS Virtual Camera** is available.

Then select **OBS Virtual Camera** in Meet/Zoom/Teams.

## Build from source

```bash
npm install
npm run build:python
npm start
```

## Package

```bash
npm run dist
```

Results are written to `dist/`.

## GitHub Releases

Push a version tag to build and publish packages:

```bash
git tag v0.1.0
git push origin main --tags
```

Packages are attached to the GitHub Release.

## Repository layout

```text
camera_virtual.py        Python camera/segmentation backend
src/                     Electron desktop application
chrome-extension/        Optional browser extension
models/                  MediaPipe selfie segmentation model
scripts/build-python.mjs PyInstaller packaging helper
```

## License

GPL-3.0-or-later
