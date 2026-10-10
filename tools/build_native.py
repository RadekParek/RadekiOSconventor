#!/usr/bin/env python3
"""Portable local build for machines with a C++17 compiler (CMake is optional).

Every unit compiles the exact same sources/flags it always did; the units are
independent, so their compiles run in parallel while the test binaries still
execute serially (same behavior as before, just faster)."""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

root = Path(__file__).resolve().parent.parent
out = root / ".local/bin"
out.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CXX", "g++")
flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root / "native/include")]
api_source = root / "native/src/apple_time_compat.cpp"
shim_source = root / "native/src/radek_ios_shims.cpp"
display_link_source = root / "native/src/cad_display_link_compat.cpp"
registry_source = root / "native/src/ioscompat_registry.cpp"

compat_sources = [
    root / "native/src/compat_runtime/guest_memory.cpp",
    root / "native/src/compat_runtime/shim_registry.cpp",
    root / "native/src/compat_runtime/macho_loader.cpp",
    root / "native/src/compat_runtime/runner.cpp",
    root / "native/src/compat_runtime/cpu.cpp",
    root / "native/src/compat_runtime/objc_runtime.cpp",
    root / "native/src/compat_runtime/objc_shims.cpp",
    root / "native/src/compat_runtime/app_lifecycle.cpp",
    root / "native/src/compat_runtime/audio_session_shims.cpp",
    root / "native/src/compat_runtime/sjlj_unwind.cpp",
    root / "native/src/compat_runtime/trap_shims.cpp",
    root / "native/src/compat_runtime/libsystem_shims.cpp",
    root / "native/src/compat_runtime/gles_shims.cpp",
    root / "native/src/compat_runtime/compiler_rt_shims.cpp",
    root / "native/src/compat_runtime/cxxabi_shims.cpp",
    root / "native/src/compat_runtime/ndk_compat_shims.cpp",
    root / "native/src/compat_runtime/virtual_file_system.cpp",
    root / "native/src/compat_runtime/darwin_compat_shims.cpp",
    root / "native/src/compat_runtime/openal_backend.cpp",
]

# unit = (label, compile argv, run argv or None)
units = [
    (
        "native analyzer",
        [
            compiler,
            *flags,
            str(root / "native/src/macho.cpp"),
            str(root / "native/src/trivial.cpp"),
            str(root / "native/src/main.cpp"),
            "-o",
            str(out / "radek-macho"),
        ],
        None,
    ),
    (
        "portable runtime tests",
        [compiler, *flags, str(root / "native/tests/runtime.cpp"), "-o", str(out / "runtime-tests")],
        [str(out / "runtime-tests")],
    ),
    (
        "trivial analyzer tests",
        [
            compiler,
            *flags,
            str(root / "native/src/macho.cpp"),
            str(root / "native/src/trivial.cpp"),
            str(root / "native/tests/trivial.cpp"),
            "-o",
            str(out / "trivial-tests"),
        ],
        [str(out / "trivial-tests")],
    ),
    (
        "bounded on-device compatibility library",
        [
            compiler,
            *flags,
            "-shared",
            "-fPIC",
            str(api_source),
            str(shim_source),
            str(registry_source),
            str(display_link_source),
            "-o",
            str(out / "libioscompat.so"),
        ],
        None,
    ),
    (
        "Apple time API tests",
        [
            compiler,
            *flags,
            str(root / "native/tests/apple_time_compat.cpp"),
            str(api_source),
            "-o",
            str(out / "api-compat-tests"),
        ],
        [str(out / "api-compat-tests")],
    ),
    (
        "compatibility registry tests",
        [
            compiler,
            *flags,
            str(root / "native/tests/ioscompat_registry.cpp"),
            str(registry_source),
            str(api_source),
            str(shim_source),
            "-o",
            str(out / "ioscompat-registry-tests"),
        ],
        [str(out / "ioscompat-registry-tests")],
    ),
    (
        "bounded C/POSIX/CoreFoundation shim tests",
        [
            compiler,
            *flags,
            str(root / "native/tests/radek_ios_shims.cpp"),
            str(shim_source),
            "-o",
            str(out / "ios-shims-tests"),
        ],
        [str(out / "ios-shims-tests")],
    ),
    (
        "CADisplayLink frame bridge tests",
        [
            compiler,
            *flags,
            str(root / "native/tests/cad_display_link_compat.cpp"),
            str(display_link_source),
            "-o",
            str(out / "cad-display-link-tests"),
        ],
        [str(out / "cad-display-link-tests")],
    ),
]

