"""APK validation utilities; game-APK packaging stays disabled without a full static recompilation backend.

A separate, honestly labelled *experimental shell* APK can be built from the
isolated statically recompiled artifacts and the generated compatibility-registry source.
It carries the ``experimental-shell-v1`` contract, states on screen and in
metadata that no game code is statically recompiled, and can never satisfy the
``complete-game-v1`` attachment checks.
"""

from __future__ import annotations
import hashlib
import json
import os
import re
import secrets
import subprocess
import zipfile
from dataclasses import dataclass
from pathlib import Path
from .archive import InputError, safe_name
from .dex import classes as dex_classes

BUILD_TOOLS = "35.0.0"

EXPERIMENTAL_SHELL_CONTRACT = "experimental-shell-v1"
EXPERIMENTAL_SHELL_PACKAGE = "dev.radek.experimental.shell"
EXPERIMENTAL_SHELL_NOTICE = (
    "This inspection shell contains isolated analysis artifacts, not a runnable game."
)


def artifact_filename(source_name: str | Path) -> str:
    """Return a safe `<IPA stem>.apk` output name derived from the input filename."""
    name = str(source_name).replace("\\", "/").rsplit("/", 1)[-1]
    if name.lower().endswith(".ipa"):
        stem = name[:-4]
    else:
        stem = name.rsplit(".", 1)[0] if "." in name else name
    safe = "".join(ch if ch.isalnum() or ch in " ._-" else "_" for ch in stem)
    safe = safe.strip(" ._-")[:80].strip(" ._-") or "ConvertedIPA"
    return f"{safe}.apk"


def run(args: list[str | Path], log=None, timeout=180) -> str:
    argv = [str(a) for a in args]
    if log:
        log("tool", " ".join(argv))
    result = subprocess.run(
        argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout
    )
    if log and result.stdout:
        log("tool-output", result.stdout.rstrip())
    if result.returncode:
        raise RuntimeError(f"{Path(argv[0]).name} failed ({result.returncode}):\n{result.stdout}")
    return result.stdout


@dataclass
class Toolchain:
    sdk: Path
    build: Path

    @classmethod
    def discover(cls):
        home = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
        if not home:
            raise RuntimeError("ANDROID_SDK_ROOT or ANDROID_HOME is required; see docs/BUILD.md")
        sdk = Path(home)
        build = sdk / "build-tools" / BUILD_TOOLS
        for name in ("aapt2", "zipalign", "apksigner"):
            path = build / name
            if not path.is_file():
                raise RuntimeError("missing toolchain file: " + str(path))
        return cls(sdk, build)

    def tool(self, name):
        return self.build / name


def build_apk(
    work: Path,
    output: Path,
    machine_code: bytes,
    metadata: dict,
    icon: bytes | None,
    assets: Path,
    report: dict,
    tools: Toolchain,
    key: Path,
    log=None,
    target_abi: str | None = None,
) -> dict:
    """Refuse the former integer-entry wrapper, which was not a game conversion."""
    raise InputError(
        "APK packaging is disabled: no complete iOS-to-Android game static recompilation backend and API replacement backend is implemented"
    )


def elf_info(data: bytes) -> dict:
    """Inspect supported Android ARM32/ARM64 ELF libraries."""
    from .elf import inspect

    return inspect(data)


# --- Honestly labelled experimental shell APK -------------------------------
# The shell packages the isolated statically recompiled artifacts and the generated
# compatibility-registry source so they can be inspected on-device. It states
# what it is in its launcher text and metadata, and it is explicitly not a
# game conversion.

_EXPERIMENTAL_MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{package}"
    android:versionCode="1"
    android:versionName="0.1-experimental">
    <application android:label="@string/app_name" android:allowBackup="false">
        <activity android:name=".ExperimentalShellActivity" android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
"""

_EXPERIMENTAL_STRINGS = """<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">{app_name}</string>
    <string name="shell_notice">{notice}</string>
</resources>
"""


def _android_string_escape(value: str) -> str:
    """Escape a value for an Android <string> resource (aapt2 rules)."""
    return value.replace("\\", "\\\\").replace("'", "\\'").replace('"', '\\"')

_EXPERIMENTAL_ACTIVITY = """package dev.radek.experimental.shell;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

