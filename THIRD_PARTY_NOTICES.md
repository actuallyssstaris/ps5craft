# Third-party notices

PS5Craft is GPL-3.0-or-later. The following components are reused rather than
reimplemented. Each keeps its upstream licence.

## Build pipeline and title packaging

- `tooling/native/`, `tools/prospero-clang18`, `runtime/libc.prx`:
  from [PlayKit](https://github.com/actuallyssstaris/playkit), GPL-3.0-or-later.
  PlayKit adapted them from
  [ps5-native-app-boilerplate](https://github.com/BlackBearReloaded/ps5-native-app-boilerplate)
  (BlackBearReloaded, GPL-3.0-or-later).
- `runtime/libc.prx` originates from
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan): a clean-room signed
  runtime module, not copied from any Sony module. GPL-3.0-or-later.

## Research references (no code copied)

- [ProsperoCraft](https://github.com/mateo-cogeanu/ProsperoCraft) — Minecraft
  server launch and JVM option set. GPL-3.0.
- [ps5-java25-homebrew](https://github.com/mateo-cogeanu/ps5-java25-homebrew) —
  OpenJDK 25 PS5 port, runtime packaging, console validation records. GPL-2.0
  with Classpath Exception for OpenJDK portions.
- [websrv](https://github.com/ps5-payload-dev/websrv) — HTTP server patterns,
  libmicrohttpd use, filesystem routes. GPL-3.0.
- [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate),
  [PS5_VKHomebrewUI](https://github.com/mihawk-99/PS5_VKHomebrewUI),
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) — Vulkan bring-up, UI
  kit, controller navigation. MIT / GPL-3.0 per component.
- [BFpilot](https://github.com/ItsBlurf/BFpilot),
  [Prospero Explorer](https://github.com/SvenGDK/Prospero-Explorer),
  [ps5-exporter](https://github.com/Marice/ps5-exporter),
  [PS5_DevTools](https://github.com/griches/PS5_DevTools),
  [ProsperoMgr](https://github.com/notmaj0r/ProsperoMgr) — file operations,
  metrics, diagnostics references.