compat_units = [
    (
        "compat-runtime core tests",
        [
            compiler,
            *flags,
            *map(str, compat_sources),
            str(root / "native/tests/compat_runtime.cpp"),
            "-o",
            str(out / "compat-runtime-tests"),
        ],
        [str(out / "compat-runtime-tests")],
    ),
    (
        "compat-runtime Objective-C tests",
        [
            compiler,
            *flags,
            str(root / "native/src/compat_runtime/objc_runtime.cpp"),
            str(root / "native/tests/compat_runtime_objc.cpp"),
            "-o",
            str(out / "compat-runtime-objc-tests"),
        ],
        [str(out / "compat-runtime-objc-tests")],
    ),
    (
        "compat-runtime trap tests",
        [
            compiler,
            *flags,
            *map(str, compat_sources),
            str(root / "native/tests/compat_runtime_traps.cpp"),
            "-o",
            str(out / "compat-runtime-traps-tests"),
        ],
        [str(out / "compat-runtime-traps-tests")],
    ),
    (
        "compat-runtime libSystem C shim tests",
        [
            compiler,
            *flags,
            *map(str, compat_sources),
            str(root / "native/tests/compat_runtime_libsystem.cpp"),
            "-o",
            str(out / "compat-runtime-libsystem-tests"),
        ],
        [str(out / "compat-runtime-libsystem-tests")],
    ),
    (
        "compat-runtime application lifecycle tests",
        [
            compiler,
            *flags,
            *map(str, compat_sources),
            str(root / "native/tests/compat_runtime_lifecycle.cpp"),
            "-o",
            str(out / "compat-runtime-lifecycle-tests"),
        ],
        [str(out / "compat-runtime-lifecycle-tests")],
    ),
    (
        "compat-runtime Darwin translation layer tests",
        [
            compiler,
            *flags,
            *map(str, compat_sources),
            str(root / "native/tests/darwin_compat.cpp"),
            "-o",
            str(out / "compat-runtime-darwin-compat-tests"),
        ],
        [str(out / "compat-runtime-darwin-compat-tests")],
    ),
    (
        "OpenAL backend",
        [
            compiler,
            *flags,
            *map(str, compat_sources),
            str(root / "native/tests/openal_backend.cpp"),
            "-o",
            str(out / "compat-runtime-openal-tests"),
        ],
        [str(out / "compat-runtime-openal-tests")],
    ),
    (
        "Chained fixups relinker",
        [
            compiler,
            *flags,
            *map(str, compat_sources),
            str(root / "native/tests/macho_chained_fixups.cpp"),
            "-o",
            str(out / "compat-runtime-chained-fixups-tests"),
        ],
        [str(out / "compat-runtime-chained-fixups-tests")],
    ),
    (
        "compat-runtime C++ ABI tests",
        [
            compiler,
            *flags,
            *map(str, compat_sources),
            str(root / "native/tests/compat_runtime_cxxabi.cpp"),
            "-o",
            str(out / "compat-runtime-cxxabi-tests"),
        ],
        [str(out / "compat-runtime-cxxabi-tests")],
    ),
]


def compile_unit(unit):
    label, argv, _ = unit
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"compile failed for {label}:\n{result.stdout}\n{result.stderr}"
        )


def run_units(all_units, workers):
    with ThreadPoolExecutor(max_workers=workers) as pool:
        # Raises (and therefore fails the build) if any compile fails.
        list(pool.map(compile_unit, all_units))
    # Test binaries run serially, exactly as before.
    for label, _, run_argv in all_units:
        if run_argv is None:
            continue
        result = subprocess.run(run_argv, capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode != 0:
            raise SystemExit(f"{label} failed with exit code {result.returncode}")
        print(f"PASS: {label}")


workers = max(1, min(4, os.cpu_count() or 1))
run_units(units, workers)
print(
    "Native analyzer, portable runtime, Apple time API, bounded C/POSIX/CoreFoundation shims, "
    "CADisplayLink frame bridge, and compatibility registry tests passed"
)
run_units(compat_units, workers)
print(
    "compat-runtime-v1 guest memory, Mach-O/dyld, shim registry, CPU boundary,"
    " Objective-C, trap, libSystem C shim, Darwin-only translation layer, full NDK"
    " provider adapters, and bounded application-lifecycle tests passed"
)
