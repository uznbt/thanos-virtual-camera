#!/usr/bin/env python3
"""Virtual camera that can remove people / backgrounds for Google Meet, Zoom, Teams.

Captures from a real webcam, runs MediaPipe selfie segmentation, and streams the
result to a v4l2loopback virtual camera (e.g. /dev/video10) that Google Meet,
Zoom, Microsoft Teams, etc. can pick as their camera.

Modes (press 'm' to cycle, or 1-4 to pick directly):
  1 normal     - passthrough (no change)
  2 remove     - Thanos snap: person dissolves into particles, leaving background only
  3 virtualbg  - replace background with an image/color, keep the person
  4 blur       - blur the background, keep the person

Keys:
  q     quit
  m     cycle mode
  1-4   select mode directly
  b     capture current frame as background (use in an empty room)
  r     reset background to last captured / blank
  t     toggle horizontal flip of output
  +/-   adjust person mask threshold
  s     save a snapshot to snapshots/
"""
import argparse
import os
import sys
import time
from pathlib import Path

import cv2
import mediapipe as mp
import numpy as np
import pyvirtualcam
from flask import Flask, Response, jsonify, request
from werkzeug.serving import make_server
import threading

if getattr(sys, "_MEIPASS", None):
    APP_DIR = Path(sys._MEIPASS)
else:
    APP_DIR = Path(__file__).resolve().parent


MODES = ["normal", "remove", "virtualbg", "blur"]
MODE_LABEL = {
    "normal": "1 Normal (passthrough)",
    "remove": "2 Thanos snap (person gone)",
    "virtualbg": "3 Virtual background",
    "blur": "4 Blur background",
}


# ---------- HTTP API (external control) ----------
API_PORT = 8765
api_app = Flask("vcam")

@api_app.after_request
def _cors(resp):
    resp.headers["Access-Control-Allow-Origin"] = "*"
    resp.headers["Access-Control-Allow-Methods"] = "GET,POST,OPTIONS"
    resp.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return resp

@api_app.route("/mode", methods=["OPTIONS"])
def _mode_options():
    return ("", 204)
_api_state = {"mode": "normal", "cmd": None, "value": None, "bg_set": False, "threshold": 0.5, "jpeg": None}

@api_app.route("/status")
def api_status():
    return jsonify({
        "mode": _api_state["mode"],
        "bg_set": _api_state.get("bg_set", False),
        "threshold": _api_state.get("threshold", 0.5),
    })


@api_app.route("/frame.jpg")
def api_frame():
    frame = _api_state.get("jpeg")
    if not frame:
        return ("", 204)
    return Response(frame, mimetype="image/jpeg")

@api_app.route("/modes")
def api_modes():
    return jsonify({"modes": MODES})

@api_app.route("/mode", methods=["POST"])
def api_mode():
    data = request.get_json(silent=True) or {}
    m = data.get("mode")
    if m not in MODES and m not in ("1","2","3","4"):
        return jsonify({"error": "invalid mode"}), 400
    if m in ("1","2","3","4"):
        m = MODES[int(m)-1]
    _api_state["mode"] = m
    _api_state["cmd"] = "mode"
    _api_state["value"] = m
    return jsonify({"ok": True, "mode": m})

@api_app.route("/snap", methods=["POST"])
def api_snap():
    _api_state["cmd"] = "snap"
    return jsonify({"ok": True})

@api_app.route("/bg", methods=["POST"])
def api_bg():
    _api_state["cmd"] = "bg"
    return jsonify({"ok": True})

@api_app.route("/resetbg", methods=["POST"])
def api_resetbg():
    _api_state["cmd"] = "resetbg"
    return jsonify({"ok": True})

@api_app.route("/threshold", methods=["POST"])
def api_threshold():
    data = request.get_json(silent=True) or {}
    v = float(data.get("value", 0.5))
    _api_state["cmd"] = "threshold"
    _api_state["value"] = v
    return jsonify({"ok": True, "value": v})

def start_api():
    make_server("127.0.0.1", API_PORT, api_app, threaded=True).serve_forever()

# ---------- end API ----------

