"""Game-runtime boot-attempt contract (``game-runtime-v1``), shared by host and device.

The host never builds APKs. :func:`run_gameboot` probes the real guest boot
with the ``radek-gameboot`` host binary and writes the manifest that the
on-device ``GameRuntimeApkBuilder`` implements: one authorized 32-bit ARM
Mach-O slice plus the bundle, packed into a ``*-game.apk``. The host probe is
finite by design; the device launcher leaves guest execution unlimited so an
implemented render loop is not stopped by a diagnostic guard, and leaves the
diagnostic screen open only when a real runtime boundary is reached.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from .archive import InputError, Limits, discover_app, extract_ipa, read_plist, sha256

CONTRACT = "game-runtime-v1"
BACKEND_HOST = "radek-host-gameboot-v1"
BACKEND_DEVICE = "radek-device-gameruntime-v1"
ARTIFACT_SUFFIX = "-game.apk"
PACKAGE_PREFIX = "dev.radek.gameruntime.p"
BOOT_ACTIVITY = "dev.radek.gameruntime.GameBootActivity"
JNI_SYMBOL = "Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt"
ASSET_EXECUTABLE = "assets/gameboot/main-executable.bin"
ASSET_METADATA = "assets/gameboot.json"
TRAP_RANGE_START = 0xF0020000
TRAP_RANGE_END = 0xF0030000
MAX_EXECUTABLE_BYTES = 256 * 1024 * 1024
MAX_EXPANDED_BYTES = 1024 * 1024 * 1024
PROBE_TIMEOUT_SECONDS = 300

_CPU_TYPE_ARM = 12
_MH_MAGIC = 0xFEEDFACE
_MH_MAGIC_64 = 0xFEEDFACF
_FAT_MAGIC = 0xCAFEBABE
_FAT_MAGIC_64 = 0xCAFEBABF
_MAX_FAT_SLICES = 64


def game_apk_name(original_name: str) -> str:
    """Mirror ``ArtifactNames.gameApkFileName`` exactly (host/device parity)."""
    original = (original_name or "").strip() or "ConvertedIPA"
    basename = original.replace("\\", "/").rsplit("/", 1)[-1]
    if basename.lower().endswith(".ipa"):
        stem = basename[:-4]
    elif "." in basename:
        stem = basename.rsplit(".", 1)[0]
    else:
        stem = basename
    safe = "".join(ch if ch.isalnum() or ch in " ._-" else "_" for ch in stem)
    safe = safe.strip(" ._-")[:80].strip(" ._-") or "ConvertedIPA"
    return f"{safe}{ARTIFACT_SUFFIX}"


def game_package(source_sha256: str, certificate_sha256: str) -> str:
    """Package id used by the on-device builder and artifact contract."""
    if not re.fullmatch(r"[0-9a-f]{64}", source_sha256 or ""):
        raise InputError("source SHA-256 is invalid")
    if not re.fullmatch(r"[0-9a-f]{64}", certificate_sha256 or ""):
        raise InputError("signer certificate SHA-256 is invalid")
    package = f"{PACKAGE_PREFIX}{source_sha256[:20]}{certificate_sha256[:8]}"
    if len(package) > 127:  # pragma: no cover - fixed-width inputs cannot hit this
        raise InputError("game package id exceeds the Android limit")
    return package


def find_gameboot_binary() -> Path | None:
    """Locate the ``radek-gameboot`` host probe built by CMake."""
    override = os.environ.get("RADEK_GAMEBOOT")
    if override:
        candidate = Path(override)
        if candidate.is_file():
            return candidate
        return None
    repo_root = Path(__file__).resolve().parent.parent
    for candidate in (
        repo_root / ".local" / "native-cmake" / "radek-gameboot",
        repo_root / ".local" / "bin" / "radek-gameboot",
    ):
        if candidate.is_file():
            return candidate
    found = shutil.which("radek-gameboot")
    return Path(found) if found else None


def describe_macho(path: Path) -> dict:
    """Classify a thin or FAT Mach-O file without loading it whole."""
    with path.open("rb") as handle:
        header = handle.read(64 * 1024)
    if len(header) < 8:
        raise InputError("executable is too small to be a Mach-O image")
    magic = struct.unpack("<I", header[0:4])[0]
    if magic in (_MH_MAGIC, _MH_MAGIC_64):
        if len(header) < 28:
            raise InputError("thin Mach-O header is truncated")
        cpu_type, cpu_subtype = struct.unpack("<II", header[4:12])
        return {
            "format": "thin-macho32" if magic == _MH_MAGIC else "thin-macho64",
            "cpuType": cpu_type,
            "cpuSubtype": cpu_subtype,
            "sliceCount": 1,
            "selectedSlice": 0,
        }
    magic_be = struct.unpack(">I", header[0:4])[0]
    if magic_be not in (_FAT_MAGIC, _FAT_MAGIC_64):
        raise InputError(f"executable is not a Mach-O or FAT image (magic {magic:08x})")
    fat64 = magic_be == _FAT_MAGIC_64
    (slice_count,) = struct.unpack(">I", header[4:8])
    if not 1 <= slice_count <= _MAX_FAT_SLICES:
        raise InputError(f"FAT slice count {slice_count} is invalid")
    record = 32 if fat64 else 20
    if len(header) < 8 + slice_count * record:
        raise InputError("FAT header is truncated")
    for index in range(slice_count):
        base = 8 + index * record
        (cpu_type,) = struct.unpack(">I", header[base : base + 4])
        if cpu_type != _CPU_TYPE_ARM:
            continue
        (cpu_subtype,) = struct.unpack(">I", header[base + 4 : base + 8])
        if fat64:
            offset, size = struct.unpack(">QQ", header[base + 8 : base + 24])
        else:
            offset, size = struct.unpack(">II", header[base + 8 : base + 16])
        return {
            "format": "fat64" if fat64 else "fat32",
            "cpuType": _CPU_TYPE_ARM,
            "cpuSubtype": cpu_subtype,
            "sliceCount": slice_count,
            "selectedSlice": index,
            "sliceOffset": offset,
            "sliceSize": size,
        }
    raise InputError("FAT image has no 32-bit ARM slice for the boot attempt")


def stage_arm_slice(executable: Path, destination: Path) -> dict:
    """Stage the bytes the boot attempt runs: the whole thin file or one FAT slice."""
    info = describe_macho(executable)
    if info["format"].startswith("thin"):
        if info["cpuType"] != _CPU_TYPE_ARM:
            raise InputError(
                f"thin Mach-O cpu type {info['cpuType']} is outside the 32-bit ARM boot-attempt subset"
            )
        shutil.copyfile(executable, destination)
    else:
        offset, size = info["sliceOffset"], info["sliceSize"]
        file_size = executable.stat().st_size
        if not 1 <= size <= MAX_EXECUTABLE_BYTES or offset < 0 or offset + size > file_size:
            raise InputError("FAT ARM slice range is invalid")
        with executable.open("rb") as src, destination.open("wb") as dst:
            src.seek(offset)
            remaining = size
            while remaining:
                chunk = src.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise InputError("FAT ARM slice is truncated")
                dst.write(chunk)
                remaining -= len(chunk)
        if destination.stat().st_size != size:
            raise InputError("FAT ARM slice could not be staged")
    staged = {key: info[key] for key in ("format", "cpuType", "cpuSubtype", "sliceCount", "selectedSlice")}
    staged["bytes"] = destination.stat().st_size
    staged["sha256"] = sha256(destination)
    return staged


def probe_boot(
    executable: Path,
    gameboot_binary: Path | None = None,
    timeout: int = PROBE_TIMEOUT_SECONDS,
    payload_dir: Path | None = None,
) -> dict:
    """Run the host boot probe and summarize its JSON report (never raises).

    ``payload_dir`` is the extracted .app bundle the guest's own file reads
    are served from. Probing without it starves the guest's startup resource
    loads, so the boot attempt is always probed with its bundle attached -
    exactly like the installed game APK.
    """
    binary = gameboot_binary or find_gameboot_binary()
    if binary is None or not binary.is_file():
        return {"status": "NOT_PROBED", "reason": "radek-gameboot host binary is not built"}
    argv = [str(binary), str(executable)]
    if payload_dir is not None:
        argv.append(str(payload_dir))
    argv.append("--diagnostic-probe")
    try:
        completed = subprocess.run(
            # The host probe is intentionally a finite diagnostic run. The
            # Android JNI path does not pass this flag and therefore keeps the
            # guest execution policy unlimited for gameplay.
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {"status": "NOT_PROBED", "reason": f"radek-gameboot timed out after {timeout}s"}
    except OSError as exc:
        return {"status": "NOT_PROBED", "reason": f"radek-gameboot could not start: {exc}"}
    # The report is the probe's only JSON document, but the guest's own console
    # output shares stdout with it once the bundle is attached. Parse the last
    # line that is a complete JSON object instead of trusting the whole stream.
    report = None
    try:
        for line in reversed(completed.stdout.decode("utf-8").splitlines()):
            stripped = line.strip()
            if not stripped.startswith("{"):
                continue
            try:
                report = json.loads(stripped)
                break
            except json.JSONDecodeError:
                continue
    except UnicodeDecodeError:
        report = None
    if not isinstance(report, dict):
        detail = completed.stderr.decode("utf-8", "replace").strip()
        return {"status": "NOT_PROBED", "reason": f"radek-gameboot report was not JSON: {detail[:300]}"}
    loader = report.get("loader") or {}
    execution = report.get("execution") or {}
    darwin_compat = report.get("darwinCompat") or {}
    gles = report.get("gles") or {}
    execution_policy = report.get("executionPolicy") or {}
    return {
        "status": "PROBED",
        "exitCode": completed.returncode,
        "loaderStatus": loader.get("status"),
        "resolvedSymbols": loader.get("resolvedSymbolCount", 0),
        "trappedSymbols": loader.get("trappedSymbolCount", 0),
        "unresolvedSymbols": loader.get("unresolvedSymbolCount", 0),
        "executionStatus": execution.get("status"),
        "entryPointReached": execution.get("entryPointReached", False),
        "instructions": execution.get("instructions", 0),
        "executionPolicy": execution_policy,
        "trapCalls": report.get("trapCalls", 0),
        "trappedImport": report.get("trappedImport"),
        "reason": report.get("reason", ""),
        # Translation-layer visibility: the Darwin-only adapters are registered
        # even though none of their names is an Android export.
        "darwinCompatBoundSymbols": darwin_compat.get("boundSymbols", 0),
        "darwinCompatStreamCells": darwin_compat.get("streamCells", 0),
        "darwinCompatOpenalCalls": darwin_compat.get("openalCalls", 0),
        "darwinCompatPersonalityBoundaries": darwin_compat.get("personalityBoundaries", 0),
        "glesDriverLoaded": bool(
            gles.get("driverEglLibraryLoaded") or gles.get("driverGlesLibraryLoaded")
        ),
        "report": report,
    }


def run_gameboot(
    ipa: Path,
    authorized: bool,
    output: Path,
    run_probe: bool = True,
    gameboot_binary: Path | None = None,
) -> dict:
    """Probe one IPA boot attempt and write the game-runtime manifest.

    ``output`` must be a new directory. Returns the manifest dict; the same
    dict is written to ``output/game-runtime-manifest.json`` alongside the
    staged slice (``main-executable.bin``) and, when probed, the full native
    report (``gameboot-report.json``).
    """
    if not authorized:
        raise InputError("authorization confirmation is required; protected binaries are never decrypted")
    ipa = ipa.resolve(strict=True)
    if ipa.suffix.lower() != ".ipa":
        raise InputError("input must have .ipa extension")
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    try:
        with tempfile.TemporaryDirectory(prefix="gameboot-", dir=output) as temporary:
            work = Path(temporary)
            extract_ipa(ipa, work / "ipa", Limits())
            app_dir = discover_app(work / "ipa")
            info = read_plist(app_dir / "Info.plist")
            executable = app_dir / info["CFBundleExecutable"]
            if not executable.is_file() or executable.stat().st_size > MAX_EXECUTABLE_BYTES:
                raise InputError("bundle executable is missing or exceeds the 256 MiB limit")
            staged_path = work / "main-executable.bin"
            staged = stage_arm_slice(executable, staged_path)
            bundle_files = 0
            bundle_bytes = 0
            for member in sorted(app_dir.rglob("*")):
                if member.is_file() and member != executable:
                    bundle_files += 1
                    bundle_bytes += member.stat().st_size
            probe: dict
            if run_probe:
                probe = probe_boot(staged_path, gameboot_binary, payload_dir=app_dir)
            else:
                probe = {"status": "NOT_PROBED", "reason": "probe skipped by caller"}
            shutil.copyfile(staged_path, output / "main-executable.bin")
            full_report = probe.pop("report", None)
            if full_report is not None:
                (output / "gameboot-report.json").write_text(json.dumps(full_report, indent=2))
        manifest = {
            "schemaVersion": 1,
            "contract": CONTRACT,
            "backendHost": BACKEND_HOST,
            "backendDevice": BACKEND_DEVICE,
            "authorizationConfirmed": True,
            "artifact": {
                "name": game_apk_name(ipa.name),
                "suffix": ARTIFACT_SUFFIX,
                "bootActivity": BOOT_ACTIVITY,
                "jniSymbol": JNI_SYMBOL,
                "assetExecutable": ASSET_EXECUTABLE,
                "assetMetadata": ASSET_METADATA,
            },
            "source": {
                "originalName": ipa.name,
                "sha256": sha256(ipa),
                "bytes": ipa.stat().st_size,
                "bundleId": info["CFBundleIdentifier"],
                "applicationName": info.get("CFBundleDisplayName")
                or info.get("CFBundleName")
                or info["CFBundleExecutable"],
                "executableEntry": f"Payload/{app_dir.name}/{info['CFBundleExecutable']}",
            },
            "executable": staged,
            "bundle": {"files": bundle_files, "bytes": bundle_bytes},
            "hostProbe": probe,
        }
        (output / "game-runtime-manifest.json").write_text(json.dumps(manifest, indent=2))
        return manifest
    except BaseException:
        shutil.rmtree(output, ignore_errors=True)
        raise


def manifest_summary(manifest: dict) -> dict:
    """Small JSON-safe summary printed by the CLI."""
    probe = manifest.get("hostProbe") or {}
    return {
        "contract": manifest.get("contract"),
        "artifact": (manifest.get("artifact") or {}).get("name"),
        "probe": probe.get("status"),
        "instructions": probe.get("instructions", 0),
        "trappedImport": probe.get("trappedImport"),
        "reason": (probe.get("reason") or "")[:300],
    }
