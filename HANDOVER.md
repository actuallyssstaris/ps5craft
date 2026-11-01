# PS5Craft — development handover

## Where things stand (2026-11-01)

Repo: `https://github.com/actuallyssstaris/ps5craft` (public)
Title ID: `PPSA90104`
Overall: **~50%** — research done, core backend done, HTTP API + dashboard validated end-to-end on host, file manager + backup manager implemented (backup manager NOT yet compiling — fix needed). PS5 title builds + signs into `dist/PPSA90104/`.

## What works (host-verified)

**Shared backend** (`core/`):
- `json.hpp/cpp` — minimal JSON value, parser, writer (round-trip tested)
- `fs.hpp/cpp` — sandboxed filesystem: list/read/write/mkdirs/rename/remove/copy/tree_size; lexical `resolve()` rejects `..`, absolute paths, NUL bytes
- `zip.hpp/cpp` — minimal ZIP writer (STORE method) + list + extract; standard .zip readable by any tool (NOT yet compiled into tests due to backup.cpp errors)
- `instance.hpp/cpp` — state machine: stopped→starting→running→stopping→crashed/error; one JVM per process
- `server.hpp/cpp` — Minecraft server wrapper: builds JVM command line, spawns supervised process, stop with timeout+kill, eula/server.properties writer, stdin write (save-off/save-on)
- `backend.hpp/cpp` — instance registry + file-manager ops + backup ops declarations; all mutex-guarded, JSON snapshot
- `platform/process.hpp/cpp` — fork+execve, pipe-based stdout/stderr capture into ring-buffer log, waitpid poll/blocking, signal, stdin write

**HTTP API** (`src/http_api.cpp`) — libmicrohttpd (websrv model):
- GET `/api/instances` — list all instances
- GET/POST `/api/instances/<id>` — instance snapshot
- POST `/api/instances/<id>/start|stop|restart`
- GET `/api/fs/<path>` — sandboxed directory listing or file read
- POST `/api/fs/<path>?action=write|mkdir|rename|delete|copy` — file ops
- GET `/api/backup/<id>` — list backups for instance (declared in backend.hpp)
- POST `/api/backup/<id>/create?full=1` — create backup
- POST `/api/backup/<id>/<backup>/restore` — restore backup
- POST `/api/backup/<id>/<backup>/delete` — delete backup
- GET `/api/backup/<id>/<backup>/download` — download ZIP
- GET `/api/status`
- CORS enabled; POST body accumulated per-connection

**Web dashboard** (`web/index.html`):
- Polls `/api/instances` every 2s
- Server cards with start/stop/restart, state badges, uptime, exit code/reason, raw JSON
- Validated end-to-end on host via `tests/http_host.cpp`

**Host tests**: `make test` passes (JSON round-trip, fs sandbox/traversal rejection, instance state transitions, backend create/list/remove, process spawn with stdout capture). `make test` currently FAILS to link because backup.cpp doesn't compile (see "broken" below).

**PS5 title** — compiles, links, signs into `dist/PPSA90104/` installable app folder (eboot.bin + sce_sys/param.json + sce_module/libc.prx). elfldr at `192.168.2.15:9021` loaded the title ELF with no error. Console stdout goes to klog (not visible from host).

## Broken right now (fix first)

`core/backup.cpp` does not compile:
1. `backup.cpp:191` — `InstanceState` not declared; fix by including `instance.hpp` (already added) — verify.
2. `backup.cpp:181,187` — `Backend` incomplete type because `backup.hpp` forward-declares `class Backend;` but `backup.cpp` uses `backend_.instance()` / `backend_.stop_instance()`. Fix: `#include "backend.hpp"` in backup.cpp AFTER forward decl, but this creates a circular include (backend.hpp includes backup.hpp). Break the cycle: move `backup_mgr_` usage out of backend.hpp into backend.cpp, or forward-declare only what's needed and include backend.hpp in backup.cpp with the circular include guarded by #pragma once (should be fine since both use #pragma once).
3. `backup.cpp:173` — `std::sort` needs `#include <algorithm>` (added) — verify.
4. `backup.cpp:148` — `sandbox_.stat` doesn't exist; remove that leftover line (already removed — verify current file).

`core/backup.hpp` forward-declares `class Backend;` and `class Instance;` but uses `Backend&` as a member reference — that's fine for declaration, but `backup.cpp` needs the full `Backend` definition to call methods. Circular include: `backend.hpp` → `backup.hpp` → `backend.hpp`. With `#pragma once` this should resolve if backup.cpp includes `backend.hpp`. Verify.

## Key decisions (don't re-litigate)

