# PS5Craft native UI (v2)

This workspace builds the console UI on the same `core/` backend as the web
dashboard. The stack decision and mirror notes are in
[`../docs/GUI_RESEARCH.md`](../docs/GUI_RESEARCH.md).

- Kit: `ps5-homebrew-ui`, pinned to upstream head
  `bbe19054ee2e7ec069254775273aeb8846c4bd3e`, fetched from the
  `JVK3R/ps5-homebrew-ui` mirror (the upstream repositories were deleted).
- SDK: `ps5-opengl` 1.0.2 from the `JMUtechnologies/ps5-opengl` mirror,
  SHA-256 `30d8e51d…` which matches the checksum the kit pins.

Both are gitignored; fetch them with `gui/tools/fetch-kit.sh`.

## Layout

```
gui/
  kit/            ps5-homebrew-ui checkout (pinned, gitignored)
  src/ps5craft/
    servers_screen.*   portable server list: layout, focus, start/stop request
    preview_concept.cpp  host snapshot adapter with sample rows
    console/           backend model and the console app entry (in progress)
  tools/          fetch-kit.sh, prepare-kit.sh, build-kit.sh
  .deps/          ps5-opengl SDK (gitignored)
```

The kit build compiles every `kit/src/**/*.cpp`, so `prepare-kit.sh` copies
the screens into `kit/src/ps5craft/` and registers the preview design. The
screen itself does not include the backend; the console app supplies rows from
`Backend::list_snapshots()` and performs the request the screen returns.

## Commands

```sh
# Fetch the pinned kit and SDK (idempotent, hash-checked)
gui/tools/fetch-kit.sh

# Fetch the payload SDK and zlib the kit links
gui/tools/build-kit.sh deps

# Render every kit design on the PC, PNGs in gui/kit/build/snapshots
gui/tools/build-kit.sh host-snapshots

# Render only the PS5Craft screen
(cd gui/kit && bash tools/host-snapshots.sh build/snapshots ps5craft)

# Build the installable title (needs x86_64 clang builtins; CI does this)
TITLE_ID=PPSA90104 APP_NAME=PS5Craft gui/tools/build-kit.sh init
gui/tools/build-kit.sh app
```

The kit's own `tools/fetch-opengl-sdk.sh` still points at the deleted upstream
release, so always build through `gui/tools/build-kit.sh`; it exports
`PS5_OPENGL_PREFIX` from the verified mirror.

## Status

- Kit builds and renders on the host: 266 PNGs across 21 designs, exit 0.
- PS5Craft server list renders through the same host path; preview at
  `docs/media/gui-servers.png`.
- Title build is wired in `.github/workflows/gui.yml`. It currently builds the
  untouched kit under the PS5Craft identity; the console app entry that runs
  `Backend` and `ServersScreen` is next.
