#!/usr/bin/env python3
"""
ClaudeWatch voice server — the Mac is the brain, the board is the microphone and speaker.

  board  --POST /voice (raw PCM 16 kHz mono s16le)-->  this server
         whisper.cpp (speech -> text)  ->  `claude -p` (answer)  ->  macOS `say` (text -> speech)
  board  <--JSON {text, reply, audio: "/audio/<id>"}--  then GET /audio/<id> (raw PCM 16 kHz mono)

The server announces itself to the board every 60 s (POST http://claude-watch.local/api/voice_server),
so nothing needs to be configured on the device.

  python3 host/voice_server.py                    # default port 8765
  python3 host/voice_server.py --port 9000 --model ~/.cache/whisper/ggml-small.bin --voice "Meijia"

Requirements: brew install whisper-cpp ; model in ~/.cache/whisper/ (ggml-base.bin downloaded by setup);
`claude` CLI on PATH (Claude Code); macOS `say` with a Chinese voice.
Environment: CLAUDE_WATCH_HOST (default claude-watch.local)
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BOARD = os.environ.get("CLAUDE_WATCH_HOST", "claude-watch.local")
RATE = 16000
WORK = os.path.expanduser("~/.claude/claude-watch/voice")
AUDIO = {}  # id -> pcm bytes (kept for a few minutes)
CLAUDE_MODEL = "haiku"   # fast model for voice; override with --claude-model

SYSTEM_PROMPT = ("你是一块桌面小屏幕上的语音助手。用户通过麦克风说话，你的回答会被朗读出来并显示在 466 像素的圆屏上。"
                 "请用简体中文口语化回答，尽量控制在两句话、60 字以内，不要用 Markdown、列表或代码块。")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def lan_ip():
    """Real LAN address (skips VPN/fake-IP utun interfaces such as 198.18.x.x)."""
    for ifn in ("en0", "en1", "en2"):
        try:
            ip = subprocess.run(["ipconfig", "getifaddr", ifn], capture_output=True, text=True, timeout=3).stdout.strip()
            if ip and not ip.startswith("198.18.") and not ip.startswith("127."):
                return ip
        except Exception:
            pass
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def pcm_to_wav(pcm, path):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


def transcribe(wav_path, model):
    out_base = wav_path[:-4]
    cmd = ["whisper-cli", "-m", model, "-l", "zh", "-f", wav_path, "-nt", "-np", "-otxt", "-of", out_base, "-t", "4"]
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    txt_path = out_base + ".txt"
    text = ""
    if os.path.exists(txt_path):
        text = open(txt_path, encoding="utf-8", errors="replace").read()
    else:
        text = r.stdout
    text = " ".join(text.split())
    text = re.sub(r"\[[^\]]*\]|\([^)]*\)", "", text).strip()   # whisper's [BLANK_AUDIO], (music) ...
    log(f"whisper {time.time() - t0:.1f}s -> {text!r}")
    return text


def ask_claude(text, cwd):
    os.makedirs(cwd, exist_ok=True)
    marker = os.path.join(cwd, ".session")
    cmd = ["claude", "-p", "--output-format", "text", "--append-system-prompt", SYSTEM_PROMPT]
    if CLAUDE_MODEL:
        cmd += ["--model", CLAUDE_MODEL]
    if os.path.exists(marker):
        cmd.append("--continue")          # keep one running conversation for the voice channel
    cmd.append(text)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, cwd=cwd)
        reply = r.stdout.strip() or r.stderr.strip()[:200]
        open(marker, "w").write(str(time.time()))
    except subprocess.TimeoutExpired:
        reply = "抱歉，这次思考超时了。"
    reply = re.sub(r"[*`#>_]+", "", reply)
    reply = " ".join(reply.split())
    log(f"claude {time.time() - t0:.1f}s -> {reply!r}")
    return reply


def synthesize(text, voice):
    """macOS say -> 16 kHz mono s16le PCM (say writes WAV when asked for LEI16)."""
    if not text:
        return b""
    fd, wav_path = tempfile.mkstemp(suffix=".wav", dir=WORK)
    os.close(fd)
    try:
        cmd = ["say", "-o", wav_path, "--data-format=LEI16@16000"]
        if voice:
            cmd += ["-v", voice]
        cmd.append(text[:400])
        subprocess.run(cmd, check=True, timeout=60, capture_output=True)
        with wave.open(wav_path, "rb") as f:
            ch = f.getnchannels()
            data = f.readframes(f.getnframes())
        if ch == 2:  # downmix, keep left
            data = b"".join(data[i:i + 2] for i in range(0, len(data), 4))
        return data
    except Exception as e:
        log(f"say failed: {e}")
        return b""
    finally:
        try:
            os.remove(wav_path)
        except OSError:
            pass


class Handler(BaseHTTPRequestHandler):
    server_version = "ClaudeWatchVoice/1.0"

    def log_message(self, *a):  # quiet
        pass

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/audio/"):
            aid = self.path.split("/")[-1]
            pcm = AUDIO.get(aid)
            if pcm is None:
                self.send_response(404); self.end_headers(); return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(pcm)))
            self.end_headers()
            self.wfile.write(pcm)
            return
        self._json(200, {"ok": True, "service": "claude-watch voice", "model": os.path.basename(self.server.model)})

    def do_POST(self):
        if self.path != "/voice":
            self._json(404, {"ok": False}); return
        n = int(self.headers.get("Content-Length", "0"))
        pcm = self.rfile.read(n)
        log(f"voice: {len(pcm) / 2 / RATE:.1f}s of audio from {self.client_address[0]}")
        if len(pcm) < RATE:  # < 0.5 s
            self._json(400, {"ok": False, "error": "too short"}); return
        os.makedirs(WORK, exist_ok=True)
        fd, wav = tempfile.mkstemp(suffix=".wav", dir=WORK)
        os.close(fd)
        try:
            pcm_to_wav(pcm, wav)
            text = transcribe(wav, self.server.model)
        finally:
            for ext in ("", ".txt"):
                try: os.remove(wav[:-4] + ext if ext else wav)
                except OSError: pass
        if not text:
            self._json(200, {"ok": True, "text": "", "reply": "没有听清，请再说一次。", "audio": ""}); return
        reply = ask_claude(text, os.path.join(WORK, "session"))
        pcm_out = synthesize(reply, self.server.voice)
        aid = uuid.uuid4().hex[:12]
        if pcm_out:
            AUDIO[aid] = pcm_out
            # forget old clips
            for k in list(AUDIO)[:-8]:
                AUDIO.pop(k, None)
        self._json(200, {"ok": True, "text": text, "reply": reply, "audio": f"/audio/{aid}" if pcm_out else ""})


def announce(url):
    while True:
        try:
            req = urllib.request.Request(f"http://{BOARD}/api/voice_server", data=json.dumps({"url": url}).encode(),
                                         headers={"Content-Type": "application/json"}, method="POST")
            with urllib.request.urlopen(req, timeout=5) as r:
                if r.status == 200:
                    log(f"announced {url} to {BOARD}")
        except Exception as e:
            log(f"announce failed ({e}); retrying")
        time.sleep(60)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--model", default=os.path.expanduser("~/.cache/whisper/ggml-base.bin"))
    ap.add_argument("--voice", default="Eddy (中文（中国大陆）)")
    ap.add_argument("--ip", default=None, help="LAN IP to announce (auto-detected)")
    ap.add_argument("--claude-model", default="haiku", help="model alias for claude -p (haiku / sonnet / opus)")
    a = ap.parse_args()
    global CLAUDE_MODEL
    CLAUDE_MODEL = a.claude_model
    if not os.path.exists(a.model):
        sys.exit(f"whisper model not found: {a.model}")
    os.makedirs(WORK, exist_ok=True)
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    srv.model = a.model
    srv.voice = a.voice
    url = f"http://{a.ip or lan_ip()}:{a.port}"
    log(f"voice server on {url}  (whisper: {os.path.basename(a.model)}, voice: {a.voice})")
    threading.Thread(target=announce, args=(url,), daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