1. **Runtime model.** Bundle Java 25 runtime as app content inside the signed title (`app0/assets/java25/...`), read `lib/modules` via `fopen`, link `JNI_CreateJavaVM` at build time. ProsperoCraft model. Do NOT `dlopen("libjvm.so")` at runtime — unsupported by the payload SDK.
2. **One JVM per process.** Parallel Minecraft instances need separate processes; homebrew cannot `fork`/`execve` arbitrary ELF.
3. **HTTP = libmicrohttpd** (websrv model), keep-alive, polling. No WebSocket route.
4. **Filesystem sandbox.** All ops confined to a data root; lexical `resolve()` rejects `..`/absolute/NUL.
5. **Graphics.** Screen abstraction NOT yet written — Vulkan native UI blocked until SDK deps available.
6. **Console access.** Real PS5 at `192.168.2.15`: elfldr 9021 (accepts ELF uploads), FTP 2121 (read-only — ftpsrv env issue). Use elfldr for payload testing; signed title folders install via the normal homebrew launcher.

## Environment limits on this host

- RAM ~7 GB, ~6 GB free disk. Mesa/RADV + OpenJDK full cross-builds won't fit here.
- No macOS host → cannot run upstream ps5-java25-homebrew build tools (need host JDK 25 + clang-18 + LLD 22 + brew).
- `PS5_PAYLOAD_SDK=/root/projects/PlaySurf/.deps/native/ps5-payload-sdk` is the stock SDK (no ps5platform, no RADV).

## Repository layout

```
core/           shared backend (JSON, fs, zip, backup, instance, server, backend, shims)
platform/       host process supervisor (+ ps5 include shims for microhttpd.h)
src/            native title: main.cpp, http_api.cpp (+hpp)
web/            dashboard index.html (polling API)
tests/          test_main.cpp (unit), http_host.cpp (end-to-end API harness)
tools/          status_server.py, build_ps5.sh, prospero-clang18
tooling/native/ PlayKit ELF/FSELF converter + libc.prx (copied, licensed)
runtime/libc.prx signed runtime module
sce_sys/param.json title identity PPSA90104
docs/research.md all 12-repo research findings
status/         live build-status page (served by tools/status_server.py)
dist/PPSA90104/ built PS5 title folder (installable)
```

## Next steps in order

1. **Fix backup.cpp compilation** (see "Broken right now"). Then `make test` passes including backup/zip code.
2. **Console / logs** — live tail of the log ring buffer over HTTP (`/api/console/<id>` GET). Logs captured in `LogRing` but not yet exposed over HTTP.
3. **Server catalog** — Mojang version manifest + Paper API + Fabric meta; download + SHA-256 verify into instances (`/api/catalog/*`).
4. **Configuration editor** — edit server.properties via `/api/fs/<path>?action=write` (already exposed at API level).
5. **Monitoring** — system metrics (CPU/memory/storage) read-only, labeled system-wide.
6. **Native Vulkan UI** — port once SDK deps available; screens behind a screen abstraction.
7. **Host integration test** — run a real Minecraft server JAR under the backend on host.
8. **Backups verification** — test create → modify → restore → verify actually works.

## Blockers

- RADV / ps5platform SDK deps missing → Vulkan native title can't build fully yet.
- macOS OpenJDK build host missing → can't rebuild ps5-java25-homebrew runtime here; bundled-runtime model still works on console.
- Console FTP read-only → title folder must be installed via homebrew launcher, not FTP upload.

## Console testing address

PS5 at `192.168.2.15`. elfldr port 9021 accepts ELF payloads. FTP 2121 is read-only. Deploy via elfldr; signed title folders install through the homebrew launcher normally.

## Commands used

Status page (dev host, LAN reachable at http://192.168.2.16:8090/):
```sh
cd /root/projects/ps5craft
python3 tools/status_server.py --port 8090
```

Run unit + API tests:
```sh
make test                    # unit tests (currently broken by backup.cpp)
./build/host/ps5craft_http &  # then curl http://127.0.0.1:18090/api/instances
```

Build PS5 title:
```sh
PS5_PAYLOAD_SDK=/root/projects/PlaySurf/.deps/native/ps5-payload-sdk \
  tools/build_ps5.sh   # -> dist/PPSA90104/
```

Deploy ELF to console (testing):
```sh
python3 - <<'EOF'
import socket
s=socket.create_connection(('192.168.2.15',9021),timeout=15)
s.sendall(open('build/ps5/ps5-craft.elf','rb').read()); s.shutdown(socket.SHUT_WR)
try: print(s.recv(4096))
except socket.timeout: print('no reply (title may be running)')
s.close()
EOF
```

Git: push per milestone to GitHub, never force-push. Current branch `main`, remote `https://github.com/actuallyssstaris/ps5craft.git`.

## Don't forget

- No feature claimed complete until it ran on the PS5 Java runtime.
- No placeholder success responses, no fake implementations.
- Update `status/status.json` after every coherent change.
- Push per milestone to GitHub, never force-push.
