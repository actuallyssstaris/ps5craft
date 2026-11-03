#!/usr/bin/env bash
# PS5Craft - place the PS5Craft screens in the pinned kit tree.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The kit builds every src/**/*.cpp it finds. This script copies the portable
# screens from gui/src/ps5craft into gui/kit/src/ps5craft and registers the
# host-preview design in the kit's concept list. It is idempotent and fails
# loudly if the pinned kit no longer has the anchors it patches.

set -euo pipefail

gui=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit="$gui/kit"
source_dir="$gui/src/ps5craft"
target_dir="$kit/src/ps5craft"

[[ -d $kit/src/concepts ]] || {
    echo "kit is missing; run gui/tools/fetch-kit.sh first" >&2
    exit 2
}
[[ -d $source_dir ]] || {
    echo "PS5Craft sources are missing: $source_dir" >&2
    exit 2
}

mkdir -p "$target_dir"
cp -f -- "$source_dir"/*.hpp "$source_dir"/*.cpp "$target_dir"/

python3 - "$kit" <<'PY'
import sys
from pathlib import Path

kit = Path(sys.argv[1])
declaration = "std::unique_ptr<app::Concept> make_ps5craft(app::Context &context);"
header = kit / "src/concepts/concepts.hpp"
text = header.read_text()
if declaration not in text:
    anchor = "std::unique_ptr<app::Concept> make_toolbox(app::Context &context);"
    if anchor not in text:
        raise SystemExit(f"concepts.hpp anchor missing: {anchor}")
    text = text.replace(anchor, anchor + "\n" + declaration, 1)
    header.write_text(text)

registry = kit / "src/concepts/registry.cpp"
text = registry.read_text()
entry = "concepts::make_ps5craft,"
if entry not in text:
    anchor = "concepts::make_aurora,"
    if anchor not in text:
        raise SystemExit(f"registry.cpp anchor missing: {anchor}")
    text = text.replace(anchor, entry + " " + anchor, 1)
    registry.write_text(text)
PY

echo "==> [ps5craft] screens installed in $target_dir"
