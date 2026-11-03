#!/usr/bin/env python3
"""Client CLI to control the virtual camera from external apps / shortcuts."""
import argparse, json, sys, urllib.request, urllib.error

BASE = "http://127.0.0.1:8765"

def call(path, method="GET", data=None):
    url = BASE + path
    req = urllib.request.Request(url, method=method)
    if data is not None:
        req.data = json.dumps(data).encode()
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status, json.loads(r.read())
    except urllib.error.URLError as e:
        return None, {"error": str(e)}

def main():
    ap = argparse.ArgumentParser(description="Control virtual camera")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    sub.add_parser("modes")

    m = sub.add_parser("mode")
    m.add_argument("name", choices=["normal", "remove", "virtualbg", "blur", "1", "2", "3", "4"])

    sub.add_parser("snap")   # trigger thanos snap (mode remove)
    sub.add_parser("bg")     # capture background now
    sub.add_parser("resetbg")

    t = sub.add_parser("threshold")
    t.add_argument("value", type=float)

    args = ap.parse_args()

    if args.cmd == "status":
        code, data = call("/status")
    elif args.cmd == "modes":
        code, data = call("/modes")
    elif args.cmd == "mode":
        name = args.name
        if name in "1234":
            name = ["normal", "remove", "virtualbg", "blur"][int(name) - 1]
        code, data = call("/mode", "POST", {"mode": name})
    elif args.cmd == "snap":
        code, data = call("/snap", "POST")
    elif args.cmd == "bg":
        code, data = call("/bg", "POST")
    elif args.cmd == "resetbg":
        code, data = call("/resetbg", "POST")
    elif args.cmd == "threshold":
        code, data = call("/threshold", "POST", {"value": args.value})

    print(json.dumps(data, indent=2, ensure_ascii=False))
    sys.exit(0 if code and code < 400 else 1)

if __name__ == "__main__":
    main()
