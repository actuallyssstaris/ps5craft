#!/usr/bin/env bash
# PS5Craft - fetch the prebuilt OpenJDK 25 runtime used by the native title.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The runtime is built once from the patched ps5-java25 tree. It contains
# HotSpot, the resident java.base libraries, the boot image, and a copy of the
# payload SDK libc whose zlib and shim symbols are isolated from the kit.
#
# usage: gui/tools/fetch-jvm.sh [destination]
# Default destination: gui/kit/.deps/jvm

set -euo pipefail

gui=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dest=${1:-$gui/kit/.deps/jvm}
url=${PS5CRAFT_JVM_URL:-https://github.com/actuallyssstaris/ps5craft/releases/download/jvm-runtime-v1/ps5craft-jvm-25.tar.gz}
sha256=${PS5CRAFT_JVM_SHA256:-84e787076df4058c86deed0afd84073c982d4fc6e324f06f00cc994347a099e9}

command -v curl >/dev/null || {
    echo "curl is required" >&2
    exit 2
}
command -v tar >/dev/null || {
    echo "tar is required" >&2
    exit 2
}

work=$(mktemp -d "${TMPDIR:-/tmp}/ps5craft-jvm.XXXXXX")
trap 'rm -rf "$work"' EXIT

echo "Fetching Java runtime: $url"
curl -fL --retry 3 --retry-delay 2 -o "$work/runtime.tar.gz" "$url"
echo "$sha256  $work/runtime.tar.gz" | sha256sum -c -

rm -rf "$dest"
mkdir -p "$dest"
tar -xzf "$work/runtime.tar.gz" -C "$dest"

for file in manifest.json native-libc.a objects/embedded-jvm.o assets/java25/lib/modules; do
    [[ -f $dest/$file ]] || {
        echo "Java runtime is incomplete: $dest/$file" >&2
        exit 2
    }
done

echo "$dest"
