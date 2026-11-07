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
    # The console title carries the Java runtime; the kit copies assets/ into
    # the app folder, so the boot image belongs there too.
    runtime=${PS5CRAFT_JVM_RUNTIME:-$kit/.deps/jvm}
    [[ -f $runtime/manifest.json ]] || {
        echo "Java runtime is missing: $runtime/manifest.json" >&2
        echo "Run gui/tools/fetch-jvm.sh or set PS5CRAFT_JVM_RUNTIME." >&2
        exit 2
    }
    rm -rf -- "$kit/assets/java25"
    mkdir -p -- "$kit/assets"
    cp -a -- "$runtime/assets/java25" "$kit/assets/java25"
fi

python3 - "$kit" "$mode" <<'PY'
import sys
from pathlib import Path

kit = Path(sys.argv[1])
mode = sys.argv[2]
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

if mode == "app":
    # HotSpot constructors are collected into their own range so the JVM
    # backend can run them immediately before JNI_CreateJavaVM.
    linker = kit / "tooling/native/ps5-pie.ld"
    text = linker.read_text()
    if "__jvm_init_array_start" not in text:
        anchor = "    .data.rel.ro : {\n        *(.data.rel.ro .data.rel.ro.*)\n    } : relro"
        if anchor not in text:
            raise SystemExit("ps5-pie.ld anchor missing")
        block = """    .data.rel.ro : {
        /* HotSpot constructors stay out of the normal startup path. The JVM
           backend runs them once, immediately before JNI_CreateJavaVM. */
        PROVIDE_HIDDEN(__jvm_init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.data.rel.ro.jvm_init_array.*)))
        KEEP(*(.data.rel.ro.jvm_init_array))
        PROVIDE_HIDDEN(__jvm_init_array_end = .);
        *(.data.rel.ro .data.rel.ro.*)
    } : relro"""
        linker.write_text(text.replace(anchor, block, 1))

    # The kit link step accepts one extra LLVM response file. PS5Craft puts
    # the embedded JVM objects and their linker options there.
    build = kit / "tools/build.sh"
    text = build.read_text()
    if "APP_LINK_RESPONSE" not in text:
        old = 'root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)\n# The app folder is the only output.'
        new = '''root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Optional LLVM response file with extra objects and linker options. PS5Craft
# uses it to link the embedded Java runtime; a stock kit leaves it unset.
link_response=${APP_LINK_RESPONSE:-}
if [[ -n $link_response && ! -f $link_response ]]; then
    echo "APP_LINK_RESPONSE is not a file: $link_response" >&2
    exit 2
fi
# The app folder is the only output.'''
        if old not in text:
            raise SystemExit("build.sh header anchor missing")
        text = text.replace(old, new, 1)
        old = 'ninja_inputs=("$native/ps5-pie.ld" "$native/app-symbols.map" "$sdk_root/bin/prospero-lld")'
        new = '''link_extra=()
[[ -z $link_response ]] || link_extra+=("@$link_response")
ninja_inputs=("$native/ps5-pie.ld" "$native/app-symbols.map" "$sdk_root/bin/prospero-lld")
[[ -z $link_response ]] || ninja_inputs+=("$link_response")'''
        if old not in text:
            raise SystemExit("build.sh inputs anchor missing")
        text = text.replace(old, new, 1)
        old = '''    -e _start -o "$build/llvm-pie.elf" "${link_inputs[@]}" \\
    --as-needed "${stub_paths[@]}" "$sdk_root"/target/lib/*.so'''
        new = '''    -e _start -o "$build/llvm-pie.elf" "${link_inputs[@]}" "${link_extra[@]}" \\
    --as-needed "${stub_paths[@]}" "$sdk_root"/target/lib/*.so'''
        if old not in text:
            raise SystemExit("build.sh link anchor missing")
        text = text.replace(old, new, 1)
        build.write_text(text)
PY

echo "==> [ps5craft] $mode sources installed in $target_dir"
