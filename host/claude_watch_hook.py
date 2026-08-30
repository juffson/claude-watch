#!/usr/bin/env python3
"""
Claude Code hook -> ClaudeWatch (ESP32-S3 AMOLED).

Installed for these hook events (see install_hooks.py):
  SessionStart, UserPromptSubmit, PreToolUse, PostToolUse, PermissionRequest,
  Notification, Stop, SessionEnd, PreCompact

Reads the hook JSON on stdin, keeps a tiny per-session state file, computes the
aggregate state across all running sessions and pushes it to the board over
HTTP (http://claude-watch.local/status), falling back to USB serial.

Never prints anything (hook stdout may be injected into the conversation) and
always exits 0 quickly: the network I/O happens in a detached child process.
"""
import fcntl
import glob
import json
import os
import socket
import sys
import time
import urllib.request

STATE_DIR = os.path.expanduser("~/.claude/claude-watch")
STATE_FILE = os.path.join(STATE_DIR, "state.json")
LOG_FILE = os.path.join(STATE_DIR, "hook.log")

HOST = os.environ.get("CLAUDE_WATCH_HOST", "claude-watch.local")
PORT = int(os.environ.get("CLAUDE_WATCH_PORT", "80"))
HTTP_TIMEOUT = 1.5
IP_CACHE_TTL = 600          # seconds
SESSION_TTL = 3 * 3600      # forget sessions silent for this long
PRIORITY = {"waiting": 4, "error": 3, "working": 2, "idle": 1}

WAITING_TOOLS = {"AskUserQuestion", "ExitPlanMode"}


def log(msg):
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        if os.path.exists(LOG_FILE) and os.path.getsize(LOG_FILE) > 200_000:
            os.replace(LOG_FILE, LOG_FILE + ".1")
        with open(LOG_FILE, "a") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    except Exception:
        pass


def short(s, n=60):
    # board has a Latin + common-Chinese font; drop control chars, collapse whitespace
    s = "".join(ch for ch in str(s or "") if ord(ch) >= 32 or ch in "\n\t")
    s = " ".join(s.split())
    return s if len(s) <= n else s[: n - 1] + "..."


def last_assistant_text(transcript_path, limit=240):
    """Last assistant text block from the session transcript (JSONL), trimmed for the display."""
    try:
        with open(transcript_path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - 200_000))
            tail = f.read().decode("utf-8", "replace")
        text = ""
        for line in tail.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            if rec.get("type") != "assistant":
                continue
            msg = rec.get("message") or {}
            content = msg.get("content")
            if isinstance(content, str):
                parts = [content]
            else:
                parts = [c.get("text", "") for c in (content or []) if isinstance(c, dict) and c.get("type") == "text"]
            t = "\n".join(pt for pt in parts if pt).strip()
            if t:
                text = t
        # drop markdown noise that renders badly on a 466px round screen
        text = text.replace("**", "").replace("`", "")
        return short(text, limit)
    except Exception as e:
        log(f"transcript read failed: {e}")
        return ""


def tool_detail(name, inp):
    """A short human hint about what the tool is doing."""
    if not isinstance(inp, dict):
        return ""
    if name == "Bash":
        return short(inp.get("description") or inp.get("command"))
    if name in ("Edit", "Write", "Read", "NotebookEdit"):
        return os.path.basename(str(inp.get("file_path") or inp.get("notebook_path") or ""))
    if name in ("Agent", "Task"):
        return short(inp.get("description"))
    if name in ("Grep", "Glob"):
        return short(inp.get("pattern"))
    if name == "WebFetch":
        return short(inp.get("url"))
    if name == "WebSearch":
        return short(inp.get("query"))
    if name == "Skill":
        return short(inp.get("skill"))
    if name == "AskUserQuestion":
        qs = inp.get("questions") or []
        if qs and isinstance(qs[0], dict):
            return short(qs[0].get("question"), 80)
    return ""


def map_event(ev):
    """Return (state|None, tool, msg, remove_session, output)."""
    name = ev.get("hook_event_name", "")
    tool = ev.get("tool_name") or ""
    inp = ev.get("tool_input") or {}

    if name == "SessionStart":
        return "idle", "", "", False, ""
    if name == "UserPromptSubmit":
        return "working", "", short(ev.get("prompt"), 80), False, ""
    if name == "PreToolUse":
        if tool in WAITING_TOOLS:
            return "waiting", tool, tool_detail(tool, inp), False, ""
        return "working", tool, tool_detail(tool, inp), False, ""
    if name == "PostToolUse":
        return "working", tool, tool_detail(tool, inp), False, ""
    if name == "PermissionRequest":
        return "waiting", tool or "permission", "approve: " + (tool_detail(tool, inp) or tool), False, ""
    if name == "Notification":
        ntype = ev.get("notification_type") or ""
        msg = ev.get("message") or ""
        low = (ntype + " " + msg).lower()
        if "permission" in low or "elicitation" in low:
            return "waiting", tool or "permission", short(msg, 80), False, ""
        if "idle" in low or "waiting for your input" in low:
            return "idle", "", "", False, ""
        return None, "", "", False, ""
    if name == "Stop":
        return "idle", "", "", False, last_assistant_text(ev.get("transcript_path") or "")
    if name == "SessionEnd":
        return None, "", "", True, ""
    if name in ("PreCompact", "PostCompact"):
        return "working", "compact", "compacting context", False, ""
    return None, "", "", False, ""


