# PS5Craft — development handover

## Where things stand (2026-11-01)

Repo: `https://github.com/actuallyssstaris/ps5craft` (public)
Title ID: `PPSA90104`
Overall: **45%** — research done, core backend done, HTTP API + dashboard validated on host.

## What works (host-verified)

- **core/json** — minimal JSON value, parser, writer (round-trip tested)
- **core/fs** — sandboxed filesystem: list/read/write/mkdirs/rename/remove/remove_tree/copy; path traversal rejected (`..`, absolute, NUL)
- **core/instance** — state machine: stopped→starting→running→stopping→crashed/error; one JVM per process
- **core/server** — Minecraft server wrapper: builds JVM command line, spawns supervised process, stop with timeout+kill, eula/server.properties writer
- **core/backend** — instance registry: create/list/remove/start/stop/restart, JSON snapshot
- **platform/process** — fork+execve, pipe-based stdout/stderr capture into ring-buffer log, waitpid poll/blocking, signal
- **HTTP API** (`src/http_api.cpp`) — libmicrohttpd (websrv model): GET/POST `/api/instances`, GET `/api/instances/<id>`, POST `/api/instances/<id>/start|stop|restart`, GET `/api/fs/<path>` (sandboxed), CORS
- **PS5 title** — compiles, links, signs into `dist/PPSA90104/` installable app folder (eboot.bin + sce_sys + sce_module/libc.prx)
- **web/index.html** — dashboard polling `/api/instances` every 2s; host-validated end-to-end via `tests/http_host.cpp`

## Key decisions (don't re-litigate)

1. **Runtime model.** Bundle Java 25 runtime as app content inside the signed title (`app0/assets/java25/...`), read `lib/modules` via `fopen`, link `JNI_CreateJavaVM` at build time. ProsperoCraft model. Do NOT `dlopen("libjvm.so")` at runtime — unsupported by the payload SDK.
2. **One JVM per process.** Parallel Minecraft instances need separate processes; homebrew cannot `fork`/`execve` arbitrary ELF.
3. **HTTP = libmicrohttpd** (websrv model), keep-alive, polling. No WebSocket route.
4. **Filesystem sandbox.** All ops confined to a data root; lexical `resolve()` rejects `..`/absolute/NUL.
5. **Graphics.** Screen abstraction written now; Vulkan backend dropped in when pinned PS5_PayloadSDK fork + RADV archive are buildable.
6. **Console access.** Real PS5 at 192.168.2.15: elfldr 9021 (accepts ELF uploads), FTP 2121 (read-only — ftpsrv env issue). Use elfldr for payload testing; signed title folders install via the normal homebrew launcher.

## Environment limits on this host

- RAM ~7 GB, ~6 GB free disk. Mesa/RADV + OpenJDK full cross-builds won't fit here.
- No macOS host → cannot run upstream ps5-java25-homebrew build tools (need host JDK 25 + clang-18 + LLD 22 + brew).
- `PS5_PAYLOAD_SDK=/root/projects/PlaySurf/.deps/native/ps5-payload-sdk` is the stock SDK (no ps5platform, no RADV).

## Repository layout

```
core/           shared backend (done: JSON, fs, instance, server, backend, shims)
platform/       host process supervisor (+ ps5 include shims)
src/            native title: main.cpp, http_api.cpp (+hpp)
web/            dashboard index.html (polling API)
tests/          test_main.cpp (unit), http_host.cpp (end-to-end API harness)
tools/          status_server.py, build_ps5.sh, prospero-clang18
tooling/native/ PlayKit ELF/FSELF converter + libc.prx (copied, licensed)
runtime/libc.prx signed runtime module
sce_sys/param.json title identity PPSA90104
docs/research.md all 12-repo research findings
status/         live build-status page (served by tools/status_server.py)
```

## Next steps in order

1. **File manager** — expose directory listing, read, write, create dir, rename, delete, copy/move via `/api/fs/*` (sandbox already enforces root).
2. **Backups** — zip world dirs to `<root>/backups/<instance>/`, restore with save-aware pause, verify on restore.
3. **Server catalog** — Mojang version manifest + Paper API + Fabric meta; download + SHA-256 verify into instances.
4. **Console / logs** — live tail of the log ring buffer over HTTP; crash state + exit code.
5. **Monitoring** — system metrics (CPU/memory/storage) read-only, labeled system-wide.
6. **Native Vulkan UI** — port once SDK deps available; screens behind screen abstraction.
7. **Host integration test** — run a real Minecraft server JAR under the backend on host.

## Blockers

- RADV / ps5platform SDK deps missing → Vulkan native title can't build fully yet.
- macOS OpenJDK build host missing → can't rebuild ps5-java25-homebrew runtime here; bundled-runtime model still works on console.
- Console FTP read-only → title folder must be installed via homebrew launcher, not FTP upload.

## Console testing address

PS5 at 192.168.2.15. elfldr port 9021 accepts ELF payloads. FTP 2121 is read-only. Deploy via elfldr; signed title folders install through the homebrew launcher normally.

## Commands used

Status page (dev host):
```sh
cd /root/projects/ps5craft
python3 tools/status_server.py --port 8090   # http://192.168.2.16:8090/
```

Run unit + API tests:
```sh
make test          # unit tests
./build/host/ps5craft_http &   # then curl http://127.0.0.1:18090/api/instances
```

Build PS5 title:
```sh
PS5_PAYLOAD_SDK=/root/projects/PlaySurf/.deps/native/ps5-payload-sdk \
  tools/build_ps5.sh   # -> dist/PPSA90104/
```

## Don't forget

- No feature claimed complete until it ran on the PS5 Java runtime.
- No placeholder success responses, no fake implementations.
- Update status.json after every coherent change.
- Push per milestone to GitHub, never force-push.
