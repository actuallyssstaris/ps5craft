#!/usr/bin/env bash
# PS5Craft - build the pinned native UI kit with the mirrored OpenGL SDK.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The kit's own tools/fetch-opengl-sdk.sh still points at the deleted upstream
# release. This wrapper exports PS5_OPENGL_PREFIX from fetch-kit.sh, which
# selects the verified JMUtechnologies mirror instead, then runs the kit's make
# target.
#
# usage: gui/tools/build-kit.sh [make target] [make arguments]
#   gui/tools/build-kit.sh            build the title
#   gui/tools/build-kit.sh deps       fetch the payload SDK and zlib
#   gui/tools/build-kit.sh host-snapshots
#   gui/tools/build-kit.sh app TITLE_ID=PPSA90104 APP_NAME=PS5Craft

set -euo pipefail

gui=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit="$gui/kit"
target=${1:-app}
if (( $# > 0 )); then
    shift
fi

[[ -d $kit ]] || {
    echo "kit is missing; run gui/tools/fetch-kit.sh first" >&2
    exit 2
}
prefix=$(bash "$gui/tools/fetch-kit.sh" | tail -1)
export PS5_OPENGL_PREFIX="$prefix"

case "$target" in
app | deploy) mode=app ;;
host-snapshots) mode=preview ;;
*) mode= ;;
esac
if [[ $mode == app ]]; then
    runtime=${PS5CRAFT_JVM_RUNTIME:-$kit/.deps/jvm}
    export PS5CRAFT_JVM_RUNTIME="$runtime"
fi
if [[ -n $mode ]]; then
    bash "$gui/tools/prepare-kit.sh" "$mode"
fi
if [[ $mode == app ]]; then
    APP_LINK_RESPONSE=$(bash "$gui/tools/jvm-runtime.sh" "$runtime")
    export APP_LINK_RESPONSE
fi

cd "$kit"
exec make "$target" "$@"
