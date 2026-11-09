# PS5Craft v2 — development handover

## Folder to continue from

Upload `/root/projects/ps5craft-ui` (the v2 repo). Do **not** upload `/root/projects/ps5craft` — that is the older v1 clone.

Console config lives outside the repo and must travel too:
- `/root/.config/ps5-agent/console.json` (host `10.96.250.8`)
- `/root/.config/opencode/opencode.jsonc` (`PS5_HOST` = `10.96.250.8`)

Single-file eboot uploader: `/tmp/opencode/upload_one.py` (`python3 upload_one.py <host> 8888 <local> <dest_dir>`).

## Where things stand

Repo `https://github.com/actuallyssstaris/ps5craft.git`, branch `main`, title ID `PPSA90104`. Title builds+signs in GitHub Actions into `gui/kit/dist/PPSA90104/`.

The v2 title runs one process: native OpenGL UI + HTTP API on `:8080` + instance supervisor (shared `core/` + `platform/` backend). Web dashboard stays available in the same process.

### Committed (built, deployed to console)

`gui/kit/dist/PPSA90104/` is deployed at `/data/homebrew/PPSA90104/`, `chmod 0777` recursively, title appears in XMB as PS5Craft. Old `eboot.bin` ran and served `:`8080.

### Uncommitted — NOT built, NOT deployed (current work)

9 modified + 2 new files:

- `core/trace.cpp`, `core/trace.hpp` — new direct-fd logger `ps5craft::trace_init`/`trace` (stdout is useless on PS5; payload libc `dup2` is a stub). Log at `/data/ps5craft/title.log` with fallback `/data/homebrew/PPSA90104/title.log`.
- `gui/src/ps5craft/console/main.cpp` — rewritten startup: sync `sceNetInit()` on main thread before any socket/curl, `trace_init` instead of `title_log`/`dup2`, loop `[trace]` fprintf removed, data-root probe traced.
- `gui/src/ps5craft/console/runtime.cpp` — `choose_data_root()` prefers `/data/ps5craft` first (ShadowMountPlus mounts host `/data` into the title sandbox as writable nullfs at the same path).
- `src/main.cpp` — same data-root candidate order for v1.
- `src/http_api.cpp` — MHD `THREAD_PER_CONNECTION | INTERNAL_POLLING_THREAD` with single-thread fallback; `[http]`, `[catalog]` traces; debug fprintf removed.
- `core/http_client.cpp` — curl global-init + get/download traces.
- `core/backend.cpp` — debug fprintf removed; `monitor_snapshot()` sampling moved off `mutex_` onto new `monitor_mutex_`; `instance_disk_bytes()` releases `mutex_` around `sandbox_.tree_size()`.
- `core/backend.hpp` — added `mutable std::mutex monitor_mutex_`.
- `core/platform_shims.cpp` — `fcntl` wrapper: emulates `F_SETFD`/`F_GETFD` (returns 0), forwards everything else to `__real_fcntl`.
- `gui/tools/jvm-runtime.sh` — added `--wrap=fcntl` to the JVM link.

Theory fixed but untested: API hang came from `monitor_sample()` doing `socket()`/`connect()` while holding `Backend::mutex_`, combined with async `sceNetInit()` on a worker thread. Fixes: sync `sceNetInit()` on main thread + monitor sampling off the registry lock. Needs rebuild + console retest.

### Open item raised by user

Homebrew memory limits: performance overlay shows `ram 0.27GB/64.00GB` and `vram 0.00GB/64GB`. `gui/kit/sce_sys/param.json` currently has `attribute3: 1048576` (0x100000), no `kernel.flexibleMemorySize`. Research: `kernel.flexibleMemorySize` ~1 GiB (1073741824, like RetroArch PPSA99169 / MihawkOS PPSA99170) raises flexible-memory budget; bit 22 of `attribute3` (0x400000) = PS5 Pro +1.19 GiB, untested on base. **Confirm intent with the user before editing `param.json`.**

## Console state (10.96.250.8)

- WFM port 8888: open, reachable. `POST /api/mkdir path=/data name=ps5craft` FAILS (`system_error`) — the title must create `/data/ps5craft` itself on first write-probe.
- pldmgr port 8084: open only after `POST /api/launch-elf` on WFM to `/data/pldmgr/payloads/pldmgr/pldmgr_v0.6.3.elf`. Endpoints known: `/payload`, `/payloads`, `/payloads.json`, `/payloads_v2.json`, `/processes_list`, `/process_kill` (POST with header `X-PLDMGR-Request: 1`), `/title`. Exact title-launch route under `/title` not yet identified.
- Port 8080: closed (title not running). Port 53: closed (nanodns not confirmed running).
- Remote Play daemon: was `starting`/`console: unknown`, needs reconnection on new IP. Alternative launch: long-press PS ~2.5 s, Cross, wait for 8080.
- `/data/homebrew/PPSA90104/` exists with old `title.log` (265 bytes). `/data/ps5craft` does not exist.

## Build / deploy / test runbook

1. Rebuild: `cd gui && PACBREW_PACKAGES='libcurl libmicrohttpd' bash tools/build-kit.sh app TITLE_ID=PPSA90104 APP_NAME=PS5Craft` (verify `core/trace.cpp` compiles into kit).
2. Kill old eboot: `POST http://10.96.250.8:8084/process_kill` with header `X-PLDMGR-Request: 1`, body `pid=217` (or current pid from `/processes_list`).
3. Upload new eboot: `python3 /tmp/opencode/upload_one.py 10.96.250.8 8888 gui/kit/dist/PPSA90104/eboot.bin /data/homebrew/PPSA90104` — WFM cannot overwrite a mapped `eboot.bin` (ETXTBSY), so stop the title first.
4. `chmod 0777` recursively on `/data/homebrew/PPSA90104` (launch fails `CE-107750-0` otherwise).
5. Launch the title (Remote Play once reconnected, or pldmgr `/title`/`/payload` endpoint — inspect pldmgr HTML/JS for exact route).
6. Read log: `GET http://10.96.250.8:8888/api/text?path=/data/ps5craft/title.log`. Verify `network init result=0`, `data root /data/ps5craft`, `http result=0`, `display ready`.
7. Verify API without hanging: `/api/status`, `/api/system`, `/api/diag/net/dns`, `/api/diag/net/ip`, `/api/catalog/vanilla/versions`.
8. Install vanilla: `POST /api/instances/Example-Server/install` body `{"software":"vanilla","version":"latest"}`; accept EULA; start; verify JVM boot, server log, API state, native UI.

## Rules carried over

- No fake implementations or hardcoded success; no feature claimed complete until tested.
- Only Vanilla, Fabric, Paper selectable; other server types disabled until tested.
- Never force-push; commit logical milestones and report hashes.
- Push finished project to a **new repo** with clean history; never rewrite/force-push `actuallyssstaris/ps5craft`.
- If the console asks for a system update, answer NO / "ultérieurement"; never update.
- Console title files under `/data/homebrew/<TITLEID>/`; filesystem ops restricted to PS5Craft-managed dirs, no traversal.
- GUI work in the copied kit folder (`gui/`), not the v1 repo.
- Replies in English only.
