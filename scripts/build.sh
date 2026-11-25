#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
: "${CM0_BUILDROOT_OUTPUT:?Set CM0_BUILDROOT_OUTPUT to a completed czlite Buildroot output}"
build="$root/build"
default_stage="$build/stage"
stage=${CM0_APP_DESTDIR:-$default_stage}
cmake="$CM0_BUILDROOT_OUTPUT/host/bin/cmake"
if [[ ! -x $cmake ]]; then cmake=cmake; fi

mkdir -p "$build"
exec 9>"$build/.device-build.lock"
flock 9
"$root/scripts/prepare-device" "$root" "$build" "$default_stage"
"$cmake" --preset device -S "$root"
"$cmake" --build "$build/device" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
DESTDIR="$stage" "$cmake" --install "$build/device" --component wiliwili-cpz
printf 'Staged wiliwili-cpz in %s\n' "$stage"
