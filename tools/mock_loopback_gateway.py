#!/usr/bin/env python3
import argparse
import json
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class State:
    def __init__(self, out_jsonl: Path | None):
        self.out_jsonl = out_jsonl
        self.started_ns = time.time_ns()
        self.requests = 0
        self.bytes = 0
        self.errors = 0
        self.file = None
        if out_jsonl is not None:
            out_jsonl.parent.mkdir(parents=True, exist_ok=True)
            self.file = out_jsonl.open("a", encoding="utf-8")

    def close(self):
        if self.file is not None:
            self.file.flush()
            self.file.close()


STATE: State | None = None


class Handler(BaseHTTPRequestHandler):
    server_version = "PolytopeMockGateway/1.0"

    def do_POST(self):
        global STATE
        now_ns = time.time_ns()
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if STATE is None:
            self.send_error(500, "state not initialized")
            return

        STATE.requests += 1
        STATE.bytes += len(body)
        record = {
            "recv_ns": now_ns,
            "path": self.path,
            "bytes": len(body),
            "content_type": self.headers.get("Content-Type", ""),
        }
        if STATE.file is not None:
            STATE.file.write(json.dumps(record, separators=(",", ":")) + "\n")
            STATE.file.flush()

        payload = json.dumps(
            {"ok": True, "received": STATE.requests, "bytes": len(body)},
            separators=(",", ":"),
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        global STATE
        if self.path != "/stats" or STATE is None:
            self.send_error(404)
            return
        payload = json.dumps(summary(), separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, _format, *_args):
        return


def summary():
    assert STATE is not None
    elapsed_s = max((time.time_ns() - STATE.started_ns) / 1_000_000_000, 0.0)
    return {
        "runtime_seconds": elapsed_s,
        "requests": STATE.requests,
        "bytes": STATE.bytes,
        "errors": STATE.errors,
        "requests_per_second": STATE.requests / elapsed_s if elapsed_s > 0 else 0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8099)
    parser.add_argument("--out-jsonl", type=Path)
    args = parser.parse_args()

    global STATE
    STATE = State(args.out_jsonl)
    server = ThreadingHTTPServer((args.host, args.port), Handler)

    def stop(_signum, _frame):
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    print(
        f"mock_loopback_gateway listening on http://{args.host}:{args.port}",
        flush=True,
    )
    try:
        server.serve_forever()
    finally:
        print(json.dumps(summary(), indent=2), flush=True)
        STATE.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
