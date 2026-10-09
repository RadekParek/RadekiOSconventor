"""Android NDK link, verification, and boot-payload preparation for translated C.

The linker emits an ABI-specific Android shared object containing generated
portable-C code, its guest-state runtime, and the GameBootActivity JNI entry.
The separate payload packer binds the translated memory image and provenance to
that verified library. This is a package-ready input, not an APK or proof of
visible rendering/gameplay; the game-runtime APK builder and launcher consume it
in a later step.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import struct
import subprocess
import zipfile
from pathlib import Path

from ..archive import InputError
from ..elf import inspect as inspect_elf


_SUPPORTED_ABIS = {
    "arm64-v8a": ("aarch64-linux-android", 21),
    "armeabi-v7a": ("armv7a-linux-androideabi", 21),
}
_ABI_ELF_CLASSES = {"arm64-v8a": 64, "armeabi-v7a": 32}
_ALLOWED_ANDROID_NEEDED = {"libc.so", "libm.so", "libdl.so"}
_TRANSLATED_GAME_ENTRY_SYMBOL = "Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame"
_TRANSLATED_GAME_PAYLOAD_CONTRACT = "translated-game-payload-v1"
_TRANSLATED_GAME_PAYLOAD_NAME = "translated-game-payload.zip"
_LINK_TIMEOUT_SECONDS = 600


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _host_tags() -> tuple[str, ...]:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "linux":
        return ("linux-x86_64", "linux-aarch64") if machine in ("aarch64", "arm64") else (
            "linux-x86_64", "linux-aarch64"
        )
    if system == "darwin":
        return ("darwin-arm64", "darwin-x86_64") if machine in ("arm64", "aarch64") else (
            "darwin-x86_64", "darwin-arm64"
        )
    if system == "windows":
        return ("windows-x86_64",)
    return ()


def _ndk_roots(environ: dict[str, str] | None = None) -> list[Path]:
    env = os.environ if environ is None else environ
    roots: list[Path] = []
    for key in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "NDK_HOME"):
        value = env.get(key)
        if value:
            roots.append(Path(value).expanduser())
    for key in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        value = env.get(key)
        if not value:
            continue
        sdk = Path(value).expanduser()
        roots.extend(sorted((sdk / "ndk").glob("*"), reverse=True))
        roots.append(sdk / "ndk-bundle")
    unique: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        normalized = os.path.normcase(os.path.abspath(root))
        if normalized not in seen:
            seen.add(normalized)
            unique.append(root)
    return unique


def find_android_clang(environ: dict[str, str] | None = None) -> tuple[Path | None, Path | None, str]:
    """Find the NDK clang driver and sysroot without relying on Gradle/JDK."""
    for root in _ndk_roots(environ):
        prebuilt = root / "toolchains" / "llvm" / "prebuilt"
        for tag in _host_tags():
            toolchain = prebuilt / tag
            clang = toolchain / "bin" / ("clang.exe" if platform.system() == "Windows" else "clang")
            sysroot = toolchain / "sysroot"
            if clang.is_file() and sysroot.is_dir():
                return clang, sysroot, ""
    roots = _ndk_roots(environ)
    if roots:
        return None, None, "Android NDK was configured but its LLVM clang/sysroot was not found"
    return None, None, "no Android NDK was found (set ANDROID_NDK_HOME or ANDROID_SDK_ROOT)"


def _link_failure(status: str, target_abi: str, reason: str) -> dict:
    return {
        "status": status,
        "attempted": False,
        "targetAbi": target_abi,
        "linkedTranslatedFunctionCount": 0,
        "translatedFunctionCount": 0,
        "androidLinkedTextBytes": 0,
        "executableTextBytes": 0,
        "androidLinkedTextPercent": 0.0,
        "architectureVerified": False,
        "dependenciesVerified": False,
        "exportsVerified": False,
        "translationEntryPointVerified": False,
        "entryPointSymbol": _TRANSLATED_GAME_ENTRY_SYMBOL,
        "allVerificationsPassed": False,
        "missingFunctionSymbols": [],
        "linkedIntoGame": False,
        "apkProduced": False,
        "completeGameConversion": False,
        "reason": reason,
    }


def verify_android_translation(
    elf_data: bytes,
    translation_report: dict,
    target_abi: str,
    *,
    artifact_name: str = "libtranslated_game.so",
) -> dict:
    """Verify the Android ELF and the complete generated-function export set."""
    if target_abi not in _SUPPORTED_ABIS:
        return _link_failure("BLOCKED_UNSUPPORTED_ANDROID_ABI", target_abi, "unsupported Android target ABI")
    try:
        elf = inspect_elf(elf_data)
    except (InputError, ValueError, struct.error) as exc:
        return _link_failure("BLOCKED_ANDROID_ELF_VALIDATION", target_abi, str(exc))

    if (
        elf.get("architecture") != target_abi
        or elf.get("elfClass") != _ABI_ELF_CLASSES[target_abi]
    ):
        return _link_failure(
            "BLOCKED_ANDROID_ELF_VALIDATION",
            target_abi,
            f"ELF architecture/class is {elf.get('architecture')!r}/{elf.get('elfClass')!r}, "
            f"expected {target_abi!r}/{_ABI_ELF_CLASSES[target_abi]}",
        )
    dynamic_dependencies = elf.get("needed", [])
    unexpected_needed = sorted(set(dynamic_dependencies) - _ALLOWED_ANDROID_NEEDED)
    if unexpected_needed:
        return _link_failure(
            "BLOCKED_ANDROID_ELF_VALIDATION",
            target_abi,
            "ELF depends on a non-Android or unreviewed library: " + ", ".join(unexpected_needed),
        )

    symbols = translation_report.get("translatedFunctionSymbols")
    if not isinstance(symbols, list) or not symbols or any(
        not isinstance(symbol, str) or not symbol for symbol in symbols
    ):
        return _link_failure(
            "BLOCKED_TRANSLATED_SYMBOL_MANIFEST",
            target_abi,
            "portable-C report does not contain a non-empty translatedFunctionSymbols list",
        )
    if len(symbols) != len(set(symbols)):
        return _link_failure(
            "BLOCKED_TRANSLATED_SYMBOL_MANIFEST",
            target_abi,
            "portable-C report contains duplicate translated function symbols",
        )
    reported_functions = translation_report.get("functions")
    function_failures = translation_report.get("functionFailures", 0)
    if (
        isinstance(reported_functions, bool)
        or not isinstance(reported_functions, int)
        or reported_functions != len(symbols)
        or function_failures != 0
    ):
        return _link_failure(
            "BLOCKED_TRANSLATED_SYMBOL_MANIFEST",
            target_abi,
            "portable-C function count, translated symbol manifest, or function-failure count is inconsistent",
        )

    exports = elf.get("exports", {})
    missing = [symbol for symbol in symbols if symbol not in exports]
    malformed = [
        symbol
        for symbol in symbols
        if symbol in exports
        and (exports[symbol].get("type") != 2 or int(exports[symbol].get("size", 0)) <= 0)
    ]
    if missing or malformed:
        reasons = []
        if missing:
            reasons.append(f"{len(missing)} translated function symbol(s) are missing")
        if malformed:
            reasons.append(f"{len(malformed)} translated symbol(s) are not sized ELF functions")
        result = _link_failure(
            "BLOCKED_TRANSLATED_SYMBOL_VERIFICATION",
            target_abi,
            "; ".join(reasons),
        )
        result["missingFunctionSymbols"] = missing[:128]
        result["malformedFunctionSymbols"] = malformed[:128]
        result["translatedFunctionCount"] = len(symbols)
        return result

    entry_point_symbol = translation_report.get(
        "translationEntryPointSymbol", _TRANSLATED_GAME_ENTRY_SYMBOL
    )
    if not isinstance(entry_point_symbol, str) or not entry_point_symbol:
        return _link_failure(
            "BLOCKED_TRANSLATION_ENTRYPOINT_VERIFICATION",
            target_abi,
            "portable-C report does not name its Android JNI entry point",
        )
    entry_point = exports.get(entry_point_symbol)
    if (
        entry_point is None
        or entry_point.get("type") != 2
        or int(entry_point.get("size", 0)) <= 0
    ):
        result = _link_failure(
            "BLOCKED_TRANSLATION_ENTRYPOINT_VERIFICATION",
            target_abi,
            "Android shared library does not export a sized GameBootActivity JNI entry point",
        )
        result["translationEntryPointVerified"] = False
        result["entryPointSymbol"] = entry_point_symbol
        result["translatedFunctionCount"] = len(symbols)
        return result

    executable_text_bytes = int(translation_report.get("executableTextBytes", 0) or 0)
    translated_text_bytes = int(
        translation_report.get(
            "translatedUniqueTextBytes",
            translation_report.get("translatedFunctionBytes", 0),
        )
        or 0
    )
    if (
        executable_text_bytes <= 0
        or translated_text_bytes <= 0
        or translated_text_bytes > executable_text_bytes
    ):
        return _link_failure(
            "BLOCKED_TRANSLATED_COVERAGE_VERIFICATION",
            target_abi,
            "translated unique instruction bytes are empty or exceed executable __text",
        )

    return {
        "status": "VERIFIED_ANDROID_SHARED_LIBRARY",
        "attempted": True,
        "targetAbi": target_abi,
        "artifact": artifact_name,
        "artifactSizeBytes": len(elf_data),
        "artifactSha256": hashlib.sha256(elf_data).hexdigest(),
        "elfArchitecture": elf["architecture"],
        "elfClass": elf["elfClass"],
        "dynamicDependencies": dynamic_dependencies,
        "dynamicUndefinedSymbols": elf.get("undefinedSymbols", []),
        "architectureVerified": True,
        "dependenciesVerified": True,
        "translatedFunctionCount": len(symbols),
        "linkedTranslatedFunctionCount": len(symbols),
        "translatedUniqueTextBytes": translated_text_bytes,
        "androidLinkedTextBytes": translated_text_bytes,
        "executableTextBytes": executable_text_bytes,
        "androidLinkedTextPercent": round(100.0 * translated_text_bytes / executable_text_bytes, 6),
        "exportsVerified": True,
        "translationEntryPointVerified": True,
        "entryPointSymbol": entry_point_symbol,
        "allVerificationsPassed": True,
        "missingFunctionSymbols": [],
        "malformedFunctionSymbols": [],
        "linkedIntoGame": False,
        "apkProduced": False,
        "completeGameConversion": False,
        "apiRelinking": {
            "status": "GUEST_IMPORTS_STILL_DISPATCHED_BY_GENERATED_SHIMS",
            "staticGuestImportRewriteCount": 0,
            "note": (
                "This link contains the portable instruction translator and its state runtime. "
                "Mach-O API calls remain generated guest-shim dispatches; no per-IPA API "
                "callsite was statically rewritten to an Android API."
            ),
        },
        "note": (
            "A standalone Android shared object was linked and every generated translated-function "
            "symbol was verified. It is not an APK, is not linked into the GameBootActivity guest "
            "runtime, and is not evidence of a playable conversion."
        ),
    }


def link_generated_translation(
    output_dir: str | Path,
    target_abi: str = "arm64-v8a",
    *,
    environ: dict[str, str] | None = None,
    timeout: int = _LINK_TIMEOUT_SECONDS,
) -> dict:
    """Link generated portable C with Android NDK clang and verify the result."""
    destination = Path(output_dir)
    if target_abi == "auto":
        target_abi = "arm64-v8a"
    if target_abi not in _SUPPORTED_ABIS:
        return _link_failure(
            "BLOCKED_UNSUPPORTED_ANDROID_ABI",
            target_abi,
            "whole-game Android linking supports arm64-v8a and armeabi-v7a targets",
        )

    required = (destination / "game_all.c", destination / "rt_gen.c", destination / "rt_gen.h")
    missing_files = [path.name for path in required if not path.is_file()]
    runtime_root = Path(__file__).with_name("rt")
    runtime_source = runtime_root / "rt_core.c"
    entry_source = runtime_root / "rt_entry.c"
    android_jni_source = runtime_root / "rt_android_jni.c"
    filesystem_source = runtime_root / "rt_fs.c"
    filesystem_header = runtime_root / "rt_fs.h"
    entry_header = runtime_root / "rt_entry.h"
    missing_files.extend(
        str(path.relative_to(runtime_root.parent))
        for path in (entry_source, android_jni_source, filesystem_source, filesystem_header, entry_header)
        if not path.is_file()
    )
    report_path = destination / "rt_report.json"
    if not runtime_source.is_file():
        missing_files.append("radek/game/rt/rt_core.c")
    if not report_path.is_file():
        missing_files.append("rt_report.json")
    if missing_files:
        return _link_failure(
            "BLOCKED_TRANSLATION_ARTIFACTS_MISSING",
            target_abi,
            "required portable-C inputs are missing: " + ", ".join(missing_files),
        )

    clang, sysroot, reason = find_android_clang(environ)
    if clang is None or sysroot is None:
        failure = _link_failure("BLOCKED_NO_ANDROID_NDK", target_abi, reason)
        failure["ndkLinkVerified"] = False
        return failure

    try:
        translation_report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return _link_failure(
            "BLOCKED_TRANSLATION_REPORT_INVALID",
            target_abi,
            f"portable-C translation report cannot be read: {exc}",
        )

    triple, api_level = _SUPPORTED_ABIS[target_abi]
    target = f"{triple}{api_level}"
    link_dir = destination / "android-link" / target_abi
    link_dir.mkdir(parents=True, exist_ok=True)
    artifact = link_dir / "libtranslated_game.so"
    temporary_artifact = link_dir / "libtranslated_game.so.tmp"
    if temporary_artifact.exists():
        temporary_artifact.unlink()
    command = [
        str(clang),
        f"--target={target}",
        f"--sysroot={sysroot}",
        "-std=c11",
        "-O2",
        "-fPIC",
        "-fno-strict-aliasing",
        "-fvisibility=default",
        "-shared",
        "-Wl,--no-undefined",
        "-Wl,--build-id=none",
        "-Wl,-z,max-page-size=16384",
        "-Wl,-soname,libtranslated_game.so",
        "-I",
        str(destination),
        "-I",
        str(runtime_source.parent),
        str(destination / "game_all.c"),
        str(destination / "rt_gen.c"),
        str(runtime_source),
        str(filesystem_source),
        str(entry_source),
        str(android_jni_source),
        "-lm",
        "-o",
        str(temporary_artifact),
    ]
    try:
        completed = subprocess.run(
            command,
            cwd=destination,
            env=None if environ is None else {**os.environ, **environ},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        if temporary_artifact.exists():
            temporary_artifact.unlink()
        return _link_failure(
            "BLOCKED_ANDROID_LINK_FAILED",
            target_abi,
            f"Android NDK clang could not link portable C: {exc}",
        )
    if completed.returncode != 0 or not temporary_artifact.is_file():
        if temporary_artifact.exists():
            temporary_artifact.unlink()
        failure = _link_failure(
            "BLOCKED_ANDROID_LINK_FAILED",
            target_abi,
            "Android NDK clang failed to link portable C",
        )
        failure["linkerExitCode"] = completed.returncode
        failure["linkerOutputTail"] = (completed.stdout or "")[-12000:]
        return failure

    try:
        elf_data = temporary_artifact.read_bytes()
        verified = verify_android_translation(
            elf_data,
            translation_report,
            target_abi,
            artifact_name=artifact.name,
        )
    except OSError as exc:
        verified = _link_failure(
            "BLOCKED_ANDROID_ELF_VALIDATION",
            target_abi,
            f"Android linker output could not be read: {exc}",
        )
    if verified["status"] != "VERIFIED_ANDROID_SHARED_LIBRARY":
        # Retain a rejected ELF as a diagnostic artifact, but never count it.
        rejected = link_dir / "libtranslated_game.unverified.so"
        temporary_artifact.replace(rejected)
        verified["unverifiedArtifact"] = str(rejected.relative_to(destination))
        verified["linkerOutputTail"] = (completed.stdout or "")[-12000:]
        return verified

    temporary_artifact.replace(artifact)
    verified["artifact"] = str(artifact.relative_to(destination))
    verified["commandTarget"] = target
    verified["linker"] = str(clang)
    verified["ndkLinkVerified"] = True
    verified["linkerOutputTail"] = (completed.stdout or "")[-4000:]
    return verified


def prepare_translated_game_runtime_input(
    output_dir: str | Path,
    target_abi: str,
    android_link_report: dict,
) -> dict:
    """Package the translated memory image and verified library for APK import.

    The returned ZIP is an input to ``GameRuntimeApkBuilder``; it is not itself
    an APK, and it does not change the standalone link's linkedIntoGame flag.
    """
    destination = Path(output_dir).resolve()
    failure = {
        "status": "BLOCKED_TRANSLATED_GAME_PACKAGE_INPUT",
        "readyForGameRuntimeApk": False,
        "linkedIntoGame": False,
        "apkProduced": False,
    }
    if target_abi not in _SUPPORTED_ABIS:
        return {**failure, "reason": "translated game package input uses an unsupported Android ABI"}
    required_flags = (
        android_link_report.get("status") == "VERIFIED_ANDROID_SHARED_LIBRARY",
        android_link_report.get("targetAbi") == target_abi,
        android_link_report.get("architectureVerified") is True,
        android_link_report.get("dependenciesVerified") is True,
        android_link_report.get("exportsVerified") is True,
        android_link_report.get("translationEntryPointVerified") is True,
        android_link_report.get("allVerificationsPassed") is True,
        android_link_report.get("ndkLinkVerified") is True,
    )
    if not all(required_flags):
        return {**failure, "reason": "Android architecture, dependency, export, entry-point, and link checks must pass first"}

    report_path = destination / "rt_report.json"
    memory_path = destination / "rt_mem.bin"
    if not report_path.is_file() or not memory_path.is_file():
        return {**failure, "reason": "rt_report.json and rt_mem.bin are required for the translated boot payload"}
    try:
        translation = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {**failure, "reason": f"portable-C translation report is invalid: {exc}"}
    source_hash = translation.get("sourceExecutableSha256")
    if not isinstance(source_hash, str) or len(source_hash) != 64 or any(c not in "0123456789abcdef" for c in source_hash):
        return {**failure, "reason": "portable-C translation report does not bind a source executable SHA-256"}
    if translation.get("translationEntryPointSymbol") != _TRANSLATED_GAME_ENTRY_SYMBOL:
        return {**failure, "reason": "portable-C report does not name the GameBootActivity translation entry point"}
    functions = translation.get("functions")
    symbols = translation.get("translatedFunctionSymbols")
    if (
        isinstance(functions, bool)
        or not isinstance(functions, int)
        or functions <= 0
        or translation.get("functionFailures") != 0
        or not isinstance(symbols, list)
        or len(symbols) != functions
        or any(not isinstance(symbol, str) or not symbol for symbol in symbols)
        or len(set(symbols)) != len(symbols)
    ):
        return {**failure, "reason": "portable-C translated-function manifest is incomplete or inconsistent"}
    if android_link_report.get("linkedTranslatedFunctionCount") != functions:
        return {**failure, "reason": "verified Android function count does not match the translation report"}

    artifact_value = android_link_report.get("artifact")
    if not isinstance(artifact_value, str) or not artifact_value:
        return {**failure, "reason": "verified Android shared-library path is missing"}
    artifact_path = (destination / artifact_value).resolve()
    try:
        artifact_path.relative_to(destination)
    except ValueError:
        return {**failure, "reason": "verified Android shared-library path escapes the translation directory"}
    if not artifact_path.is_file():
        return {**failure, "reason": "verified Android shared library is missing"}
    library_sha = _file_sha256(artifact_path)
    if library_sha != android_link_report.get("artifactSha256"):
        return {**failure, "reason": "Android shared-library hash no longer matches its verified link report"}
    if android_link_report.get("entryPointSymbol") != _TRANSLATED_GAME_ENTRY_SYMBOL:
        return {**failure, "reason": "verified Android link report has the wrong JNI entry point"}

    memory_sha = _file_sha256(memory_path)
    translation_report_sha = _file_sha256(report_path)
    manifest = {
        "contract": _TRANSLATED_GAME_PAYLOAD_CONTRACT,
        "schemaVersion": 1,
        "targetAbi": target_abi,
        "entryPointSymbol": _TRANSLATED_GAME_ENTRY_SYMBOL,
        "sourceExecutableSha256": source_hash,
        "translationReportSha256": translation_report_sha,
        "androidLibrarySha256": library_sha,
        "translatedFunctionCount": functions,
        "translatedUniqueTextBytes": translation.get(
            "translatedUniqueTextBytes", translation.get("translatedFunctionBytes", 0)
        ),
        "executableTextBytes": translation.get("executableTextBytes", 0),
        "memoryImage": {
            "name": "rt_mem.bin",
            "sizeBytes": memory_path.stat().st_size,
            "sha256": memory_sha,
        },
    }
    payload_path = destination / _TRANSLATED_GAME_PAYLOAD_NAME
    temporary_payload = destination / (_TRANSLATED_GAME_PAYLOAD_NAME + ".tmp")
    input_path = destination / "translated-game-runtime-input.zip"
    temporary_input = destination / (input_path.name + ".tmp")
    link_bytes = json.dumps(android_link_report, sort_keys=True, indent=2).encode("utf-8")
    bundle_manifest = {
        "contract": "translated-game-runtime-input-v1",
        "schemaVersion": 1,
        "targetAbi": target_abi,
        "sourceExecutableSha256": source_hash,
        "entryPointSymbol": _TRANSLATED_GAME_ENTRY_SYMBOL,
        "files": {
            "libtranslated_game.so": {
                "sizeBytes": artifact_path.stat().st_size,
                "sha256": library_sha,
            },
            _TRANSLATED_GAME_PAYLOAD_NAME: {
                "sizeBytes": 0,
                "sha256": "",
            },
            "android-link-report.json": {
                "sizeBytes": len(link_bytes),
                "sha256": hashlib.sha256(link_bytes).hexdigest(),
            },
            "rt_report.json": {
                "sizeBytes": report_path.stat().st_size,
                "sha256": translation_report_sha,
            },
        },
        "linkVerification": {
            "architectureVerified": True,
            "dependenciesVerified": True,
            "exportsVerified": True,
            "translationEntryPointVerified": True,
            "allVerificationsPassed": True,
            "status": "VERIFIED_ANDROID_SHARED_LIBRARY",
        },
    }
    try:
        with zipfile.ZipFile(temporary_payload, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            archive.writestr("manifest.json", json.dumps(manifest, sort_keys=True, separators=(",", ":")))
            archive.write(memory_path, "rt_mem.bin")
        payload_sha = _file_sha256(temporary_payload)
        bundle_manifest["files"][_TRANSLATED_GAME_PAYLOAD_NAME] = {
            "sizeBytes": temporary_payload.stat().st_size,
            "sha256": payload_sha,
        }
        with zipfile.ZipFile(temporary_input, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            archive.writestr("manifest.json", json.dumps(bundle_manifest, sort_keys=True, separators=(",", ":")))
            archive.write(artifact_path, "libtranslated_game.so")
            archive.write(temporary_payload, _TRANSLATED_GAME_PAYLOAD_NAME)
            archive.writestr("android-link-report.json", link_bytes)
            archive.write(report_path, "rt_report.json")
        # Read each produced archive to catch truncated central directories before
        # reporting a package-ready handoff.
        for candidate in (temporary_payload, temporary_input):
            with zipfile.ZipFile(candidate, "r") as archive:
                if archive.testzip() is not None:
                    raise OSError("translated game runtime ZIP failed its CRC check")
        os.replace(temporary_payload, payload_path)
        os.replace(temporary_input, input_path)
    except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
        temporary_payload.unlink(missing_ok=True)
        temporary_input.unlink(missing_ok=True)
        payload_path.unlink(missing_ok=True)
        input_path.unlink(missing_ok=True)
        return {**failure, "reason": f"translated game-runtime package input could not be written: {exc}"}

    return {
        "status": "READY_FOR_GAME_RUNTIME_APK",
        "readyForGameRuntimeApk": True,
        "contract": "translated-game-runtime-input-v1",
        "payloadContract": _TRANSLATED_GAME_PAYLOAD_CONTRACT,
        "targetAbi": target_abi,
        "entryPointSymbol": _TRANSLATED_GAME_ENTRY_SYMBOL,
        "sourceExecutableSha256": source_hash,
        "translatedGamePayload": payload_path.name,
        "translatedGamePayloadSha256": payload_sha,
        "translatedGamePayloadBytes": payload_path.stat().st_size,
        "runtimeInputBundle": input_path.name,
        "runtimeInputBundleSha256": _file_sha256(input_path),
        "runtimeInputBundleBytes": input_path.stat().st_size,
        "linkedIntoGame": False,
        "apkProduced": False,
        "gamePlayable": False,
        "note": (
            "Verified host input bundle for the game-runtime APK builder. This is not itself an APK, "
            "is not yet packaged by the device app, and does not provide an EGL renderer or gameplay evidence."
        ),
    }


__all__ = [
    "find_android_clang",
    "link_generated_translation",
    "prepare_translated_game_runtime_input",
    "verify_android_translation",
]
