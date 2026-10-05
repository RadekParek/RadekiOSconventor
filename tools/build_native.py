#!/usr/bin/env python3
"""Portable local build for machines with a C++17 compiler (CMake is optional)."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / ".local/bin"
out.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CXX", "g++")
flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root / "native/include")]
api_source = root / "native/src/apple_time_compat.cpp"
shim_source = root / "native/src/radek_ios_shims.cpp"
display_link_source = root / "native/src/cad_display_link_compat.cpp"
subprocess.run(
    [
        compiler,
        *flags,
        str(root / "native/src/macho.cpp"),
        str(root / "native/src/trivial.cpp"),
        str(root / "native/src/main.cpp"),
        "-o",
        str(out / "radek-macho"),
    ],
    check=True,
)
subprocess.run(
    [compiler, *flags, str(root / "native/tests/runtime.cpp"), "-o", str(out / "runtime-tests")], check=True
)
subprocess.run([str(out / "runtime-tests")], check=True)
subprocess.run(
    [
        compiler,
        *flags,
        str(root / "native/src/macho.cpp"),
        str(root / "native/src/trivial.cpp"),
        str(root / "native/tests/trivial.cpp"),
        "-o",
        str(out / "trivial-tests"),
    ],
    check=True,
)
subprocess.run([str(out / "trivial-tests")], check=True)
api_source = root / "native/src/apple_time_compat.cpp"
registry_source = root / "native/src/ioscompat_registry.cpp"
subprocess.run(
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
    check=True,
)
subprocess.run(
    [
        compiler,
        *flags,
        str(root / "native/tests/apple_time_compat.cpp"),
        str(api_source),
        "-o",
        str(out / "api-compat-tests"),
    ],
    check=True,
)
subprocess.run([str(out / "api-compat-tests")], check=True)
subprocess.run(
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
    check=True,
)
subprocess.run([str(out / "ioscompat-registry-tests")], check=True)
subprocess.run(
    [
        compiler,
        *flags,
        str(root / "native/tests/radek_ios_shims.cpp"),
        str(shim_source),
        "-o",
        str(out / "ios-shims-tests"),
    ],
    check=True,
)
subprocess.run([str(out / "ios-shims-tests")], check=True)
subprocess.run(
    [
        compiler,
        *flags,
        str(root / "native/tests/cad_display_link_compat.cpp"),
        str(display_link_source),
        "-o",
        str(out / "cad-display-link-tests"),
    ],
    check=True,
)
subprocess.run([str(out / "cad-display-link-tests")], check=True)
print(
    "Native analyzer, portable runtime, Apple time API, bounded C/POSIX/CoreFoundation shims, "
    "CADisplayLink frame bridge, and compatibility registry tests passed"
)

compat_sources = [
    root / "native/src/compat_runtime/guest_memory.cpp",
    root / "native/src/compat_runtime/shim_registry.cpp",
    root / "native/src/compat_runtime/macho_loader.cpp",
    root / "native/src/compat_runtime/runner.cpp",
    root / "native/src/compat_runtime/cpu.cpp",
    root / "native/src/compat_runtime/objc_runtime.cpp",
]
subprocess.run(
    [
        compiler,
        *flags,
        *map(str, compat_sources),
        str(root / "native/tests/compat_runtime.cpp"),
        "-o",
        str(out / "compat-runtime-tests"),
    ],
    check=True,
)
subprocess.run([str(out / "compat-runtime-tests")], check=True)
subprocess.run(
    [
        compiler,
        *flags,
        str(root / "native/src/compat_runtime/objc_runtime.cpp"),
        str(root / "native/tests/compat_runtime_objc.cpp"),
        "-o",
        str(out / "compat-runtime-objc-tests"),
    ],
    check=True,
)
subprocess.run([str(out / "compat-runtime-objc-tests")], check=True)
print("compat-runtime-v1 guest memory, Mach-O/dyld, shim registry, CPU boundary, and Objective-C tests passed")
