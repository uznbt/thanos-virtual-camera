#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
# load v4l2loopback if needed
if [ ! -e /dev/video10 ]; then
  pkexec modprobe v4l2loopback devices=1 video_nr=10 card_label="VirtualCam" exclusive_caps=1
fi
# OpenCV/Qt bundled with opencv-python expects a font directory.
SITE_PACKAGES="$(.venv/bin/python -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])' 2>/dev/null || true)"
if [ -n "$SITE_PACKAGES" ]; then
  mkdir -p "$SITE_PACKAGES/cv2/qt"
  ln -sfn /usr/share/fonts/TTF "$SITE_PACKAGES/cv2/qt/fonts"
fi
# OpenCV ships libqxcb but not the Wayland plugin.
export QT_QPA_PLATFORM=xcb
export TF_CPP_MIN_LOG_LEVEL=2
export GLOG_minloglevel=2
.venv/bin/python camera_virtual.py "$@"