/** Shows the honest status of this package: artifacts only, no game. */
public final class ExperimentalShellActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setPadding(48, 96, 48, 48);
        text.setTextSize(16f);
        text.setText(getString(R.string.shell_notice));
        setContentView(text);
    }
}
"""


def experimental_shell_sources(work: Path) -> Path:
    """Write the manifest, resources and launcher source for the shell APK."""
    root = work / "experimental-shell-src"
    (root / "res" / "values").mkdir(parents=True, exist_ok=True)
    (root / "src").mkdir(parents=True, exist_ok=True)
    (root / "AndroidManifest.xml").write_text(
        _EXPERIMENTAL_MANIFEST.format(package=EXPERIMENTAL_SHELL_PACKAGE), encoding="utf-8"
    )
    (root / "res" / "values" / "strings.xml").write_text(
        _EXPERIMENTAL_STRINGS.format(
            app_name=_android_string_escape("Radek Experimental Shell"),
            notice=_android_string_escape(EXPERIMENTAL_SHELL_NOTICE),
        ),
        encoding="utf-8",
    )
    (root / "src" / "ExperimentalShellActivity.java").write_text(
        _EXPERIMENTAL_ACTIVITY, encoding="utf-8"
    )
    return root


def _android_jar(tools: Toolchain) -> Path:
    platforms = tools.sdk / "platforms"
    candidates = sorted(platforms.glob("android-*/android.jar")) if platforms.is_dir() else []
    if not candidates:
        raise RuntimeError("no Android platform android.jar found under " + str(platforms))
    return candidates[-1]


def _generate_debug_keystore(work: Path, log=None) -> tuple[Path, str]:
    keystore = work / "experimental-shell.keystore"
    password = secrets.token_hex(16)
    run(
        [
            "keytool",
            "-genkeypair",
            "-keystore",
            keystore,
            "-storetype",
            "PKCS12",
            "-alias",
            "radek-experimental",
            "-keyalg",
            "RSA",
            "-keysize",
            "2048",
            "-validity",
            "10000",
            "-storepass",
            password,
            "-keypass",
            password,
            "-dname",
            "CN=Radek Experimental Shell (local debug identity)",
        ],
        log=log,
    )
    return keystore, password


def build_experimental_shell(
    work: Path,
    output: Path,
    tools: Toolchain,
    provenance: dict,
    artifacts: dict[str, tuple[str, bytes]] | None = None,
    log=None,
) -> dict:
    """Assemble, align and sign the honest experimental shell APK.

    ``artifacts`` maps an APK entry name to ``(kind, payload)``: statically recompiled
    code or compatibility-registry sources produced by this run. Every step
    uses the real Android toolchain (aapt2, javac, d8, zipalign, apksigner).
    """
    sources = experimental_shell_sources(work)
    android_jar = _android_jar(tools)
    build = work / "experimental-shell-build"
    build.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)

    compiled = build / "compiled-res.zip"
    run([tools.tool("aapt2"), "compile", "--dir", sources / "res", "-o", compiled], log=log)
    base_apk = build / "base.apk"
    gen = build / "gen"
    run(
        [
            tools.tool("aapt2"),
            "link",
            "-o",
            base_apk,
            "-I",
            android_jar,
            "--manifest",
            sources / "AndroidManifest.xml",
            "--java",
            gen,
            "--min-sdk-version",
            "21",
            "--target-sdk-version",
            "35",
            compiled,
        ],
        log=log,
    )

    classes = build / "classes"
    classes.mkdir(parents=True, exist_ok=True)
    java_sources = [str(sources / "src" / "ExperimentalShellActivity.java")]
    java_sources += [str(path) for path in sorted(gen.rglob("R.java"))]
    run(
        [
            "javac",
            "-source",
            "17",
            "-target",
            "17",
            "-nowarn",
            "-classpath",
            android_jar,
            "-d",
            classes,
            *java_sources,
        ],
        log=log,
    )
    dex_dir = build / "dex"
    dex_dir.mkdir(parents=True, exist_ok=True)
    run(
        [
            tools.tool("d8"),
            "--min-api",
            "21",
            "--lib",
            android_jar,
            "--output",
            dex_dir,
            *[str(path) for path in sorted(classes.rglob("*.class"))],
        ],
        log=log,
    )
    dex = dex_dir / "classes.dex"
    if not dex.is_file():
        raise RuntimeError("d8 produced no classes.dex")

    metadata = {
        "contract": EXPERIMENTAL_SHELL_CONTRACT,
        "generator": "RadekiOSConventor",
        "honestLabeling": True,
        "containsGameCode": False,
        "completeGameConversion": False,
        "disclosure": EXPERIMENTAL_SHELL_NOTICE,
        "provenance": provenance,
        "artifacts": sorted((artifacts or {}).keys()),
    }
    aligned = build / "aligned.apk"
    with zipfile.ZipFile(base_apk, "a", compression=zipfile.ZIP_STORED) as package:
        package.writestr("classes.dex", dex.read_bytes())
        package.writestr(
            "assets/conversion-metadata.json", json.dumps(metadata, indent=2, ensure_ascii=True)
        )
        for entry, (kind, payload) in sorted((artifacts or {}).items()):
            package.writestr(f"assets/{kind}/{entry}", payload)
    run([tools.tool("zipalign"), "-f", "4", base_apk, aligned], log=log)

    keystore, password = _generate_debug_keystore(build, log=log)
    final = output / "experimental-shell.apk"
    run(
        [
            tools.tool("apksigner"),
            "sign",
            "--ks",
            keystore,
            "--ks-pass",
            "pass:" + password,
            "--key-pass",
            "pass:" + password,
            "--out",
            final,
            aligned,
        ],
        log=log,
    )
    validation = validate_experimental_shell(final, tools)
    if validation.get("status") != "VALID":
        raise RuntimeError("experimental shell failed its own validation: " + validation.get("reason", ""))
    return {
        "contract": EXPERIMENTAL_SHELL_CONTRACT,
        "status": "BUILT_NOT_A_GAME",
        "path": final.name,
        "sha256": hashlib.sha256(final.read_bytes()).hexdigest(),
        "sizeBytes": final.stat().st_size,
        "package": EXPERIMENTAL_SHELL_PACKAGE,
        "signedWith": "local debug keystore generated for this run",
        "artifacts": sorted((artifacts or {}).keys()),
        "validation": validation,
        "completeGameConversion": False,
        "message": (
            "An explicitly labelled experimental shell APK was aligned and signed. It packages the "
            "isolated statically recompiled artifacts and compatibility-registry source for inspection only; "
            "it contains no statically recompiled game and is not a complete-game conversion."
        ),
    }


def validate_experimental_shell(apk: Path, tools: Toolchain | None = None) -> dict:
    """Statically validate the honest experimental-shell contract."""
    if not apk.is_file():
        return {"status": "INVALID", "reason": "APK file is missing"}
    try:
        package = zipfile.ZipFile(apk)
    except zipfile.BadZipFile:
        return {"status": "INVALID", "reason": "not a valid zip archive"}
    with package:
        names = set(package.namelist())
        if "classes.dex" not in names:
            return {"status": "INVALID", "reason": "missing classes.dex launcher bytecode"}
        if "AndroidManifest.xml" not in names:
            return {"status": "INVALID", "reason": "missing AndroidManifest.xml"}
        if "assets/conversion-metadata.json" not in names:
            return {"status": "INVALID", "reason": "missing conversion metadata"}
        try:
            metadata = json.loads(package.read("assets/conversion-metadata.json"))
        except (ValueError, UnicodeDecodeError):
            return {"status": "INVALID", "reason": "conversion metadata is not valid JSON"}
        if metadata.get("contract") != EXPERIMENTAL_SHELL_CONTRACT:
            return {
                "status": "INVALID",
                "reason": "metadata does not declare the experimental-shell-v1 contract",
            }
        if metadata.get("honestLabeling") is not True or metadata.get("containsGameCode"):
            return {"status": "INVALID", "reason": "metadata lacks the honest-labelling disclosure"}
        if EXPERIMENTAL_SHELL_NOTICE not in metadata.get("disclosure", ""):
            return {"status": "INVALID", "reason": "metadata disclosure text is missing"}
        try:
            dex_classes(package.read("classes.dex"))
        except Exception as exc:  # noqa: BLE001 - any malformed DEX fails validation closed
            return {"status": "INVALID", "reason": f"classes.dex failed integrity checks: {exc}"}
    result = {"status": "VALID", "contract": EXPERIMENTAL_SHELL_CONTRACT, "signatureVerified": False}
    if tools is not None:
        try:
            run([tools.tool("apksigner"), "verify", apk])
            result["signatureVerified"] = True
        except RuntimeError as exc:
            return {"status": "INVALID", "reason": f"apksigner rejected the signature: {exc}"}
    return result

def _validate_complete_game_metadata(metadata: dict, expected_package: str, target_abi: str) -> dict:
    """Fail closed unless the APK declares full reachable-code/API/resource coverage."""
    if not isinstance(metadata, dict):
        raise InputError("complete-game conversion provenance must be a JSON object")
    if metadata.get("contract") != "complete-game-v1":
        raise InputError("APK is not a complete-game conversion; restricted native-entry packages are rejected")
    source = metadata.get("source") or {}
    if not isinstance(source, dict):
        raise InputError("complete-game source provenance is invalid")
    source_hash = source.get("sha256", "")
    if not isinstance(source_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", source_hash):
        raise InputError("complete-game source hash is missing or invalid")
    if expected_package != "dev.radek.converted.p" + source_hash[:20] or metadata.get("package") != expected_package:
        raise InputError("complete-game APK package/source identity mismatch")
    if metadata.get("targetAbi") != target_abi:
        raise InputError("complete-game target ABI mismatch")

    conversion = metadata.get("conversion") or {}
    if not isinstance(conversion, dict):
        raise InputError("complete-game conversion details are invalid")
    output_bytes = conversion.get("outputBytes")
    if not isinstance(output_bytes, int) or isinstance(output_bytes, bool) or not 0 < output_bytes <= 64 * 1024 * 1024:
        raise InputError("missing or invalid generated native-code size")
    backend = conversion.get("backend", "")
    if conversion.get("targetAbi") != target_abi or not isinstance(backend, str) or not backend.strip():
        raise InputError("missing or inconsistent complete-game conversion provenance")

    game = metadata.get("gameConversion") or {}
    if not isinstance(game, dict):
        raise InputError("complete-game evidence must be a JSON object")
    if game.get("status") != "COMPLETE" or game.get("completeGameConversion") is not True:
        raise InputError("host did not attest a complete game conversion")
    reachable = game.get("reachableSourceFunctions")
    recompiled_count = game.get("recompiledReachableFunctions")
    not_recompiled_count = game.get("notRecompiledReachableFunctions")
    if (
        not isinstance(reachable, int)
        or isinstance(reachable, bool)
        or reachable <= 0
        or not isinstance(recompiled_count, int)
        or isinstance(recompiled_count, bool)
        or recompiled_count != reachable
        or not isinstance(not_recompiled_count, int)
        or isinstance(not_recompiled_count, bool)
        or not_recompiled_count != 0
    ):
        raise InputError("not all reachable game functions were statically recompiled")

    reachable_apis = game.get("reachableApiCount")
    generated_replacements = game.get("generatedApiReplacements")
    native_passthroughs = game.get("nativeApiPassthroughs")
    unimplemented_apis = game.get("unimplementedReachableApiCount")
    replacements = game.get("apiReplacements")
    if (
        game.get("apiCoverageComplete") is not True
        or not isinstance(unimplemented_apis, int)
        or isinstance(unimplemented_apis, bool)
        or unimplemented_apis != 0
        or not isinstance(reachable_apis, int)
        or isinstance(reachable_apis, bool)
        or not isinstance(generated_replacements, int)
        or isinstance(generated_replacements, bool)
        or not isinstance(native_passthroughs, int)
        or isinstance(native_passthroughs, bool)
        or reachable_apis < 0
        or generated_replacements < 0
        or native_passthroughs < 0
        or reachable_apis != generated_replacements + native_passthroughs
        or not isinstance(replacements, list)
        or len(replacements) != generated_replacements
    ):
        raise InputError("reachable iOS API replacement accounting is incomplete")
    for replacement in replacements:
        if not isinstance(replacement, dict):
            raise InputError("API mapping is only a candidate; generated linked implementation evidence is missing")
        source_symbol = replacement.get("sourceSymbol")
        target_api = replacement.get("targetAndroidApi")
        implementation_hash = replacement.get("implementationSha256")
        implementation_artifact = replacement.get("implementationArtifact")
        if (
            replacement.get("codeGenerated") is not True
            or replacement.get("linkedIntoApk") is not True
            or replacement.get("reachableFromEntry") is not True
            or not isinstance(source_symbol, str)
            or not source_symbol.strip()
            or not isinstance(target_api, str)
            or not target_api.strip()
            or not isinstance(implementation_hash, str)
            or not re.fullmatch(r"[0-9a-f]{64}", implementation_hash)
            or not isinstance(implementation_artifact, str)
            or not implementation_artifact.strip()
        ):
            raise InputError("API mapping is only a candidate; generated linked implementation evidence is missing")
        try:
            safe_name(implementation_artifact)
        except (InputError, UnicodeError) as exc:
            raise InputError("generated API implementation path is unsafe") from exc

    if game.get("resourcesComplete") is not True:
        raise InputError("game resources are not declared complete")
    if game.get("lifecycleImplemented") is not True:
        raise InputError("Android application lifecycle is not declared implemented")
    icon_hash = game.get("sourceIconSha256", "")
    launcher_hash = game.get("launcherIconSha256", "")
    if (
        not isinstance(icon_hash, str)
        or not isinstance(launcher_hash, str)
        or (icon_hash and (not re.fullmatch(r"[0-9a-f]{64}", icon_hash) or launcher_hash != icon_hash))
        or (launcher_hash and not re.fullmatch(r"[0-9a-f]{64}", launcher_hash))
    ):
        raise InputError("recovered source icon was not preserved")
    return game


def _validate_packaged_payloads(z: zipfile.ZipFile, names: list[str], source_hash: str) -> None:
    """Reject source IPA bytes and Apple executables anywhere in the APK."""
    apple_executable_magics = {
        b"\xcf\xfa\xed\xfe",
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xfe\xed\xfa\xce",
        b"\xca\xfe\xba\xbe",
        b"\xca\xfe\xba\xbf",
        b"\xbe\xba\xfe\xca",
        b"\xbf\xba\xfe\xca",
    }
    for name in names:
        if name.lower().endswith(".ipa"):
            raise InputError("original IPA must not be included")
        with z.open(name) as entry:
            digest = hashlib.sha256()
            magic = entry.read(4)
            if magic in apple_executable_magics:
                raise InputError("Apple executable leaked into APK")
            digest.update(magic)
            for chunk in iter(lambda: entry.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() == source_hash:
            raise InputError("original IPA content must not be included in the APK")


def validate_apk(
    path: Path,
    tools: Toolchain,
    expected_package: str,
    expected_entry: str,
    converted: bool = True,
    log=None,
    expected_abi: str | None = None,
) -> dict:
    if not path.is_file() or not 0 < path.stat().st_size:
        raise InputError("APK is missing/empty")
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if (
            len(names) > 20000
            or sum(i.file_size for i in z.infolist()) > 1024 * 1024 * 1024
            or any(i.file_size > 256 * 1024 * 1024 for i in z.infolist())
        ):
            raise InputError("APK ZIP size/entry limit exceeded")
        if len(names) != len(set(names)):
            raise InputError("APK duplicate ZIP paths")
        for name in names:
            safe_name(name)
        if z.testzip():
            raise InputError("APK CRC failure")
        for required in ("AndroidManifest.xml", "resources.arsc", "classes.dex"):
            if required not in names:
                raise InputError("APK missing " + required)
        defined_classes = set()
        for name in names:
            if re.fullmatch(r"classes(?:[2-9]|[1-9][0-9]+)?\.dex", name):
                defined_classes.update(dex_classes(z.read(name)))
        if "L" + expected_entry.replace(".", "/") + ";" not in defined_classes:
            raise InputError("Android entry class is absent from DEX definitions")
        if z.read("AndroidManifest.xml")[:2] != b"\x03\x00":
            raise InputError("APK manifest is not compiled binary XML")
        conversion_metadata = None
        if converted:
            if "assets/conversion.json" not in names:
                raise InputError("complete-game conversion provenance is absent")
            try:
                conversion_metadata = json.loads(z.read("assets/conversion.json"))
            except (ValueError, UnicodeDecodeError) as exc:
                raise InputError("invalid complete-game conversion provenance") from exc
        if converted:
            if not isinstance(conversion_metadata, dict):
                raise InputError("complete-game conversion provenance must be a JSON object")
            conversion_details = conversion_metadata.get("conversion")
            if not isinstance(conversion_details, dict):
                raise InputError("complete-game target ABI provenance is missing")
            target_abi = conversion_details.get("targetAbi")
        else:
            target_abi = "arm64-v8a"
        if not isinstance(target_abi, str) or target_abi not in ("arm64-v8a", "armeabi-v7a"):
            raise InputError("unsupported target ABI in conversion provenance")
        if expected_abi is not None and expected_abi != target_abi:
            raise InputError("APK native ABI does not match the requested validation ABI")
        expected_library_prefix = f"lib/{target_abi}/"
        libraries = {
            name.rsplit("/", 1)[-1]: elf_info(z.read(name))
            for name in names
            if name.startswith(expected_library_prefix) and name.endswith(".so")
        }
        if not libraries:
            raise InputError(f"APK has no {target_abi} native libraries")
        if any(
            name.startswith("lib/") and not name.startswith(expected_library_prefix)
            for name in names
            if name.endswith(".so")
        ):
            raise InputError("unexpected native architecture")
        if any(info["architecture"] != target_abi for info in libraries.values()):
            raise InputError("native library ELF architecture does not match its APK ABI directory")
        android_system = {
            "libc.so",
            "libm.so",
            "libdl.so",
            "liblog.so",
            "libandroid.so",
            "libGLESv2.so",
            "libEGL.so",
            "libz.so",
        }
        for name, info in libraries.items():
            unresolved = set(info["needed"]) - libraries.keys() - android_system
            if unresolved:
                raise InputError(f"{name}: unresolved native dependencies {sorted(unresolved)}")
        if converted:
            if "libconverted.so" not in libraries:
                raise InputError("converted native library is absent")
            metadata = conversion_metadata
            game = _validate_complete_game_metadata(metadata, expected_package, target_abi)
            conversion = metadata["conversion"]
            native = libraries["libconverted.so"]
            if native["architecture"] != target_abi:
                raise InputError("native ELF architecture does not match APK ABI path")
            entry = native["exports"].get("Java_dev_radek_generated_MainActivity_runNative")
            if (
                entry is None
                or entry["type"] != 2
                or entry["size"] != conversion["outputBytes"]
                or entry.get("sha256") != conversion.get("machineCodeSha256")
            ):
                raise InputError("native JNI entry/code does not match conversion provenance")
            for replacement in game["apiReplacements"]:
                artifact = str(safe_name(replacement["implementationArtifact"]))
                if artifact not in names:
                    raise InputError("generated API implementation is not packaged: " + artifact)
                if hashlib.sha256(z.read(artifact)).hexdigest() != replacement["implementationSha256"]:
                    raise InputError("generated API implementation hash mismatch: " + artifact)
            resource_inventory = metadata.get("resourceInventory", [])
            if not isinstance(resource_inventory, list):
                raise InputError("game resource inventory is invalid")
            for resource in resource_inventory:
                if not isinstance(resource, dict) or not isinstance(resource.get("path"), str):
                    raise InputError("game resource inventory entry is invalid")
                resource_path = str(safe_name(resource["path"]))
                name = "assets/bundle/" + resource_path
                resource_hash = resource.get("sha256")
                if (
                    name not in names
                    or not isinstance(resource_hash, str)
                    or not re.fullmatch(r"[0-9a-f]{64}", resource_hash)
                    or hashlib.sha256(z.read(name)).hexdigest() != resource_hash
                ):
                    raise InputError("resource integrity failure: " + name)
            _validate_packaged_payloads(z, names, metadata["source"]["sha256"])
    badging = run([tools.tool("aapt2"), "dump", "badging", path], log)
    if not re.search(r"^package: name='" + re.escape(expected_package) + r"'", badging, re.M):
        raise InputError("manifest package identity mismatch")
    if not re.search(r"^launchable-activity: name='" + re.escape(expected_entry) + r"'", badging, re.M):
        raise InputError("manifest Android entry point missing/mismatched")
    icons = re.findall(r"^application-icon-\d+:'([^']+)'", badging, re.M)
    if not icons or not any(icon in names for icon in icons):
        raise InputError("manifest icon is absent")
    signature = run(
        [tools.tool("apksigner"), "verify", "--verbose", "--print-certs", "--min-sdk-version", "26", path],
        log,
    )
    run([tools.tool("zipalign"), "-c", "-P", "16", "4", path], log)
    return {
        "status": "PASSED",
        "abi": target_abi,
        "checks": [
            "structure",
            "binary-manifest",
            "package",
            "launcher",
            "DEX-integrity-and-entry",
            "signing",
            f"{target_abi}-ELF",
            "dependencies",
            "resources",
            "icon",
            "alignment",
        ]
        + (
            ["assets", "resource-hashes", "native-JNI-entry", "native-code-hash", "conversion-provenance"]
            if converted
            else []
        ),
        "libraries": libraries,
        "targetAbi": target_abi,
        "signature": signature.strip(),
        "runtimeExecution": "NOT_TESTED",
    }
