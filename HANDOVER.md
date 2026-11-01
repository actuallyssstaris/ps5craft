# PS5Craft — development handover

## Where things stand (2026-11-01)

Repo: `https://github.com/actuallyssstaris/ps5craft` (public, branch `main`)
Title ID: `PPSA90104`
Overall: **~62%**. Core backend, real process supervision, backups, console/logs, persistence and the dashboard are implemented and host-tested. PS5 title builds + signs into `dist/PPSA90104/`.

Live pages from the development device:
- Dashboard: `http://192.0.0.4:8080/` (host build of the same HTTP API)
- Build status: `http://192.0.0.4:8090/`

## What works (host-verified)

**Shared backend** (`core/`):
- `json` — minimal JSON value/parser/writer
- `fs` — sandboxed filesystem; lexical `resolve()` rejects `..`, absolute paths, NUL
- `zip` — spec-correct STORE ZIP writer + central-directory reader + extract; rejects traversal names
- `backup` — create (world or full), manifest inside archive, list newest-first, restore (validates archive before deleting current world), delete, traversal-safe backup ids
- `instance` — owns `Server` + `LogRing`; start spawns, stop sends Minecraft `stop` then SIGTERM/SIGKILL at deadline, poll reaps and updates state, restart, stdin commands, log snapshot
- `server` — JVM command line, process spawn, output drain, stop ladder, per-instance `server.properties` and `eula.txt`
- `backend` — registry + persistence (`instances.json`), lifecycle, file ops, backups, logs/commands/eula; software gating: only `vanilla`, `fabric`, `paper`

**Platform** (`platform/process_host.cpp`): fork+execve, stdout/stderr pipe into ring buffer, stdin pipe, waitpid, signals. FDs closed on reap.

**HTTP API** (`src/http_api.cpp`, libmicrohttpd):
- `GET /` — dashboard (`web/index.html`, root from `PS5CRAFT_WEB_ROOT`)
- `GET /api/instances`, `POST /api/instances`, `GET/DELETE /api/instances/<id>`
- `POST /api/instances/<id>/start|stop|restart|eula|command`
- `GET /api/instances/<id>/logs`
- `GET /api/fs/<path>`, `POST /api/fs/<path>?action=write|upload|mkdir|rename|delete|copy`
  (`upload` takes a raw body up to 256 MiB; verified byte-exact with a 56 MB JAR)
- `GET /api/backup/<id>`, `POST /api/backup/<id>/create?full=1`, `POST /api/backup/<id>/<backup>/restore|delete`, `GET /api/backup/<id>/<backup>/download`
- `GET /api/system` — volume capacity + per-instance directory sizes (30 s cache)
- `GET /api/status`; CORS; MHD query args; POST body accumulation fixed

**Dashboard** (`web/index.html`): PS5SX2 visual language (violet gradient, translucent cards, pill controls) with Pelican's information architecture (status-striped server cards, summary tiles, console-first server page). Overview with grid/table views, per-instance disk usage and volume capacity; server detail with console log (2 s poll), inline command input, file manager, backups and settings. Responsive for phone.

**Host binaries**:
- `build/host/ps5craft_test` — unit tests (`make test`)
- `build/host/ps5craft_http` — old socket harness (superseded by `ps5craft_host`)
- `build/host/ps5craft_host` — real `main.cpp` + `http_api.cpp` on libmicrohttpd

**PS5 title**: compiles, links, signs into `dist/PPSA90104/`. elfldr at `192.168.2.15:9021` loads ELF payloads.

## Real Minecraft run (host, 2026-11-01)

Vanilla **1.21.11** (Java 21) was run end to end:

- Server JAR fetched from Mojang, SHA-1 verified, uploaded through
  `POST /api/fs/...?action=upload` (56,327,581 bytes, hash matched on disk).
- Start reached `Done (12.049s)!`, console command `list` answered in the log.
- World backup taken while running (2.27 MB ZIP), then restored while stopped.
- `stop` produced a clean shutdown and exit code 0; TCP 25565 accepted a
  connection.

Two bugs found and fixed by this run:

1. `execve` does not search `PATH`, so a bare `java` failed with exit 127.
   The supervisor now resolves the executable against the child's `PATH`.
2. The JVM rejects relative `-Duser.dir` / `-Djava.io.tmpdir`; the instance
   directory is resolved to an absolute path before building the command,
   and `tmp/` is created before spawn.

