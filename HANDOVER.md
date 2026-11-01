# PS5Craft — development handover

## Where things stand (2026-11-01)

Repo: `https://github.com/actuallyssstaris/ps5craft` (public)
Title ID: `PPSA90104`
Overall: **16%** — research done, core backend ~30%, nothing on console yet.

## Key decisions (don't re-litigate)

1. **Runtime model.** PS5Craft bundles the Java 25 runtime as app content inside its signed title (`app0/assets/java25/...`), reads `lib/modules` via `fopen`, and links `JNI_CreateJavaVM` at build time. This is exactly what ProsperoCraft does (`server/native/bootstrap.cpp`). Do NOT try `dlopen("libjvm.so")` at runtime: the payload SDK maps `__dlopen` to statically linked core-library symbols only; arbitrary ELF loading is unsupported.
2. **One JVM per process.** JNI allows one VM per process. Parallel Minecraft instances need separate processes; PS5 homebrew cannot `fork`/`execve` arbitrary ELF. State this in the UI.
3. **HTTP = libmicrohttpd** (websrv model), keep-alive, polling. No WebSocket route exists in websrv/MHD here.
4. **Filesystem sandbox.** All ops confined to a `PS5Craft` data root. Lexical `resolve()` rejects `..`, absolute paths, NUL bytes.
5. **Graphics.** Screen abstraction written now; Vulkan backend dropped in when the pinned PS5_PayloadSDK fork + RADV archive are buildable.
6. **Console access.** Real PS5 reachable at 192.168.2.15: elfldr 9021 (accepts ELF uploads), FTP 2121 (read-only). Use elfldr for deploy; don't plan FTP uploads of runtime libs.

## Environment limits on this host

- RAM ~7 GB, ~1 GB free; root disk 6 GB free. Mesa/RADV + OpenJDK full cross-builds won't fit here.
- No macOS host → cannot run the upstream ps5-java25-homebrew build tools (need host JDK 25 + clang-18 + LLD 22 + brew).
- `PS5_PAYLOAD_SDK=/root/projects/PlaySurf/.deps/native/ps5-payload-sdk` is the stock SDK (no ps5platform, no RADV).

## Repository layout

```
core/           shared backend (JSON done, fs done, instances+supervisor+logs next)
platform/       host + PS5 process/metrics/runtime/input shims
src/            native title: startup, screens, input, HTTP, dashboard host
web/            dashboard assets (HTML/CSS/JS, not started)
tests/          host tests (not started)
tools/          status_server.py, build helpers
tooling/native/ PlayKit ELF/FSELF converter + libc.prx (copied, licensed)
runtime/libc.prx signed runtime module
sce_sys/param.json title identity PPSA90104
docs/research.md all 12-repo research findings
status/         live build-status page (served by tools/status_server.py)
probe/          console-side test binaries (jvmprobe.elf loaded via elfldr OK)
```

## Next steps in order

1. **core/instance.hpp/cpp** — instance record, state machine (stopped/starting/running/stopping/crashed), config fields.
2. **core/process.hpp/cpp** — spawn+supervise a child process, capture stdout/stderr to ring-buffer logs, stop/kill, exit detection.
3. **core/server.hpp/cpp** — Minecraft server wrapper over process + logs + properties.
4. **core/backend.hpp/cpp** — singleton manager: instance registry, create/start/stop/restart/delete, JSON snapshot for API.
5. Host unit tests (pure C++, no SDK): JSON, fs sandbox, instance state, process spawn with a real `java` if available.
6. `src/` title skeleton: main, HTTP server wrapping libmicrohttpd, `/api/instances` JSON endpoint.
7. `web/` dashboard: single HTML + JS polling `/api/instances`.
8. PS5 title build: wire PlayKit's `tools/build.sh` path (prospero-clang18 → prospero-lld → native_app_builder → self sign) with title ID PPSA90104.
9. Then: Vulkan UI screens, file manager, backups, catalog, monitoring.

## Blockers

- RADV / ps5platform SDK deps missing → Vulkan native title can't build fully yet. Frontend code stays behind the screen abstraction so it ports later.
- macOS OpenJDK build host missing → can't rebuild ps5-java25-homebrew runtime here; the bundled-runtime model still works on console because the runtime ships as app content.

## Console testing address

PS5 at 192.168.2.15. elfldr port 9021 accepts ELF payloads. FTP 2121 is read-only. Deploy via elfldr; signed title folders install through the homebrew launcher normally.

## Commands used

Status page (dev host):
```sh
cd /root/projects/ps5craft
python3 tools/status_server.py --port 8090   # http://192.168.2.16:8090/
```

Console probe (already done):
```sh
cd /root/projects/ps5craft/probe
python3 - <<'EOF'
import socket
s = socket.create_connection(('192.168.2.15', 9021))
s.sendall(open('jvmprobe.elf','rb').read()); s.shutdown(socket.SHUT_WR)
print(s.recv(4096)); s.close()
EOF
# -> "probe start\n"  (ELF loaded and ran via elfldr)
```

## Don't forget

- No feature claimed complete until it ran on the PS5 Java runtime.
- No placeholder success responses, no fake implementations.
- Update status.json after every coherent change.
- Push per milestone to GitHub, never force-push.
