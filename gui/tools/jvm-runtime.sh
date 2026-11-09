#!/usr/bin/env bash
# PS5Craft - write the LLVM link response for the embedded Java runtime.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The console title carries HotSpot inside eboot.bin. This script turns the
# unpacked runtime directory into one response file for the kit's link step.
#
# usage: gui/tools/jvm-runtime.sh [runtime-directory]
# Prints the response file path. Set PS5CRAFT_JVM_RUNTIME to override the
# default location (gui/kit/.deps/jvm).

set -euo pipefail

gui=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit="$gui/kit"
runtime=${1:-${PS5CRAFT_JVM_RUNTIME:-$kit/.deps/jvm}}
sdk="$kit/.deps/native/ps5-payload-sdk"

[[ -f $runtime/manifest.json ]] || {
    echo "Java runtime is missing: $runtime/manifest.json" >&2
    echo "Run gui/tools/fetch-jvm.sh or set PS5CRAFT_JVM_RUNTIME." >&2
    exit 2
}
[[ -f $runtime/objects/embedded-jvm.o && -f $runtime/native-libc.a ]] || {
    echo "Java runtime is incomplete: $runtime" >&2
    exit 2
}
[[ -f $runtime/assets/java25/lib/modules ]] || {
    echo "Java boot image is missing: $runtime/assets/java25/lib/modules" >&2
    exit 2
}
[[ -d $sdk/target/lib ]] || {
    echo "payload SDK is missing; run gui/tools/build-kit.sh deps" >&2
    exit 2
}

response="$runtime/link.rsp"
python3 - "$runtime" "$sdk" "$response" <<'PY'
import json
import sys
from pathlib import Path

runtime = Path(sys.argv[1])
sdk = Path(sys.argv[2])
response = Path(sys.argv[3])
objects = sorted((runtime / "objects").glob("*.o"))
if not objects:
    raise SystemExit("runtime has no objects")

values = ["-L" + str(sdk / "target/lib")]
values += [str(path) for path in objects]
values += [
    str(runtime / "native-libc.a"),
    str(sdk / "target/lib/libpthread.a"),
    str(sdk / "target/lib/libc++.a"),
    str(sdk / "target/lib/libc++abi.a"),
    str(sdk / "target/lib/libunwind.a"),
    "--wrap=pthread_create",
    "--wrap=pthread_join",
    "--wrap=fcntl",
    "--defsym=__cxa_thread_atexit_impl=0",
    "--defsym=collector_func_load=0",
    "--defsym=kernel_mprotect=0",
]
response.write_text("\n".join(json.dumps(value) for value in values) + "\n")
print(response)
PY