def load_state():
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(state):
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, STATE_FILE)


def aggregate(sessions):
    if not sessions:
        return {"state": "offline", "tool": "", "project": "", "msg": "", "output": "", "sessions": 0, "epoch": int(time.time())}
    best = max(sessions.values(), key=lambda s: (PRIORITY.get(s["state"], 0), s["ts"]))
    return {
        "state": best["state"],
        "tool": best.get("tool", ""),
        "project": best.get("project", ""),
        "msg": best.get("msg", ""),
        "output": best.get("output", ""),
        "sessions": len(sessions),
        "epoch": int(time.time()),   # the board has no RTC; it adopts this until NTP corrects it
    }


# ---------------- transport (runs in the detached child) ----------------

def resolve_host(state):
    ip = state.get("ip")
    if ip and time.time() - state.get("ip_ts", 0) < IP_CACHE_TTL:
        return ip, False
    try:
        infos = socket.getaddrinfo(HOST, PORT, socket.AF_INET, socket.SOCK_STREAM)
        return infos[0][4][0], True
    except Exception as e:
        log(f"resolve {HOST} failed: {e}")
        return None, False


def send_http(ip, payload):
    req = urllib.request.Request(
        f"http://{ip}:{PORT}/status",
        data=payload.encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
        return r.status == 200


def send_serial(payload):
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        return False
    data = (payload + "\n").encode("utf-8")
    for port in ports:
        try:
            try:
                import serial  # pyserial (installed alongside esptool)
                s = serial.Serial()
                s.port = port
                s.baudrate = 115200
                s.timeout = 0.3
                s.write_timeout = 0.5
                s.dtr = True   # DTR=1,RTS=0: the only combo that does NOT reset the ESP32-S3 USB-JTAG
                s.rts = False
                s.open()
                s.write(data)
                s.flush()
                s.close()
            except ImportError:
                # a raw open() would toggle DTR/RTS and may reset the board — refuse
                log("pyserial missing: pip3 install pyserial")
                return False
            return True
        except Exception as e:
            log(f"serial {port} failed: {e}")
    return False


def deliver(payload):
    state = load_state()
    ip, fresh = resolve_host(state)
    if ip:
        try:
            if send_http(ip, payload):
                if fresh:
                    with_lock(lambda st: st.update({"ip": ip, "ip_ts": time.time()}))
                return
        except Exception as e:
            log(f"http {ip} failed: {e}")
            with_lock(lambda st: st.pop("ip", None))
    # USB serial fallback (Wi-Fi is preferred). The port is opened with DTR=1/RTS=0,
    # the only line state that does not trigger the ESP32-S3 USB-JTAG reset logic.
    # Set CLAUDE_WATCH_SERIAL=0 to disable it.
    if os.environ.get("CLAUDE_WATCH_SERIAL", "1") != "0":
        if send_serial(payload):
            return
    log("delivery failed: " + payload)


def with_lock(fn):
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(STATE_FILE + ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        state = load_state()
        result = fn(state)
        save_state(state)
        return result


# ---------------- main ----------------

def main():
    raw = sys.stdin.read()
    try:
        ev = json.loads(raw) if raw.strip() else {}
    except Exception:
        ev = {}

    sid = str(ev.get("session_id") or "default")
    cwd = ev.get("cwd") or os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()
    project = os.path.basename(cwd.rstrip("/")) or cwd
    state_name, tool, msg, remove, output = map_event(ev)
    if state_name is None and not remove:
        return

    now = time.time()

    def update(st):
        sessions = st.setdefault("sessions", {})
        for k in [k for k, v in sessions.items() if now - v.get("ts", 0) > SESSION_TTL]:
            sessions.pop(k, None)
        if remove:
            sessions.pop(sid, None)
        else:
            prev = sessions.get(sid) or {}
            # keep the last reply visible while the next turn is working; replace it on the next Stop
            keep = prev.get("output", "") if state_name == "working" or not output else output
            sessions[sid] = {"state": state_name, "tool": tool, "project": project, "msg": msg,
                             "output": keep if not output else output, "ts": now}
        return aggregate(sessions)

    payload = json.dumps(with_lock(update), ensure_ascii=False)

    # detach: the hook returns immediately, the child does the I/O
    if os.fork() != 0:
        return
    os.setsid()
    if os.fork() != 0:
        os._exit(0)
    devnull = os.open(os.devnull, os.O_RDWR)
    for fd in (0, 1, 2):
        os.dup2(devnull, fd)
    try:
        deliver(payload)
    except Exception as e:
        log(f"deliver error: {e}")
    os._exit(0)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # never break Claude Code
        log(f"hook error: {e}")
    sys.exit(0)
