#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .local/bin
"${CXX:-g++}" -std=c++17 -g -O1 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -I native/include native/src/macho.cpp native/src/trivial.cpp native/src/main.cpp \
  -o .local/bin/radek-macho-sanitized
RADEK_ANALYZER="$PWD/.local/bin/radek-macho-sanitized" \
  python3 -m unittest tests.test_macho tests.test_pipeline -v
"${CXX:-g++}" -std=c++17 -g -O1 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -I native/include native/src/macho.cpp native/src/trivial.cpp native/tests/trivial.cpp \
  -o .local/bin/trivial-sanitized
.local/bin/trivial-sanitized
"${CXX:-g++}" -std=c++17 -g -O1 -pthread -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -I native/include native/tests/runtime.cpp -o .local/bin/runtime-sanitized
.local/bin/runtime-sanitized
"${CXX:-g++}" -std=c++17 -g -O1 -pthread -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -I native/include native/tests/ioscompat_registry.cpp native/src/ioscompat_registry.cpp \
  native/src/apple_time_compat.cpp native/src/radek_ios_shims.cpp -o .local/bin/ioscompat-registry-sanitized
.local/bin/ioscompat-registry-sanitized
"${CXX:-g++}" -std=c++17 -g -O1 -pthread -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -I native/include native/tests/radek_ios_shims.cpp native/src/radek_ios_shims.cpp \
  -o .local/bin/ios-shims-sanitized
.local/bin/ios-shims-sanitized
"${CXX:-g++}" -std=c++17 -g -O1 -pthread -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -I native/include native/tests/cad_display_link_compat.cpp native/src/cad_display_link_compat.cpp \
  -o .local/bin/cad-display-link-sanitized
.local/bin/cad-display-link-sanitized
"${CXX:-g++}" -std=c++17 -g -O1 -pthread -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -I native/include native/src/compat_runtime/guest_memory.cpp \
  native/src/compat_runtime/shim_registry.cpp native/src/compat_runtime/macho_loader.cpp \
  native/src/compat_runtime/runner.cpp native/src/compat_runtime/cpu.cpp \
  native/src/compat_runtime/objc_runtime.cpp native/tests/compat_runtime.cpp \
  -o .local/bin/compat-runtime-sanitized
.local/bin/compat-runtime-sanitized
"${CXX:-g++}" -std=c++17 -g -O1 -pthread -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -I native/include native/src/compat_runtime/objc_runtime.cpp \
  native/tests/compat_runtime_objc.cpp -o .local/bin/compat-runtime-objc-sanitized
.local/bin/compat-runtime-objc-sanitized
