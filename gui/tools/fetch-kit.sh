#!/usr/bin/env bash
# PS5Craft - fetch the pinned native UI kit and its OpenGL SDK.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The upstream ps5-homebrew-ui repositories were deleted. This script uses
# byte-identical mirrors and verifies both the checkout and the SDK archive:
#
#   kit  JVK3R/ps5-homebrew-ui at bbe19054 (upstream head, 2026-10-13)
#   sdk  JMUtechnologies/ps5-opengl v1.0.2, checksum equals the kit's pin
#
# usage: gui/tools/fetch-kit.sh
# Prints the SDK prefix to export as PS5_OPENGL_PREFIX.

set -euo pipefail

gui=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit="$gui/kit"
cache="$gui/.deps/ps5-opengl"
kit_repo=https://github.com/JVK3R/ps5-homebrew-ui.git
kit_rev=bbe19054ee2e7ec069254775273aeb8846c4bd3e
sdk_version=1.0.2
sdk_sha256=30d8e51dd062739b629346102be5932728ba714bc50b1bdcdf08306c8843d9a4
sdk_url="https://github.com/JMUtechnologies/ps5-opengl/releases/download/v$sdk_version/ps5-opengl-sdk-$sdk_version.tar.gz"
prefix="$cache/ps5-opengl-sdk-$sdk_version/sdk"

if [[ ! -d $kit/.git ]]; then
    mkdir -p "$gui"
    git clone -q "$kit_repo" "$kit"
fi
git -C "$kit" fetch -q origin "$kit_rev" 2>/dev/null || true
git -C "$kit" checkout -q "$kit_rev"
actual=$(git -C "$kit" rev-parse HEAD)
[[ $actual == "$kit_rev" ]] || {
    echo "kit checkout is $actual, expected $kit_rev" >&2
    exit 2
}

if [[ ! -f $prefix/manifest.sha256 ]]; then
    mkdir -p "$cache"
    archive="$cache/ps5-opengl-sdk-$sdk_version.tar.gz"
    if [[ ! -f $archive ]] ||
        ! sha256sum --check --status <<<"$sdk_sha256  $archive"; then
        echo "==> downloading ps5-opengl SDK $sdk_version" >&2
        curl -fL --retry 3 -o "$archive.part" "$sdk_url"
        mv -- "$archive.part" "$archive"
    fi
    sha256sum --check --strict <<<"$sdk_sha256  $archive" >&2
    rm -rf -- "${prefix%/sdk}"
    tar -xzf "$archive" -C "$cache" \
        "ps5-opengl-sdk-$sdk_version/sdk" \
        "ps5-opengl-sdk-$sdk_version/LICENSE" \
        "ps5-opengl-sdk-$sdk_version/LICENSES" \
        "ps5-opengl-sdk-$sdk_version/THIRD_PARTY_NOTICES.md"
fi

(cd "$prefix" && sha256sum --check --strict --quiet manifest.sha256)
echo "$prefix"
