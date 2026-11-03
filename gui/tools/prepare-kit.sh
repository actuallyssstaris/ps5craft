#!/usr/bin/env bash
# PS5Craft - place the PS5Craft sources in the pinned kit tree.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The kit builds every src/**/*.cpp it finds.
#
#   prepare-kit.sh preview   copy the portable screen + preview adapter
#   prepare-kit.sh app       copy the console entry, core backend and HTTP API,
#                            and retire the kit's demo main
#
# Idempotent. Fails loudly if the pinned kit no longer has the anchors it
# patches.

set -euo pipefail

gui=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(cd -- "$gui/.." && pwd)
kit="$gui/kit"
source_dir="$gui/src/ps5craft"
target_dir="$kit/src/ps5craft"
mode=${1:-preview}

[[ -d $kit/src/concepts ]] || {
    echo "kit is missing; run gui/tools/fetch-kit.sh first" >&2
    exit 2
}
[[ -d $source_dir ]] || {
    echo "PS5Craft sources are missing: $source_dir" >&2
    exit 2
}

case "$mode" in
preview | app) ;;
*)
    echo "usage: prepare-kit.sh [preview|app]" >&2
    exit 2
    ;;
esac

mkdir -p "$target_dir"

if [[ $mode == preview ]]; then
    # Only the portable screen and its snapshot adapter: the console entry
    # would add a second main() and pull in libcurl/libmicrohttpd.
    rm -rf -- "$target_dir/console" "$target_dir/core" "$target_dir/platform" \
        "$target_dir/src" "$target_dir/main.cpp" "$target_dir/model.cpp" \
        "$target_dir/model.hpp" "$target_dir/runtime.cpp" "$target_dir/runtime.hpp"
    cp -f -- "$source_dir"/*.hpp "$source_dir"/*.cpp "$target_dir"/
    if [[ -f $kit/src/main.cpp.disabled ]]; then
        mv -- "$kit/src/main.cpp.disabled" "$kit/src/main.cpp"
    fi
else
    # The whole tree, including console/, plus the backend it calls. The
    # backend keeps its repo-relative includes by preserving core/, platform/
    # and src/ at the same depth as in the repository.
    rm -rf -- "$target_dir"
    mkdir -p "$target_dir"
    cp -a -- "$source_dir/." "$target_dir/"
    cp -a -- "$repo/core" "$target_dir/core"
    cp -a -- "$repo/platform" "$target_dir/platform"
    mkdir -p "$target_dir/src"
    cp -a -- "$repo/src/http_api.cpp" "$repo/src/http_api.hpp" "$target_dir/src/"
    if [[ -f $kit/src/main.cpp ]]; then
        mv -- "$kit/src/main.cpp" "$kit/src/main.cpp.disabled"
    fi
    # The catalog downloads over HTTPS; the title ships the same bundle as v1.
    if [[ -f $repo/assets/ca-bundle.crt ]]; then
        cp -f -- "$repo/assets/ca-bundle.crt" "$kit/assets/ca-bundle.crt"
    fi
fi

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

echo "==> [ps5craft] $mode sources installed in $target_dir"
