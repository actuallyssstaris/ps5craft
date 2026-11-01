# Panel research: Pelican, Pterodactyl, Hydrodactyl

What these panels do, and which parts fit a PS5 homebrew title that manages
Minecraft Java servers on the console itself. Sources: pelican.dev/docs,
docs.pterodactyl.io, hydrodactyl.dev (read 2026-11-01).

## What each one is

**Pterodactyl** — the reference design. Two parts: *Panel* (web UI, PHP/Laravel)
and *Wings* (node daemon that runs each server in a Docker container). The
Panel never touches a server process directly; it talks to Wings over a
token-authenticated API. Client API under `/api/client`, admin API under
`/api/application`.

**Pelican** — a fork of Pterodactyl that has grown past it. Same Panel/Wings
split, rebuilt UI (Laravel + Filament), one-click install templates ("eggs"),
themes, OAuth/SSO, better backup handling. Still Docker-based.

**Hydrodactyl** — a Pterodactyl fork focused on the front end: Vite build,
faster renders, accessibility as a stated goal. Feature set is Pterodactyl's.

## Features that define the category

| Area | What the panels do |
| --- | --- |
| Server list | Grid or table of servers with status dot, software, address, resource bars |
| Console | Live output over WebSocket, command input on the same screen |
| Power | Start / Restart / Stop grouped in one control, always visible |
| Files | Breadcrumb browser, create/edit/rename/delete/archive, drag upload |
| Backups | Named backups, download, restore, delete, retention and locks |
| Schedules | Cron tasks that run power actions, commands, or backups |
| Users | Subusers with per-feature permissions, activity log |
| Resources | CPU, memory, disk, network graphs per server |
| Startup | Variables (Java version, heap, jar, args) edited per server |
| Network | Port allocations, SFTP endpoint |
| Admin | Nodes, locations, nests/eggs, users, API keys |

## What PS5Craft adopts

1. **Two-level navigation.** Global server list first, then one server with its
   own sections. This is the strongest pattern in all three panels and it
   scales from one server to many.
2. **Console is the landing view.** Power controls and command input stay on
   the console screen. No separate "send command" page.
3. **Status is dot + text, never colour alone.** Hydrodactyl's accessibility
   push; also survives colour-blind users and screenshots.
4. **Grid and table views** for the server list, choice remembered locally.
   Grid for few servers, table for many.
5. **Labelled resource bars with an explicit "—" when unknown.** No invented
   zeroes. PS5Craft reports what it actually has: heap limit, uptime, PID,
   exit code, and free space on the data volume.
6. **Breadcrumb file manager** with a plain text editor, matching the panels'
   file section, restricted to the instance directory.
7. **Backups as first-class rows**: size, time, restore, download, delete.
8. **Empty states that name the next action.**
9. **Keyboard and screen-reader basics**: real buttons, focus outlines,
   `aria-live` toasts, semantic tables, native `<dialog>` focus handling.
10. **One backend for web and native UI.** The panels separate UI from node
    daemon; PS5Craft keeps the same split inside one process (`core/` is the
    daemon, HTTP and the future Vulkan UI are clients).

## What PS5Craft does not copy

- **Docker isolation.** A homebrew title cannot run containers. Instances are
  separate JVMs in separate directories, which is the honest limit.
- **Multi-node.** One console, one node.
- **Subusers and permissions.** Requires accounts and authentication. Not
  implemented, so no UI is shown for it.
- **SFTP, databases, port allocations, billing.** Out of scope.
- **Resource graphs.** No reliable per-process CPU sampling on the PS5 yet.
  Showing a fake graph would be worse than showing nothing.
- **Schedules.** Useful, but not implemented. No placeholder tab.

## Resulting screen map

```
Top bar        brand · connection state · clock · New server
Sidebar        All servers · instance list with status dots · volume usage
Overview       summary tiles · grid/table toggle · server cards with disk bar
Server         name + state · power control · stats
  Console      log tail + command input
  Files        breadcrumbs · table · edit dialog
  Backups      create world/full · list · restore/download/delete
  Settings     real config values · EULA · delete
```

## What was taken from each panel (2026-11-01)

Two references were fetched and mixed:

- **PS5SX2** — `ps5/frontend/assets/web/index.html` from
  `Swordpdf/PS5SX2` (the console's own settings page).
- **Pelican** — `resources/css/*` and the server entry / small stat / file
  list Blade templates from `pelican/panel`.

From **PS5SX2**, the visual language:

- Deep blue-violet background with a soft radial glow and a vertical gradient.
- Translucent rounded cards (18 px) with 1 px white borders at 9% opacity.
- Pill controls: segmented tabs with a white active pill and violet glow,
  pill buttons, pill inputs, pill toasts.
- Uppercase, letter-spaced section labels in a faint colour.
- Green status dot with glow and a slow pulse; amber tag pills; red danger.
- Wordmark with the second word in the accent colour.

From **Pelican**, the information architecture:

- Left sidebar with the server list and a storage card at the bottom.
- Server cards led by a coloured status stripe, name plus uptime, a power
  action group, and labelled resource bars (current / limit).
- Dashboard summary tiles above the list.
- Console-first server page, with Files, Backups and Settings as peers.
- Table alternative for many servers.

What PS5Craft adds or changes:

- Resource bars show only measured values. Disk is the real size of the
  instance directory; heap is shown as a configured limit, not as usage.
- The status stripe colour and the dot come from the same state value, so
  they never disagree.
- The clock and connection indicator are real, not decorative.

