#!/usr/bin/env python3
"""Serve the PS5Craft status page on the LAN."""
import argparse
import http.server
import functools
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "status"


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()
    server = http.server.ThreadingHTTPServer(
        (args.bind, args.port), functools.partial(Handler))
    print(f"status page: http://{args.bind}:{args.port}/", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
