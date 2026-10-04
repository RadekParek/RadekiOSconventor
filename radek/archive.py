"""Untrusted IPA import. No executable is ever launched during inspection."""

from __future__ import annotations
import hashlib
import os
import plistlib
import shutil
import stat
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


class InputError(ValueError):
    pass


@dataclass(frozen=True)
class Limits:
    """Bounds for reading untrusted archives.

    There is deliberately no ``archive_bytes`` limit any more: 512 MiB was an
    arbitrary number that stopped real games from ever being analyzed. An
    archive is bounded by the free space of the device it is read on (see
    :func:`require_free_space`) plus the per-member guards below, which exist to
    stop ZIP bombs rather than to police how large a game may be.
    """

    expanded_bytes: int = 1024 * 1024 * 1024
    file_bytes: int = 256 * 1024 * 1024
    entries: int = 20000
    ratio: int = 250


def require_free_space(path: Path, needed_bytes: int) -> None:
    """Raise :class:`InputError` unless ``path`` has room for ``needed_bytes``.

    The only archive-level bound that remains is the device's own storage, so a
    game is rejected for being too big for this machine rather than for
    exceeding a number picked in advance.
    """
    probe = path if path.is_dir() else path.parent
    while not probe.is_dir() and probe.parent != probe:
        probe = probe.parent
    try:
        free = shutil.disk_usage(probe).free
    except OSError:  # pragma: no cover - platform without statvfs
        return
    if free < needed_bytes + 64 * 1024 * 1024:
        raise InputError(
            f"not enough free storage: {needed_bytes // (1024 * 1024)} MiB needed, "
            f"{free // (1024 * 1024)} MiB free"
        )


def safe_name(name: str) -> PurePosixPath:
    if not name or "\\" in name or "\x00" in name or ":" in name:
        raise InputError("invalid archive path")
    parts = name.rstrip("/").split("/")
    if name.startswith("/") or any(p in ("", ".", "..") for p in parts):
        raise InputError("unsafe archive path: " + name)
    if len(name.encode("utf-8")) > 1024 or len(parts) > 32:
        raise InputError("archive path exceeds limit")
    return PurePosixPath(*parts)


def extract_ipa(source: Path, destination: Path, limits: Limits = Limits()) -> None:
    """Destination must not exist. Remove all partial output on any failure."""
    require_free_space(destination, source.stat().st_size)
    destination.mkdir(mode=0o700, parents=False, exist_ok=False)
    try:
        with zipfile.ZipFile(source) as archive:
            infos = archive.infolist()
            if len(infos) > limits.entries:
                raise InputError("too many ZIP entries")
            seen: set[str] = set()
            total = 0
            for info in infos:
                rel = safe_name(info.filename)
                folded = str(rel).casefold()
                if folded in seen:
                    raise InputError("duplicate/case-colliding ZIP entry")
                seen.add(folded)
                mode = (info.external_attr >> 16) & 0xFFFF
                kind = stat.S_IFMT(mode)
                if kind not in (0, stat.S_IFREG, stat.S_IFDIR):
                    raise InputError("links and special files are prohibited")
                if info.flag_bits & 1:
                    raise InputError("encrypted ZIP is prohibited")
                if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    raise InputError("unsupported ZIP compression")
                if info.file_size > limits.file_bytes:
                    raise InputError("ZIP member too large")
                if info.file_size > max(1, info.compress_size) * limits.ratio:
                    raise InputError("ZIP expansion ratio exceeds limit")
                total += info.file_size
                if total > limits.expanded_bytes:
                    raise InputError("ZIP expanded size exceeds limit")
                target = destination.joinpath(*rel.parts)
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                written = 0
                with archive.open(info) as src, target.open("xb") as dst:
                    while chunk := src.read(64 * 1024):
                        written += len(chunk)
                        if written > info.file_size or written > limits.file_bytes:
                            raise InputError("ZIP actual size exceeds advertised size")
                        dst.write(chunk)
                if written != info.file_size:
                    raise InputError("truncated ZIP entry")
                target.chmod(0o600)
    except BaseException:
        shutil.rmtree(destination)
        raise


def discover_app(root: Path) -> Path:
    payload = root / "Payload"
    apps = sorted(p for p in payload.glob("*.app") if p.is_dir())
    if len(apps) != 1:
        raise InputError(f"expected exactly one top-level Payload/*.app, got {len(apps)}")
    return apps[0]


def read_plist(path: Path) -> dict:
    if path.stat().st_size > 8 * 1024 * 1024:
        raise InputError("plist exceeds 8 MiB")
    try:
        value = plistlib.loads(path.read_bytes())
    except Exception as exc:
        raise InputError(f"invalid XML/binary plist: {exc}") from exc
    if not isinstance(value, dict):
        raise InputError("Info.plist root must be a dictionary")
    for key in ("CFBundleExecutable", "CFBundleIdentifier"):
        if not isinstance(value.get(key), str) or not value[key]:
            raise InputError("missing " + key)
    for key in ("CFBundleName", "CFBundleDisplayName", "CFBundleVersion", "CFBundleShortVersionString"):
        if key in value and not isinstance(value[key], str):
            raise InputError("invalid non-string bundle field: " + key)
    executable = safe_name(value["CFBundleExecutable"])
    if len(executable.parts) != 1:
        raise InputError("bundle executable must be a filename")
    return value


def icon_candidates(info: dict, app: Path) -> list[Path]:
    names = []
    for key in ("CFBundleIcons", "CFBundleIcons~ipad"):
        icons = info.get(key, {})
        if isinstance(icons, dict):
            primary = icons.get("CFBundlePrimaryIcon", {})
            if isinstance(primary, dict):
                names.extend(primary.get("CFBundleIconFiles", []))
    names.extend(info.get("CFBundleIconFiles", []))
    if isinstance(info.get("CFBundleIconFile"), str):
        names.append(info["CFBundleIconFile"])
    candidates = []
    for name in names:
        if not isinstance(name, str):
            continue
        rel = safe_name(name)
        for suffix in ("", ".png", "@3x.png", "@2x.png", "~ipad.png", "@2x~ipad.png"):
            p = app / (str(rel) + suffix)
            if p.is_file():
                candidates.append(p)
    if not candidates:
        candidates = list(app.glob("*Icon*.png"))
    return sorted(set(candidates), key=lambda p: p.stat().st_size, reverse=True)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def metadata(info: dict, source: Path) -> dict:
    return {
        "name": info.get("CFBundleDisplayName") or info.get("CFBundleName") or info["CFBundleExecutable"],
        "bundleId": info["CFBundleIdentifier"],
        "version": info.get("CFBundleShortVersionString", ""),
        "build": info.get("CFBundleVersion", ""),
        "executable": info["CFBundleExecutable"],
        "originalName": source.name,
        "minimumIOSVersion": (
            info.get("MinimumOSVersion", "")
            if isinstance(info.get("MinimumOSVersion", ""), str)
            else ""
        ),
        "fileSize": source.stat().st_size,
        "sha256": sha256(source),
    }
