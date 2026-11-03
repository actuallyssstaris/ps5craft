# PS5Craft native UI (v2)

This workspace builds the console UI on the same `core/` backend as the web
dashboard. The stack decision and mirror notes are in
[`../docs/GUI_RESEARCH.md`](../docs/GUI_RESEARCH.md).

- Kit: `ps5-homebrew-ui`, pinned to upstream head
  `bbe19054ee2e7ec069254775273aeb8846c4bd3e`, fetched from the
  `JVK3R/ps5-homebrew-ui` mirror (the upstream repositories were deleted).
- SDK: `ps5-opengl` 1.0.2 from the `JMUtechnologies/ps5-opengl` mirror,
  SHA-256 `30d8e51d…` which matches the checksum the kit pins.

Both are gitignored; fetch them with:

```sh
gui/tools/fetch-kit.sh
export PS5_OPENGL_PREFIX=$(gui/tools/fetch-kit.sh | tail -1)
```

## Layout

```
gui/
  kit/            ps5-homebrew-ui checkout (pinned, gitignored)
  src/ps5craft/   PS5Craft screens and UI glue (tracked here)
  tools/          fetch-kit.sh and build wrappers
  .deps/          ps5-opengl SDK (gitignored)
```

The kit build compiles every `kit/src/**/*.cpp`, so the build wrapper links
`gui/src/ps5craft` into `kit/src/ps5craft` before invoking the kit's `make`.
The screens call the shared `core/` backend in-process.

## Build order

1. Confirm the untouched kit builds and runs first. Its own README documents
   `make deps`, `make`, `make host-snapshots`.
2. Add the PS5Craft screens under `gui/src/ps5craft/`.
3. Link `core/` into the kit build and drive it from the screens.

Nothing in this folder changes the v1 title until the GUI title is ready.
