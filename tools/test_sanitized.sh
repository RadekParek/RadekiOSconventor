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
  native/src/apple_time_compat.cpp -o .local/bin/ioscompat-registry-sanitized
.local/bin/ioscompat-registry-sanitized
