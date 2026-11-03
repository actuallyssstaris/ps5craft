# Native UI research (v2)

Date: 2026-11-03. Scope: how PS5 homebrew builds a native, controller-driven UI,
which of those stacks is usable now, and how PS5Craft maps onto one.

## What other homebrew does

### SmartTube for PS5 (`haasele/ytps5`, PPSA99350)

A native YouTube client. Its README states the stack plainly: built on
`ps5-native-app-boilerplate` plus the OpenGL UI kit `ps5-homebrew-ui`.

- One process, one frame loop: read pad, update, draw, present.
- `src/ui/` is a vendored copy of the kit (widgets, glyphs, pixel font, theme,
  motion, confetti). `src/app/` holds the screens: home, search, login,
  subscriptions, settings, player.
- Text entry uses the kit's on-screen keyboard; the pad drives focus.
- Networking is libcurl; settings and tokens persist under `/download0/<app>`.
- Assets (fonts, sounds, images) ship in `/app0/assets`.

So the polished PS5 homebrew look comes from a GPU-drawn UI kit, not from a
web view and not from ImGui defaults.

### `ps5-homebrew-ui` (BlackBearReloaded)

The kit SmartTube uses. GPL-3.0-or-later.

- 21 complete designs, 30 themes, 75 components.
- Components own focus, values, motion and sound: list, grid, dialog, form,
  keyboard, text field, search field, progress, toast, table, tabs, sheet,
  stat, notification, wizard, and more.
- One instanced signed-distance-field shader draws shapes, glyphs and images.
- Host renderer: `make host-snapshots` draws every design on a PC to PNG
  through Mesa surfaceless EGL, so screens can be reviewed without a console.
- Builds against the `ps5-opengl` SDK (OpenGL 4.6 through Mesa on the console).

The upstream repositories were deleted. Mirrors, verified:

| Repository | State | Use |
| --- | --- | --- |
| `JVK3R/ps5-homebrew-ui` | exact upstream head `bbe19054` (2026-10-13), full history | **pin this** |
| `Skystar9999/ps5-homebrew-ui-mirror` | same head `bbe19054` | spare |
| `Phononenom/ps5-homebrew-ui` | same head `bbe19054`, has a release ZIP | spare |
| `J7mbo/ps5-homebrew-ui` | head `0f5742db`, patches the SDK URL to its own repack | avoid (different SDK bytes) |
| `abhi245y/ps5-homebrew-ui` | head `7cf1fcd4`, removes images, notes the original is gone | spare |
| `JMUtechnologies/ps5-homebrew-ui` | single-commit snapshot `51c37b3a` with provenance | spare |

SDK mirrors for `ps5-opengl` 1.0.2 (the upstream release URL is 404):

| Repository | Asset | SHA-256 |
| --- | --- | --- |
| `JMUtechnologies/ps5-opengl` | 226,310,155 bytes | `30d8e51dd062739b629346102be5932728ba714bc50b1bdcdf08306c8843d9a4` |
| `darkxex/ps5-opengl` | same size | same bytes, no checksum file |
| `J7mbo/ps5-opengl` | 40,514,503 bytes | different repack; do not mix with the pin |

The `JMUtechnologies` asset matches the checksum the kit itself pins
(`tools/fetch-opengl-sdk.sh`), so it is a byte-exact replacement for the dead
upstream download.

Boilerplate mirrors (our repo already carries its runtime and converter):
`drakmor/ps5-native-app-boilerplate` is the fullest; `JMUtechnologies`,
`leolobato`, `efkann`, `Phononenom` also carry it.

### Vulkan alternative

`mihawk-99/PS5_VulkanTemplate` is a strong, newer foundation: 16 Vulkan
samples proven on the console, the same UI kit through `PS5_VKHomebrewUI`,
a title generator, and a host reference renderer. It is MIT plus the
GPL-3.0 UI module.

It is not the first choice here:

- It needs `PS5_Vulkan`, `PS5_Mesa` and the `PS5_PayloadSDK` fork checked out
  beside it, then RADV built from Mesa (`tools/build-radv.sh`). No prebuilt
  RADV release assets are published.
- The OpenGL kit has the SmartTube precedent, a host preview and a verified
  SDK mirror today.

Keep Vulkan as the fallback if OpenGL 4.6 on the console turns out to be the
blocker.

## Decision

Build the v2 native UI on **ps5-homebrew-ui (OpenGL)**, pinned to
`bbe19054ee2e7ec069254775273aeb8846c4bd3e`, with the `ps5-opengl` 1.0.2 SDK
from the verified mirror.

Reasons:

1. Same kit the reference app (SmartTube) ships, so the interaction model is
   already proven with a controller.
2. It has the components PS5Craft needs: server list, detail list, forms,
   on-screen keyboard, dialogs, progress, toasts, tabs.
3. Host snapshots let screens be reviewed without a console.
4. The SDK download is available and hash-verified.
5. Our v1 title already uses the same boilerplate runtime pieces
   (`libc.prx`, converter), so the packaging path is familiar.

## How PS5Craft maps onto the kit

| PS5Craft screen | Kit pieces |
| --- | --- |
| Server list (home) | `list` or `grid`, `card`, `badge` for state, `quick_action` |
| Server detail | `tabs` (Console, Files, Backups, Settings), `detail_list`, `stat` |
| Console log | `text_view` / `scroll_area`, `text_field` for commands, `keyboard` |
| Install flow | `wizard` or `dialog`, `select` for software/version, `progress` |
| Backups | `table`, `dialog` for confirm, `toast` for result |
| Settings | `form`, `range_slider` for memory, `switch`, `select` |
| Global | `sidenav`, `notification_center`, `loading_screen`, `hold_button` |

Data comes from the existing `core/` backend in-process: `Backend`,
`InstanceConfig`, `start_instance`, `stop_instance`, logs, file sandbox,
backups, catalog. The HTTP API is not needed inside the UI process; it can
stay for the web dashboard or be dropped from the GUI title.

## Integration shape

```
ps5craft-ui/
  core/            shared backend (unchanged)
  platform/        process supervisor
  gui/
    kit/           pinned ps5-homebrew-ui checkout (gitignored)
    src/ps5craft/  PS5Craft screens and UI glue
    tools/         fetch-kit.sh, build wrappers
```

The kit's build compiles every `src/**/*.cpp`, so the build wrapper links
`gui/src/ps5craft` into `kit/src/ps5craft`. The screens call `core/` through
an include path and an explicit source list added to the build.

## Next steps

1. Fetch the kit at the pin and the SDK from the mirror.
2. Build the untouched kit title in GitHub Actions; deploy to the console and
   confirm it draws and takes input.
3. Add one PS5Craft screen (server list) fed by the real backend.
4. Add start/stop and the console log; then the rest.
