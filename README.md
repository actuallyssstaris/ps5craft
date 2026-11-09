# PS5Craft

A PlayStation 5 homebrew title for hosting and managing **Minecraft Java Edition**
servers, with a native on-console interface and a browser dashboard on the LAN.

Status: **early development.** The status page at `status/index.html` records what
is verified and what is not. Nothing is claimed as working until it has run.

## What it is

- One native title (installed like any homebrew app), drawn on the console.
- One web dashboard, served from the title itself over the LAN.
- One shared backend used by both; the HTTP API and the native UI call the same
  code, so the two never disagree about server state.

## What it is not

- Not a payload. It is a normal signed title with `eboot.bin` and `sce_sys/`.
- Not a Minecraft client.
- Not a claim that every server flavour works. Support is listed only after it
  runs on the PS5 Java runtime.

## Layout

| Path | Purpose |
| --- | --- |
| `core/` | Shared backend: instances, lifecycle, logs, files, backups, catalog, monitoring |
| `platform/` | Host and PS5 implementations of process, metrics, runtime and input |
| `src/` | Native title: startup, screens, input, HTTP server, dashboard host |
| `web/` | Dashboard assets (HTML, CSS, JavaScript) |
| `tests/` | Host tests for the shared backend |
| `tools/` | Build, deploy, status helpers |
| `tooling/` | ELF/FSELF converter used to sign the title |
| `runtime/` | `libc.prx` runtime module required by the title loader |
| `docs/` | Research notes, architecture, limitations |

## Build

Requirements: Linux, `clang-18`, `lld-18`, `ninja`, `python3`, and the PS5
payload SDK. The native UI kit and the OpenGL SDK are fetched from pinned
mirrors on the first build.

```sh
make app          # builds and signs gui/kit/dist/PPSA90104/
```

Host build for developing and testing the backend:

```sh
make host
make test
```

## Runtime notes

Minecraft needs a Java 25 runtime built for the console. This project does not
bundle one and does not reimplement it: it uses the runtime from
[ps5-java25-homebrew](https://github.com/mateo-cogeanu/ps5-java25-homebrew).
Reusing it has a hard constraint, documented in `docs/research.md`: the payload
SDK cannot `dlopen` arbitrary ELF modules, and the Java port links its core
libraries into the title at build time. This project therefore ships the
management layer and the interface, and its Java bridge reports exactly which
runtime pieces are missing rather than pretending a server started.

## Licences

GPL-3.0-or-later. Reused components keep their own licences and notices; see
`THIRD_PARTY_NOTICES.md`.
