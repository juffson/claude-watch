#!/usr/bin/env python3
"""
watchctl — talk to the ClaudeWatch board from the Mac.

  watchctl.py ports                         list candidate serial ports
  watchctl.py monitor [PORT]                stream the board's serial log (Ctrl+C quits)
  watchctl.py wifi SSID PASSWORD [PORT]     store Wi-Fi credentials over USB
  watchctl.py time [PORT]                   push the Mac's clock to the board over USB
  watchctl.py info [PORT]                   print board info over USB
  watchctl.py reset [PORT]                  pulse the reset line (serial silent after flashing)
  watchctl.py sleep [PORT]                  deep sleep now (same as a long press)
  watchctl.py status                        GET current status over HTTP
  watchctl.py send STATE [--tool T] [--msg M] [--project P] [--sessions N]
                                            push a status (HTTP, falls back to USB)

Environment: CLAUDE_WATCH_HOST (default claude-watch.local), CLAUDE_WATCH_PORT (80)
"""
import argparse
import glob
import json
import os
import sys
import time
import urllib.request

HOST = os.environ.get("CLAUDE_WATCH_HOST", "claude-watch.local")
PORT = int(os.environ.get("CLAUDE_WATCH_PORT", "80"))


def ports():
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def open_serial(port=None):
    import serial  # pyserial
    port = port or (ports()[0] if ports() else None)
    if not port:
        sys.exit("no /dev/cu.usbmodem* port found — is the board plugged in?")
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = True   # DTR=1,RTS=0: the only combo that does NOT reset the ESP32-S3 USB-JTAG
    s.rts = False
    s.open()
    return s


def serial_cmd(line, port=None, wait=1.5):
    s = open_serial(port)
    time.sleep(0.2)
    s.write(b"\n")            # terminate any half-received line left in the board's buffer
    s.flush()
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    s.flush()
    end = time.time() + wait
    out = []
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            out.append(chunk.decode(errors="replace"))
    s.close()
    print("".join(out), end="")


def cmd_reset(port=None):
    """Pulse the USB-JTAG reset line (DTR=0/RTS=1) — recovers a board whose serial went silent after flashing."""
    import serial
    port = port or (ports()[0] if ports() else None)
    if not port:
        sys.exit("no /dev/cu.usbmodem* port found")
    s = serial.Serial(); s.port = port; s.baudrate = 115200; s.dtr = False; s.rts = True
    s.open(); time.sleep(0.1); s.close()
    print("reset pulse sent to", port)


def cmd_monitor(port=None):
    s = open_serial(port)
    print(f"-- monitor {s.port} (Ctrl+C to quit) --")
    try:
        while True:
            chunk = s.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode(errors="replace"))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


def http(method, path, body=None, timeout=3):
    req = urllib.request.Request(
        f"http://{HOST}:{PORT}{path}",
        data=body.encode() if body else None,
        headers={"Content-Type": "application/json"} if body else {},
        method=method,
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ports")
    p = sub.add_parser("monitor"); p.add_argument("port", nargs="?")
    p = sub.add_parser("wifi"); p.add_argument("ssid"); p.add_argument("password"); p.add_argument("port", nargs="?")
    p = sub.add_parser("time"); p.add_argument("port", nargs="?")
    p = sub.add_parser("info"); p.add_argument("port", nargs="?")
    p = sub.add_parser("reset"); p.add_argument("port", nargs="?")
    p = sub.add_parser("sleep"); p.add_argument("port", nargs="?")
    sub.add_parser("status")
    p = sub.add_parser("send")
    p.add_argument("state", choices=["offline", "idle", "working", "waiting", "error"])
    p.add_argument("--tool", default="")
    p.add_argument("--msg", default="")
    p.add_argument("--project", default=os.path.basename(os.getcwd()))
    p.add_argument("--sessions", type=int, default=1)
    a = ap.parse_args()

    if a.cmd == "ports":
        print("\n".join(ports()) or "(none)")
    elif a.cmd == "monitor":
        cmd_monitor(a.port)
    elif a.cmd == "wifi":
        serial_cmd(f"wifi {a.ssid} {a.password}", a.port, wait=8)
    elif a.cmd == "time":
        serial_cmd(f"time {int(time.time())}", a.port)
    elif a.cmd == "info":
        serial_cmd("info", a.port)
    elif a.cmd == "reset":
        cmd_reset(a.port)
    elif a.cmd == "sleep":
        serial_cmd("sleep", a.port, wait=1.0)   # deep sleep; wake with BOOT or a touch
    elif a.cmd == "status":
        print(http("GET", "/status"))
    elif a.cmd == "send":
        payload = json.dumps({"state": a.state, "tool": a.tool, "msg": a.msg,
                              "project": a.project, "sessions": a.sessions})
        try:
            print("http:", http("POST", "/status", payload))
        except Exception as e:
            print(f"http failed ({e}), trying USB serial…")
            serial_cmd(payload)


if __name__ == "__main__":
    main()