Minecraft **26.3** requires Java **25** (`javaVersion.majorVersion: 25`).
The host has Java 21, which is why the real run uses 1.21.11. The PS5 title
bundles Java 25, so it can run 26.3; the catalog must carry this mapping.

## Run the host build

```sh
make host
PS5CRAFT_DATA_ROOT=/tmp/ps5craft_demo \
PS5CRAFT_WEB_ROOT=$PWD/web \
PS5CRAFT_PORT=8080 \
  ./build/host/ps5craft_host
```

Demo data lives in `/tmp/ps5craft_demo`. The `Demo Server` instance uses `tests/fake_java.sh` as its `java_path` so Start/Stop/logs work without a Minecraft JAR. A real run needs a server JAR in the instance dir and `java_path` pointing at a Java 25 binary.

## Key decisions (don't re-litigate)

1. **Runtime model.** Bundle Java 25 runtime as app content inside the signed title (`app0/assets/java25/...`), read `lib/modules` via `fopen`, link `JNI_CreateJavaVM` at build time. Do NOT `dlopen("libjvm.so")`.
2. **One JVM per process.** Parallel instances need separate processes; homebrew cannot fork/exec arbitrary ELF.
3. **HTTP = libmicrohttpd**, polling, no WebSocket.
4. **Filesystem sandbox.** All ops confined to a data root; lexical `resolve()` rejects `..`/absolute/NUL.
5. **Software honesty.** Only Vanilla, Fabric, Paper are selectable. Spigot/Purpur/Pufferfish/Forge/NeoForge/Quilt are shown disabled until tested on the console; the backend rejects them.
6. **Graphics.** Native Vulkan UI still blocked (see Blockers).

## Next steps in order

1. **Server catalog** — Mojang version manifest + Paper API + Fabric meta; download, SHA-256 verify, install into an instance. Needs an HTTP client on PS5; decide between a small TLS client over the existing socket layer and a vendored library.
2. **Real Minecraft run on host** — drop a Vanilla/Paper JAR into an instance, set `java_path` to a Java 25 binary, run a full start → join → backup → stop cycle.
3. **Monitoring** — CPU/memory/storage, read-only, labelled system-wide.
4. **Console verification** — install the signed title, start a server, confirm the dashboard works from another machine.
5. **Native Vulkan UI** — once the pinned PS5_PayloadSDK fork and RADV archive are available.

## Blockers

- RADV / ps5platform SDK deps missing → Vulkan native title can't build fully.
- macOS OpenJDK build host missing → can't rebuild ps5-java25-homebrew runtime here.
- Console FTP read-only → install the title through the homebrew launcher, not FTP.

## Tests

`make test` covers: JSON round-trip, fs sandbox + traversal rejection, ZIP write/read/extract, instance lifecycle through the real supervisor with a fake server (start, logs, command, stop, exit code), backup create/restore/remove + traversal rejection + restore-while-running refusal, backend registry, process spawn/output capture.

## Repository layout

```
core/           shared backend (json, fs, zip, backup, instance, server, backend, shims)
platform/       host process supervisor
src/            native title: main.cpp, http_api.cpp (+hpp)
web/            dashboard index.html
tests/          test_main.cpp (unit), http_host.cpp (legacy harness), fake_java.sh
tools/          status_server.py, build_ps5.sh, prospero-clang18
tooling/native/ PlayKit ELF/FSELF converter + libc.prx
runtime/libc.prx signed runtime module
sce_sys/param.json title identity PPSA90104
docs/research.md all 12-repo research findings
status/         build-status page (tools/status_server.py --port 8090)
dist/PPSA90104/ built PS5 title folder
```

## Commands

```sh
make test                 # unit tests
make host                 # all host binaries
make app                  # PS5 build + sign -> dist/PPSA90104/

PS5_PAYLOAD_SDK=/root/projects/PlaySurf/.deps/native/ps5-payload-sdk tools/build_ps5.sh
```

Deploy ELF to console (testing):

```sh
python3 - <<'EOF'
import socket
s = socket.create_connection(('192.168.2.15', 9021), timeout=15)
s.sendall(open('build/ps5/ps5-craft.elf', 'rb').read())
s.shutdown(socket.SHUT_WR)
try: print(s.recv(4096))
except socket.timeout: print('no reply (title may be running)')
s.close()
EOF
```

## Don't forget

- No feature claimed complete until it ran on the PS5 Java runtime.
- No placeholder success responses, no fake implementations.
- Update `status/status.json` after every coherent change.
- Push per milestone to GitHub, never force-push.