def parse_args():
    p = argparse.ArgumentParser(description="Person-removing virtual camera")
    p.add_argument("--src", default="0", help="source webcam index or path (default 0)")
    default_dst = "OBS Virtual Camera" if sys.platform.startswith("win") else "/dev/video10"
    p.add_argument("--dst", default=default_dst, help="virtual camera device")
    p.add_argument("--width", type=int, default=640)
    p.add_argument("--height", type=int, default=480)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--bg", default=None, help="path to virtual background image")
    p.add_argument("--threshold", type=float, default=0.5)
    p.add_argument("--smooth", type=float, default=0.55,
                   help="temporal smoothing of the mask (0..0.95)")
    p.add_argument("--snap-duration", type=float, default=1.6,
                   help="duration of Thanos snap animation in seconds")
    p.add_argument("--no-preview", action="store_true", help="disable preview window")
    return p.parse_args()


def open_source(src, w, h, fps):
    try:
        idx = int(src)
    except ValueError:
        idx = src
    cap = cv2.VideoCapture(idx)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, w)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, h)
    cap.set(cv2.CAP_PROP_FPS, fps)
    if not cap.isOpened():
        raise RuntimeError(f"Cannot open source camera: {src}")
    return cap


def load_bg(path, w, h):
    if path and os.path.exists(path):
        img = cv2.imread(path)
        if img is not None:
            return cv2.resize(img, (w, h))
    g = np.zeros((h, w, 3), dtype=np.uint8)
    for y in range(h):
        v = int(40 + 60 * (y / max(h - 1, 1)))
        g[y, :] = (v, v + 10, v + 20)
    return g


def build_segmenter(model_path):
    base = mp.tasks.BaseOptions(model_asset_path=model_path)
    opts = mp.tasks.vision.ImageSegmenterOptions(
        base_options=base,
        running_mode=mp.tasks.vision.RunningMode.VIDEO,
        output_category_mask=True,
    )
    return mp.tasks.vision.ImageSegmenter.create_from_options(opts)


