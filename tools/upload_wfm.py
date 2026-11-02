#!/usr/bin/env python3
"""Upload PS5Craft title files to the console through ps5-web-file-manager.

The console's ftpsrv.elf hangs when a file starts with the SELF magic, so the
title cannot be installed over FTP. This uses the web file manager's HTTP API:

  POST /api/upload/prepare   (form: path, src, total, count, rels, sizes, overwrite)
  POST /api/upload-file      (headers X-WFM-Task-ID, X-WFM-Path, X-WFM-Rel,
                              X-WFM-Overwrite, X-WFM-Size; raw body)
  POST /api/upload/finish    (header X-WFM-Task-ID)

Usage: upload_wfm.py <console-ip> [port]
"""
import sys
import os
import json
import hashlib
import urllib.request
import urllib.parse

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.15"
PORT = sys.argv[2] if len(sys.argv) > 2 else "8889"
BASE = f"http://{HOST}:{PORT}"

TITLE_DIR = "/data/homebrew/PPSA90104"
FILES = [
    (TITLE_DIR, "eboot.bin", "dist/PPSA90104/eboot.bin"),
    (TITLE_DIR + "/sce_sys", "param.json", "dist/PPSA90104/sce_sys/param.json"),
    (TITLE_DIR + "/sce_sys", "icon0.png", "dist/PPSA90104/sce_sys/icon0.png"),
    (TITLE_DIR + "/sce_module", "libc.prx", "dist/PPSA90104/sce_module/libc.prx"),
    (TITLE_DIR + "/assets", "ca-bundle.crt", "dist/PPSA90104/assets/ca-bundle.crt"),
]


def post(path, data=None, headers=None, timeout=300):
    req = urllib.request.Request(BASE + path, data=data, method="POST")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def prepare(dest_dir, name, size):
    form = urllib.parse.urlencode({
        "path": dest_dir,
        "src": name,
        "total": size,
        "count": 1,
        "rels": name,
        "sizes": size,
        "overwrite": "1",
    }).encode()
    status, body = post("/api/upload/prepare", form,
                        {"Content-Type": "application/x-www-form-urlencoded"})
    doc = json.loads(body)
    if not doc.get("ok"):
        raise RuntimeError(f"prepare failed: {doc}")
    return doc["task_id"]


def upload(task_id, dest_dir, name, local):
    size = os.path.getsize(local)
    with open(local, "rb") as f:
        payload = f.read()
    status, body = post("/api/upload-file", payload, {
        "X-WFM-Task-ID": str(task_id),
        "X-WFM-Path": dest_dir,
        "X-WFM-Rel": name,
        "X-WFM-Overwrite": "1",
        "X-WFM-Size": str(size),
        "Content-Type": "application/octet-stream",
    })
    return status, body


def finish(task_id):
    status, body = post("/api/upload/finish", b"", {
        "X-WFM-Task-ID": str(task_id),
    })
    return status, body


def main():
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    for dest_dir, name, local in FILES:
        want = sha256_file(local)
        size = os.path.getsize(local)
        print(f"\n-> {dest_dir}/{name}  {size} bytes  {want[:16]}…")
        task = prepare(dest_dir, name, size)
        print(f"   task {task}")
        status, body = upload(task, dest_dir, name, local)
        print(f"   upload HTTP {status}: {body[:120]!r}")
        status, body = finish(task)
        print(f"   finish HTTP {status}: {body[:120]!r}")
    print("\nDone.")


if __name__ == "__main__":
    main()
