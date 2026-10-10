#!/usr/bin/env bash
# Builds every sanitized test binary (compiles run concurrently, up to the
# runner's core count) and then executes each suite serially, exactly as
# before. Individual build logs live in .local/sanitized-logs/.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .local/bin .local/sanitized-logs
CXX="${CXX:-g++}"
SAN=(-std=c++17 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
SAN_P=(-std=c++17 -g -O1 -pthread -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
COMPAT_SRCS=(
  native/src/compat_runtime/guest_memory.cpp
  native/src/compat_runtime/shim_registry.cpp
  native/src/compat_runtime/macho_loader.cpp
  native/src/compat_runtime/runner.cpp
  native/src/compat_runtime/cpu.cpp
  native/src/compat_runtime/objc_runtime.cpp
  native/src/compat_runtime/objc_shims.cpp
  native/src/compat_runtime/libsystem_shims.cpp
  native/src/compat_runtime/app_lifecycle.cpp
  native/src/compat_runtime/audio_session_shims.cpp
  native/src/compat_runtime/gles_shims.cpp
  native/src/compat_runtime/compiler_rt_shims.cpp
  native/src/compat_runtime/cxxabi_shims.cpp
  native/src/compat_runtime/ndk_compat_shims.cpp
  native/src/compat_runtime/virtual_file_system.cpp
  native/src/compat_runtime/darwin_compat_shims.cpp
  native/src/compat_runtime/openal_backend.cpp
  native/src/compat_runtime/sjlj_unwind.cpp
  native/src/compat_runtime/trap_shims.cpp
)

MAX_JOBS="$(nproc 2>/dev/null || echo 2)"
UNIT_NAMES=()

start_build() {
  local name="$1"
  shift
  UNIT_NAMES+=("$name")
  ("$@" > ".local/sanitized-logs/$name.log" 2>&1 && touch ".local/sanitized-logs/$name.ok") &
}

throttle() {
  while [ "$(jobs -rp | wc -l)" -ge "$MAX_JOBS" ]; do
    wait -n || break
  done
}

# --- All sanitized binaries, compiled concurrently -----------------------------
start_build radek-macho-sanitized "$CXX" "${SAN[@]}" \
  -I native/include native/src/macho.cpp native/src/trivial.cpp native/src/main.cpp \
  -o .local/bin/radek-macho-sanitized
throttle
start_build trivial-sanitized "$CXX" "${SAN[@]}" \
  -I native/include native/src/macho.cpp native/src/trivial.cpp native/tests/trivial.cpp \
  -o .local/bin/trivial-sanitized
throttle
start_build runtime-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include native/tests/runtime.cpp -o .local/bin/runtime-sanitized
throttle
start_build ioscompat-registry-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include native/tests/ioscompat_registry.cpp native/src/ioscompat_registry.cpp \
  native/src/apple_time_compat.cpp native/src/radek_ios_shims.cpp -o .local/bin/ioscompat-registry-sanitized
throttle
start_build ios-shims-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include native/tests/radek_ios_shims.cpp native/src/radek_ios_shims.cpp \
  -o .local/bin/ios-shims-sanitized
throttle
start_build cad-display-link-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include native/tests/cad_display_link_compat.cpp native/src/cad_display_link_compat.cpp \
  -o .local/bin/cad-display-link-sanitized
throttle
start_build compat-runtime-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include "${COMPAT_SRCS[@]}" \
  native/tests/compat_runtime.cpp \
  -o .local/bin/compat-runtime-sanitized
throttle
start_build compat-runtime-traps-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include "${COMPAT_SRCS[@]}" \
  native/tests/compat_runtime_traps.cpp \
  -o .local/bin/compat-runtime-traps-sanitized
throttle
start_build compat-runtime-objc-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include native/src/compat_runtime/objc_runtime.cpp \
  native/tests/compat_runtime_objc.cpp -o .local/bin/compat-runtime-objc-sanitized
throttle
start_build compat-runtime-lifecycle-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include "${COMPAT_SRCS[@]}" \
  native/tests/compat_runtime_lifecycle.cpp \
  -o .local/bin/compat-runtime-lifecycle-sanitized
throttle
start_build compat-runtime-cxxabi-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include "${COMPAT_SRCS[@]}" \
  native/tests/compat_runtime_cxxabi.cpp \
  -o .local/bin/compat-runtime-cxxabi-sanitized
throttle
start_build compat-runtime-openal-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include "${COMPAT_SRCS[@]}" \
  native/tests/openal_backend.cpp \
  -o .local/bin/compat-runtime-openal-sanitized
throttle
start_build macho-chained-fixups-sanitized "$CXX" "${SAN_P[@]}" \
  -I native/include "${COMPAT_SRCS[@]}" \
  native/tests/macho_chained_fixups.cpp \
  -o .local/bin/macho-chained-fixups-sanitized

wait || true
for name in "${UNIT_NAMES[@]}"; do
  if [ ! -f ".local/sanitized-logs/$name.ok" ]; then
    echo "sanitized build failed: $name" >&2
    cat ".local/sanitized-logs/$name.log" >&2
    exit 1
  fi
done

# --- Serial execution, same order as before ----------------------------------
RADEK_ANALYZER="$PWD/.local/bin/radek-macho-sanitized" \
  python3 -m unittest tests.test_macho tests.test_pipeline -v
.local/bin/trivial-sanitized
.local/bin/runtime-sanitized
.local/bin/ioscompat-registry-sanitized
.local/bin/ios-shims-sanitized
.local/bin/cad-display-link-sanitized
.local/bin/compat-runtime-sanitized
.local/bin/compat-runtime-traps-sanitized
.local/bin/compat-runtime-objc-sanitized
.local/bin/compat-runtime-lifecycle-sanitized
.local/bin/compat-runtime-cxxabi-sanitized
.local/bin/compat-runtime-openal-sanitized
.local/bin/macho-chained-fixups-sanitized
