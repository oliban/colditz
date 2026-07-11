#!/usr/bin/env python3
"""Spectator dashboard server for the Colditz Escape AI speedrun.

Stdlib-only HTTP server bound to 127.0.0.1:8900. Serves the dashboard
(index.html), proxies the game API (127.0.0.1:8765) for /api/state and
/api/screen, and exposes the event log (../live-log.jsonl) incrementally
via /api/log?since=N.

No external dependencies. Run with: python3 server.py
"""
import json
import os
import subprocess
import sys
import time
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HOST = "127.0.0.1"
PORT = 8900
GAME_API = "http://127.0.0.1:8765"

DASHBOARD_DIR = os.path.dirname(os.path.abspath(__file__))
CAMPAIGN_DIR = os.path.dirname(DASHBOARD_DIR)
LOG_PATH = os.path.join(CAMPAIGN_DIR, "live-log.jsonl")
INDEX_PATH = os.path.join(DASHBOARD_DIR, "index.html")
RUNS_MD_PATH = os.path.join(CAMPAIGN_DIR, "runs.md")
RECORDER_PATH = os.path.join(CAMPAIGN_DIR, "recorder.sh")

GAME_TIMEOUT = 2.0  # seconds
RECORDER_TIMEOUT = 10.0  # seconds; start's own permission probe takes ~3s


def _next_run_number():
    """Next run number = count of existing DATA rows in runs.md + 1.

    Mirrors run-timer.sh's arithmetic exactly: runs.md has a header line
    and a `|---|` separator line, both starting with `|`, so counting all
    lines starting with `|` over-counts data rows by 2.
    """
    try:
        with open(RUNS_MD_PATH, "r") as f:
            pipe_lines = sum(1 for line in f if line.startswith("|"))
    except OSError:
        pipe_lines = 0
    data_rows = max(pipe_lines - 2, 0)
    return data_rows + 1


def _run_recorder(args):
    """Run recorder.sh with args, return (returncode, stdout, stderr)."""
    try:
        proc = subprocess.run(
            ["/bin/bash", RECORDER_PATH] + args,
            capture_output=True,
            text=True,
            timeout=RECORDER_TIMEOUT,
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "recorder.sh timed out"
    except OSError as e:
        return -1, "", "could not run recorder.sh: %s" % e


def _parse_recorder_status(stdout):
    """Parse recorder.sh status's `key: value` lines into a dict."""
    fields = {"state": "idle", "file": "", "error": ""}
    for line in stdout.splitlines():
        if ": " in line:
            key, _, val = line.partition(": ")
        elif line.endswith(":"):
            key, val = line[:-1], ""
        else:
            continue
        if key in fields:
            fields[key] = val
    return {
        "recording": fields["state"] == "recording",
        "file": fields["file"] or None,
        "error": fields["error"] or None,
    }


def _append_log_event(entry_type, text):
    entry = {"ts": round(time.time(), 3), "type": entry_type, "text": text}
    try:
        with open(LOG_PATH, "a") as f:
            f.write(json.dumps(entry) + "\n")
    except OSError:
        pass


def _proxy_game(path):
    """GET a path from the game API. Returns (status, content_type, bytes)."""
    url = GAME_API + path
    try:
        with urllib.request.urlopen(url, timeout=GAME_TIMEOUT) as resp:
            body = resp.read()
            ctype = resp.headers.get("Content-Type", "application/octet-stream")
            return resp.status, ctype, body
    except urllib.error.HTTPError as e:
        # Game API returned a real HTTP error (e.g. 404) - pass it through
        # as JSON so the client can still tell what happened.
        try:
            body = e.read()
        except Exception:
            body = b""
        return e.code, "application/json", body
    except (urllib.error.URLError, OSError, TimeoutError):
        return None, None, None


class Handler(BaseHTTPRequestHandler):
    server_version = "ColditzDashboard/1.0"

    def log_message(self, fmt, *args):
        # Keep the console quiet; default logging is noisy for a poll-heavy
        # dashboard (state/screen polled every 500ms-1s).
        pass

    def _send_json(self, status, obj):
        payload = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _send_bytes(self, status, ctype, payload):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _game_offline(self, detail=""):
        self._send_json(502, {"error": "GAME OFFLINE", "detail": detail})

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/" or path == "/index.html":
            self._serve_index()
        elif path == "/api/log":
            self._serve_log(parse_qs(parsed.query))
        elif path == "/api/state":
            self._serve_state()
        elif path == "/api/screen":
            self._serve_screen()
        elif path == "/api/record/status":
            self._record_status()
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/api/record/start":
            self._record_start()
        elif path == "/api/record/stop":
            self._record_stop()
        else:
            self._send_json(404, {"error": "not found"})

    def _read_json_body(self):
        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            length = 0
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        try:
            obj = json.loads(raw)
            return obj if isinstance(obj, dict) else {}
        except (json.JSONDecodeError, ValueError):
            return {}

    def _record_start(self):
        body = self._read_json_body()
        n = body.get("n")
        if not isinstance(n, int) or n <= 0:
            n = _next_run_number()

        rc, out, err = _run_recorder(["start", str(n)])
        rc2, out2, err2 = _run_recorder(["status"])
        result = _parse_recorder_status(out2)
        result["n"] = n
        if result["recording"]:
            _append_log_event("say", "RECORDING STARTED")
        self._send_json(200, result)

    def _record_stop(self):
        rc0, out0, err0 = _run_recorder(["status"])
        was_recording = _parse_recorder_status(out0)["recording"]

        _run_recorder(["stop"])
        rc, out, err = _run_recorder(["status"])
        result = _parse_recorder_status(out)
        if was_recording:
            _append_log_event("say", "RECORDING STOPPED")
        self._send_json(200, result)

    def _record_status(self):
        rc, out, err = _run_recorder(["status"])
        result = _parse_recorder_status(out)
        self._send_json(200, result)

    def _serve_index(self):
        try:
            with open(INDEX_PATH, "rb") as f:
                body = f.read()
        except OSError:
            self._send_json(500, {"error": "index.html missing"})
            return
        self._send_bytes(200, "text/html; charset=utf-8", body)

    def _serve_log(self, query):
        try:
            since = int(query.get("since", ["0"])[0])
        except ValueError:
            since = 0
        if since < 0:
            since = 0

        events = []
        next_idx = since
        try:
            with open(LOG_PATH, "r") as f:
                lines = f.readlines()
        except OSError:
            lines = []

        next_idx = len(lines)
        for line in lines[since:]:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                # Skip a malformed/partial line (e.g. a concurrent writer
                # mid-append) rather than failing the whole poll.
                continue

        self._send_json(200, {"next": next_idx, "events": events})

    def _serve_state(self):
        status, ctype, body = _proxy_game("/state")
        if status is None:
            self._game_offline("could not reach game API at " + GAME_API)
            return
        self._send_bytes(status, ctype or "application/json", body)

    def _serve_screen(self):
        status, ctype, body = _proxy_game("/screen")
        if status is None:
            self._game_offline("could not reach game API at " + GAME_API)
            return
        self._send_bytes(status, ctype or "image/png", body)


def main():
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"Colditz spectator dashboard: http://{HOST}:{PORT}/")
    print(f"Proxying game API at {GAME_API}")
    print(f"Event log: {LOG_PATH}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
