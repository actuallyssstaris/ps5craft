#!/usr/bin/env bash
# PS5Craft - PS5 title build.
#
# Compiles the shared backend + title entry, links with the payload SDK,
# converts to the PS5 ELF layout and signs it into an installable app folder.
#
# Requires: PS5_PAYLOAD_SDK set to a PS5 payload SDK tree containing
#           target/lib/*.so stubs and prospero-lld.
#
#   PS5_PAYLOAD_SDK=/path/to/sdk tools/build_ps5.sh
#
# Output: dist/PPSA90104/  (installable title folder)

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="${PS5_PAYLOAD_SDK:-/root/projects/PlaySurf/.deps/native/ps5-payload-sdk}"
[[ -d $sdk ]] || { echo "PS5_PAYLOAD_SDK not found at $sdk" >&2; exit 2; }
[[ -x $sdk/bin/prospero-lld ]] || { echo "prospero-lld missing" >&2; exit 2; }

title_id="PPSA90104"
build="$root/build/ps5"
export PATH="$sdk/bin:$PATH"
dist="$root/dist/$title_id"
native="$root/tooling/native"
CLANG="$root/tools/prospero-clang18"
LLD="$sdk/bin/prospero-lld"
pacbrew="/root/projects/PlaySurf/.deps/pacbrew/v0.40.2/sysroot/user/homebrew"

mkdir -p "$build/obj" "$dist"

echo "==> [ps5] building ps5-native-tool (host)"
HOST_CXX=${HOST_CXX:-$(command -v g++ || command -v clang++)}
nt_objs=()
for s in elf_object.cpp sce_module_writer.cpp self_container.cpp native_app_builder.cpp; do
  [[ -f $native/$s ]] || { echo "missing $s" >&2; exit 2; }
  o="$build/obj/nt_$(basename "$s" .cpp).o"
  "$HOST_CXX" -std=c++20 -O2 -Wall -Wextra -I"$native" -c "$native/$s" -o "$o"
  nt_objs+=("$o")
done
"$HOST_CXX" -std=c++20 -O2 "${nt_objs[@]}" -lz -o "$root/tools/ps5-native-tool"
TOOL=$root/tools/ps5-native-tool

cc() { "$CLANG" -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -I"$root/core" -I"$root/platform/host" \
    -I"$root/platform/ps5/include" -I"$root/src" -I"$pacbrew/include" -MD -MF "$2.d" -c "$1" -o "$2"; }

# App objects.
objs=()
for src in core/json.cpp core/hash.cpp core/http_client.cpp core/catalog.cpp core/monitor.cpp core/fs.cpp core/zip.cpp core/backup.cpp core/instance.cpp core/server.cpp core/backend.cpp \
           core/platform_shims.cpp platform/process_host.cpp src/main.cpp src/http_api.cpp; do
  obj="$build/obj/$(basename "$src" .cpp).o"
  cc "$root/$src" "$obj"
  objs+=("$obj")
done

# Startup objects (PS5 CRT + C++ runtime).
for name in app_crt app_cpp_runtime; do
  obj="$build/obj/$name.o"
  cc "$native/$name.cpp" "$obj"
  objs+=("$obj")
done

echo "==> [ps5] linking"
"$LLD" -T "$native/ps5-pie.ld" --eh-frame-hdr \
  --version-script "$native/app-symbols.map" -e _start \
  -o "$build/llvm-pie.elf" "${objs[@]}" \
  "$sdk/target/lib/libScePad.so" \
  "$sdk/target/lib/libSceVideoOut.so" \
  "$sdk/target/lib/libSceSystemService.so" \
  "$sdk/target/lib/libSceUserService.so" \
  "$sdk/target/lib/libkernel.so" \
  -lkernel -lSceLibcInternal \
  "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" \
  "$pacbrew/lib/libmicrohttpd.a" \
  "$pacbrew/lib/libcurl.a" \
  "$pacbrew/lib/libpsl.a" \
  "$pacbrew/lib/libssh2.a" \
  "$pacbrew/lib/libiconv.a" \
  "$pacbrew/lib/libz.a" \
  "$pacbrew/lib/libssl.a" \
  "$pacbrew/lib/libcrypto.a" \
  -L"$sdk/target/lib" --as-needed "$sdk/target/lib"/*.so

echo "==> [ps5] converting to PS5 ELF"
"$TOOL" link --in "$build/llvm-pie.elf" --out "$build/ps5-craft.elf" \
  --stub-dir "$sdk/target/lib"

echo "==> [ps5] signing eboot.bin"
"$TOOL" self --sign --in "$build/ps5-craft.elf" --out "$build/eboot.bin" --magic 0x1D3D154F

echo "==> [ps5] assembling app folder"
rm -rf -- "$dist"
mkdir -p "$dist/sce_sys" "$dist/sce_module" "$dist/assets"
cp "$root/sce_sys/param.json" "$dist/sce_sys/param.json"
cp "$build/eboot.bin" "$dist/eboot.bin"
cp "$root/runtime/libc.prx" "$dist/sce_module/libc.prx"
[[ -d $root/assets ]] && cp -a "$root/assets/." "$dist/assets/"

echo "Build complete."
echo "App folder: $dist"