def make_particles(mask, w, h):
    ys, xs = np.where(mask > 0.5)
    if len(xs) == 0:
        return []
    step = max(1, len(xs) // 2500)
    xs = xs[::step]
    ys = ys[::step]
    particles = []
    for x, y in zip(xs.tolist(), ys.tolist()):
        particles.append({
            "x": float(x), "y": float(y),
            "vx": float(np.random.uniform(-1.5, 1.5)),
            "vy": float(np.random.uniform(-3.0, -0.5)),
            "life": 1.0,
            "size": int(np.random.randint(1, 4)),
            "color_bgr": None,
        })
    return particles



PANEL_HEIGHT = 110

def get_buttons(width, height):
    y1 = height + 10
    y2 = height + 54
    bw = 140
    bw2 = 146
    gap = 8
    return [
        ("mode:normal", "1 Normal", 10, y1, bw, 36),
        ("mode:remove", "2 Remove", 10 + (bw + gap), y1, bw, 36),
        ("mode:virtualbg", "3 VBG", 10 + 2 * (bw + gap), y1, bw, 36),
        ("mode:blur", "4 Blur", 10 + 3 * (bw + gap), y1, bw, 36),
        ("bg", "Capture BG", 10, y2, bw2, 36),
        ("resetbg", "Reset BG", 10 + (bw2 + gap), y2, bw2, 36),
        ("flip", "Flip", 10 + 2 * (bw2 + gap), y2, bw2, 36),
        ("quit", "Quit", 10 + 3 * (bw2 + gap), y2, bw2, 36),
    ]

def draw_preview(frame, mode, threshold, background_set, flip_enabled):
    height, width = frame.shape[:2]
    preview = np.zeros((height + PANEL_HEIGHT, width, 3), dtype=np.uint8)
    preview[:height] = frame
    preview[height:] = (30, 30, 30)

    for name, label, x, y, bw, bh in get_buttons(width, height):
        active = name == f"mode:{mode}"
        if active:
            color = (40, 140, 40)
        elif name == "bg":
            color = (120, 70, 20)
        elif name == "resetbg":
            color = (20, 100, 160)
        elif name == "quit":
            color = (60, 40, 160)
        else:
            color = (70, 70, 70)
        cv2.rectangle(preview, (x, y), (x + bw, y + bh), color, -1)
        cv2.rectangle(preview, (x, y), (x + bw, y + bh), (110, 110, 110), 1)
        cv2.putText(preview, label, (x + 12, y + 23), cv2.FONT_HERSHEY_SIMPLEX,
                    0.48, (255, 255, 255), 1, cv2.LINE_AA)

    status = f"Mode: {mode}   BG: {'set' if background_set else 'none'}   Thr: {threshold:.2f}   Flip: {'on' if flip_enabled else 'off'}"
    cv2.putText(preview, status, (10, height + 104), cv2.FONT_HERSHEY_SIMPLEX,
                0.38, (210, 210, 210), 1, cv2.LINE_AA)
    return preview

def main():
    args = parse_args()
    here = APP_DIR
    model_path = here / "models" / "selfie_segmenter.tflite"
    if not model_path.exists():
        print("Model not found, downloading...")
        import urllib.request
        model_path.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(
            "https://storage.googleapis.com/mediapipe-models/image_segmenter/"
            "selfie_segmenter/float16/latest/selfie_segmenter.tflite",
            str(model_path),
        )

    cap = open_source(args.src, args.width, args.height, args.fps)
    seg = build_segmenter(str(model_path))

    virtual_bg = load_bg(args.bg, args.width, args.height)
    background = None
    mask_smooth = None
    threshold = args.threshold
    mode = "normal"
    flip = True
    last_ts = -1

    snap_active = False
    snap_start = 0.0
    snap_particles = []
    snap_bg = None
    snap_prev_mode = "normal"
    reassemble_active = False
    reassemble_start = 0.0
    reassemble_particles = []
    reassemble_bg = None

    work_dir = Path(os.environ.get("THANOS_CAMERA_HOME", Path.cwd()))
    snapshot_dir = work_dir / "snapshots"
    os.makedirs(snapshot_dir, exist_ok=True)

    cam = pyvirtualcam.Camera(
        width=args.width, height=args.height, fps=args.fps,
        device=args.dst, fmt=pyvirtualcam.PixelFormat.BGR,
    )
    print(f"Virtual camera opened: {cam.device}")
    print("Controls: m=cycle mode, 1-4=select, b=capture background, "
          "r=reset bg, t=flip, +/-=threshold, s=snapshot, q=quit")

    threading.Thread(target=start_api, daemon=True).start()
    print(f"HTTP API on http://127.0.0.1:{API_PORT}")

    preview_name = "Virtual Camera (q to quit)"
    _button_events = []

    def on_mouse(event, x, y, flags, param):
        if event != cv2.EVENT_LBUTTONDOWN:
            return
        for name, label, bx, by, bw, bh in get_buttons(args.width, args.height):
            if bx <= x <= bx + bw and by <= y <= by + bh:
                _button_events.append(name)
                return

    if not args.no_preview:
        cv2.namedWindow(preview_name, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(preview_name, args.width, args.height + PANEL_HEIGHT)
        cv2.setMouseCallback(preview_name, on_mouse)

    try:
        while True:
            ok, frame = cap.read()
            if not ok or frame is None:
                time.sleep(0.01)
                continue
            frame = cv2.resize(frame, (args.width, args.height))
            if flip:
                frame = cv2.flip(frame, 1)

            # apply external commands
            if _api_state.get('cmd') is not None:
                c = _api_state['cmd']; v = _api_state['value']
                _api_state['cmd'] = None
                if c == 'mode':
                    mode = v
                elif c == 'snap':
                    if not snap_active:
                        snap_active = True; snap_start = now if 'now' in locals() else time.perf_counter()
                        snap_bg = background.copy() if background is not None else frame.copy()
                        snap_particles = make_particles(hard if 'hard' in locals() else np.zeros((args.height,args.width)), args.width, args.height)
                        for pt in snap_particles:
                            x,y=int(pt['x']),int(pt['y']); x=min(max(x,0),args.width-1); y=min(max(y,0),args.height-1)
                            pt['color_bgr']=tuple(int(cc) for cc in frame[y,x])
                elif c == 'bg':
                    background = frame.copy(); snap_active=False; reassemble_active=False; print('Background captured (api).')
                elif c == 'resetbg':
                    background = None; snap_active=False; reassemble_active=False; print('Background reset (api).')
                elif c == 'threshold':
                    threshold = float(v)
            _api_state['mode'] = mode
            _api_state['bg_set'] = background is not None
            _api_state['threshold'] = threshold

            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            mpimg = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
            ts = max(last_ts + 1, int(time.perf_counter() * 1000))
            last_ts = ts
            result = seg.segment_for_video(mpimg, ts)
            raw_mask = result.category_mask.numpy_view()
            person = (raw_mask.squeeze() < 127).astype(np.float32)

            if mask_smooth is None:
                mask_smooth = person.copy()
            else:
                a = float(np.clip(args.smooth, 0.0, 0.95))
                mask_smooth = a * mask_smooth + (1.0 - a) * person
            mask = np.clip(mask_smooth, 0.0, 1.0)

            hard = (mask >= threshold).astype(np.float32)
            soft = cv2.GaussianBlur(mask, (0, 0), sigmaX=3, sigmaY=3)
            soft = np.clip(soft, 0.0, 1.0)
            m3 = soft[:, :, None]

            if background is None:
                background = frame.copy()

            blurred = cv2.GaussianBlur(frame, (35, 35), 0)

            now = time.perf_counter()

            if mode == "remove" and not snap_active and snap_prev_mode != "remove" and not reassemble_active:
                snap_active = True
                snap_start = now
                snap_bg = background.copy()
                snap_particles = make_particles(hard, args.width, args.height)
                for pt in snap_particles:
                    x, y = int(pt["x"]), int(pt["y"])
                    x = min(max(x, 0), args.width - 1)
                    y = min(max(y, 0), args.height - 1)
                    pt["color_bgr"] = tuple(int(c) for c in frame[y, x])

            # Trigger reverse snap (reassemble) when leaving remove -> normal
            if snap_prev_mode == "remove" and mode != "remove" and not reassemble_active and not snap_active:
                reassemble_active = True
                reassemble_start = now
                reassemble_bg = background.copy() if background is not None else frame.copy()
                ys, xs = np.where(hard > 0.5)
                if len(xs) > 0:
                    step = max(1, len(xs) // 2500)
                    xs = xs[::step]; ys = ys[::step]
                    reassemble_particles = []
                    for x, y in zip(xs.tolist(), ys.tolist()):
                        sx = float(x + np.random.uniform(-80, 80))
                        sy = float(y + np.random.uniform(-80, 80))
                        reassemble_particles.append({
                            "sx": sx, "sy": sy,
                            "tx": float(x), "ty": float(y),
                            "x": sx, "y": sy,
                            "color_bgr": tuple(int(c) for c in frame[min(max(y,0),args.height-1), min(max(x,0),args.width-1)]),
                            "size": int(np.random.randint(1, 4)),
                        })
                else:
                    reassemble_particles = []

            snap_prev_mode = mode

            if mode == "normal":
                if reassemble_active:
                    elapsed = now - reassemble_start
                    dur = args.snap_duration
                    progress = min(1.0, elapsed / dur)
                    p = 1.0 - (1.0 - progress) ** 3
                    canvas = reassemble_bg.copy() if reassemble_bg is not None else frame.copy()
                    for pt in reassemble_particles:
                        pt["x"] = pt["sx"] + (pt["tx"] - pt["sx"]) * p
                        pt["y"] = pt["sy"] + (pt["ty"] - pt["sy"]) * p
                        if 0 <= pt["x"] < args.width and 0 <= pt["y"] < args.height:
                            alpha = progress
                            c = pt["color_bgr"]
                            dust = (180, 180, 180)
                            col = tuple(int(dust[i] * (1 - alpha) + c[i] * alpha) for i in range(3))
                            size = max(1, int(pt["size"] * (0.5 + 0.5 * alpha)))
                            cv2.circle(canvas, (int(pt["x"]), int(pt["y"])), size, col, -1)
                    if progress > 0.3:
                        blend = min(1.0, (progress - 0.3) / 0.7)
                        canvas = cv2.addWeighted(canvas, 1 - blend, frame, blend, 0)
                    out = canvas
                    if progress >= 1.0:
                        reassemble_active = False
                        out = frame
                else:
                    out = frame
            elif mode == "virtualbg":
                out = (frame.astype(np.float32) * m3
                       + virtual_bg.astype(np.float32) * (1.0 - m3)).astype(np.uint8)
            elif mode == "blur":
                out = (frame.astype(np.float32) * m3
                       + blurred.astype(np.float32) * (1.0 - m3)).astype(np.uint8)
            elif mode == "remove":
                if snap_active:
                    elapsed = now - snap_start
                    dur = args.snap_duration
                    progress = min(1.0, elapsed / dur)
                    p = progress * progress
                    canvas = snap_bg.copy()
                    for pt in snap_particles:
                        pt["x"] += pt["vx"] * (1 + p * 2)
                        pt["y"] += pt["vy"] * (1 + p * 2)
                        pt["vy"] += 0.15
                        pt["life"] = 1.0 - p
                        if pt["life"] <= 0:
                            continue
                        if 0 <= pt["x"] < args.width and 0 <= pt["y"] < args.height:
                            alpha = pt["life"]
                            c = pt["color_bgr"]
                            dust = (180, 180, 180)
                            col = tuple(int(c[i] * alpha + dust[i] * (1 - alpha)) for i in range(3))
                            size = max(1, int(pt["size"] * (0.5 + 0.5 * alpha)))
                            cv2.circle(canvas, (int(pt["x"]), int(pt["y"])), size, col, -1)
                    out = canvas
                    if progress >= 1.0:
                        snap_active = False
                        out = snap_bg.copy()
                else:
                    out = background.copy()
            else:
                out = frame

            camera_out = out.copy()
            ok, jpeg = cv2.imencode('.jpg', camera_out, [cv2.IMWRITE_JPEG_QUALITY, 80])
            if ok:
                _api_state['jpeg'] = jpeg.tobytes()
            cam.send(camera_out)
            cam.sleep_until_next_frame()

            if not args.no_preview:
                preview = draw_preview(out, mode, threshold, background is not None, flip)
                cv2.imshow(preview_name, preview)
                key = cv2.waitKey(1) & 0xFF
                for action in list(_button_events):
                    if action.startswith("mode:"):
                        mode = action.split(":", 1)[1]
                    elif action == "bg":
                        background = frame.copy()
                        snap_active = False
                        reassemble_active = False
                        print("Background captured.")
                    elif action == "resetbg":
                        background = None
                        snap_active = False
                        reassemble_active = False
                        print("Background reset.")
                    elif action == "flip":
                        flip = not flip
                    elif action == "quit":
                        key = ord("q")
                _button_events.clear()
                if key == ord("q"):
                    break
                elif key == ord("m"):
                    i = (MODES.index(mode) + 1) % len(MODES)
                    mode = MODES[i]
                elif key in (ord("1"), ord("2"), ord("3"), ord("4")):
                    mode = MODES[key - ord("1")]
                elif key == ord("b"):
                    background = frame.copy()
                    snap_active = False
                    reassemble_active = False
                    print("Background captured.")
                elif key == ord("r"):
                    background = None
                    snap_active = False
                    reassemble_active = False
                    print("Background reset.")
                elif key == ord("t"):
                    flip = not flip
                elif key == ord("s"):
                    pth = snapshot_dir / f"snap_{int(time.time())}.png"
                    cv2.imwrite(str(pth), out)
                    print("Saved", pth)
                elif key in (ord("+"), ord("=")):
                    threshold = min(0.99, threshold + 0.02)
                elif key == ord("-"):
                    threshold = max(0.01, threshold - 0.02)
            else:
                time.sleep(1.0 / args.fps)

    except KeyboardInterrupt:
        pass
    finally:
        cam.close()
        cap.release()
        if not args.no_preview:
            cv2.destroyAllWindows()
        seg.close()
        print("Stopped.")


if __name__ == "__main__":
    main()
