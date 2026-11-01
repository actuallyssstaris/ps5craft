# PS5Craft — development handover

## Where things stand (2026-11-01)

Repo: `https://github.com/actuallyssstaris/ps5craft` (public, branch `main`)
Title ID: `PPSA90104`
Overall: **~72%**. Core backend, real process supervision, backups, console/logs, persistence, the dashboard and the server software catalog are implemented and host-tested. PS5 title builds + signs into `dist/PPSA90104/`.

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
- `hash` — SHA-1 and SHA-256 (FIPS 180-4), in-memory and streaming file hashing
- `http_client` — libcurl wrapper: GET to memory, GET to file with progress, CA bundle override, timeouts, low-speed abort
- `catalog` — Mojang manifest v2, Paper Fill v3, Fabric meta; resolves version → URL + SHA-1/SHA-256 + size + required Java major
- `backend` — registry + persistence (`instances.json`), lifecycle, file ops, backups, logs/commands/eula; software gating: only `vanilla`, `fabric`, `paper`; background install jobs with progress

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
- `GET /api/catalog`, `GET /api/catalog/<software>/versions`, `GET /api/catalog/<software>/<version>`
- `POST /api/instances/<id>/install` (JSON `{software, version}`) and `GET /api/instances/<id>/install` for progress
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

## Server catalog (host-verified, 2026-11-01)

Downloads follow the Pelican vanilla egg path (manifest → version JSON →
`downloads.server.url`), with two additions Pelican does not do: the
publisher checksum is verified before the JAR is moved into place, and the
download runs as a background job with visible progress.

| Software | Source | Checksum |
|---|---|---|
| Vanilla | `piston-meta.mojang.com` manifest v2 → version JSON | SHA-1 |
| Paper | `fill.papermc.io/v3` (the v2 API is sunset) | SHA-256 |
| Fabric | `meta.fabricmc.net/v2` loader + installer | none published |

Verified end to end on the host with Minecraft **1.21.11** (Java 21):

- **Vanilla**: installed by the API, 56,327,581 bytes, SHA-1
  `64bb6d76…` matched. Start → `Done (12.049s)!`, command answered,
  backup + restore, clean stop.
- **Paper**: build 132, 54,846,016 bytes, SHA-256 `5ffef465…` matched.
  Start → `Done (58.382s)!` (it remaps the server on first boot),
  command answered, clean stop, exit 0.
- **Fabric**: launcher jar installed (no published checksum; the response
  says so), loader 0.20.1 + installer 1.1.2. First start downloaded the
  libraries, `Done (21.816s)!`, command answered, clean stop, exit 0.

Install behaviour:

- Refused while the server is running, starting or stopping.
- Refused for software that is not in the supported list.
- Downloads to `server.jar.part`, verifies hash and size, then renames.
- On mismatch the partial file is deleted and the job reports the expected
  and actual hash.
- The instance `version`, `software` and `jar_path` are persisted after a
  successful install.

PS5 notes:

- `libcurl.a` from the pacbrew sysroot links statically; the title links
  `libcurl`, `libpsl`, `libssh2`, `libzstd`, `libiconv`, `libssl`,
  `libcrypto`, `libz`.
- The console has no system CA store. Put a Mozilla bundle at
  `<data root>/ca-bundle.crt` or set `PS5CRAFT_CA_BUNDLE`.
- Paper Fill requires a descriptive User-Agent with contact information;
  `http_client` sends `PS5Craft/0.1 (+https://github.com/actuallyssstaris/ps5craft)`.

Not done yet: the dashboard still has no catalog picker. The API is
complete and tested; the UI is the next piece.

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
6. **Downloads use libcurl.** Static on PS5 (OpenSSL backend), shared on
   host. No hand-rolled TLS, no shelling out to `curl`/`wget`/`jq`.
7. **Graphics.** Native Vulkan UI still blocked (see Blockers).

## Next steps in order

1. **Catalog UI** — software + version picker in the dashboard, install
   button, progress bar, honest errors. The API already returns everything
   the page needs.
2. **PS5 CA bundle** — ship `ca-bundle.crt` with the title and document the
   path so HTTPS works on the console.
3. **Monitoring** — CPU/memory/storage, read-only, only values the console
   can measure.
4. **Console verification** — install the signed title, run a Vanilla
   26.3 server with the bundled Java 25, open the dashboard from another
   machine.
5. **Native Vulkan UI** — once the pinned PS5_PayloadSDK fork and RADV
   archive are available.

## Blockers

- RADV / ps5platform SDK deps missing → Vulkan native title can't build fully.
- macOS OpenJDK build host missing → can't rebuild ps5-java25-homebrew runtime here.
- Console FTP read-only → install the title through the homebrew launcher, not FTP.

## Tests

`make test` covers: JSON round-trip, fs sandbox + traversal rejection, ZIP write/read/extract, instance lifecycle through the real supervisor with a fake server (start, logs, command, stop, exit code), backup create/restore/remove + traversal rejection + restore-while-running refusal, backend registry, process spawn/output capture.

## Repository layout

```
core/           shared backend (json, hash, http_client, catalog, fs, zip, backup,
                instance, server, backend, shims)
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
- A catalog entry is only marked `supported` after it ran on this build.
- Push per milestone to GitHub, never force-push.
