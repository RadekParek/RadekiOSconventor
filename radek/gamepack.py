"""Bounded complete-game conversion backend (``complete-game-v1``).

The general iOS-to-Android game converter does not exist. This module converts
the one statically proven subset end to end, and it fails closed for anything
outside it. An IPA is convertible only when **every** check below is verified
from its own analysis, never assumed:

* exactly one Mach-O image (no embedded frameworks/plugins);
* one ARM slice selected by the normal ABI policy;
* no imports, no linked dependencies, no dyld fixups/chained fixups, no
  Objective-C/Swift/unwind metadata, no encryption;
* the executable's whole ``__text`` section is exactly the proven
  closed-integer entry routine (MOV-immediate/MOVK/register MOV/immediate
  ADD/SUB/RET), so reachable functions == translated functions == 1 and
  reachable instruction-byte coverage is 100%;
* no reachable APIs (nothing is imported), so API accounting is complete with
  zero replacements and zero passthroughs;
* every remaining bundle file is small enough to be packaged verbatim under
  ``assets/bundle/`` and hashed into the resource inventory;
* no launcher icon was recovered (a host cannot reproduce the byte-exact
  device re-encode, so an icon-bearing IPA stays honestly blocked here; the
  on-device converter handles those).

The produced APK carries a generated Android launcher that displays the launch
message recovered from the IPA's ``__cstring`` section and executes the
translated entry through JNI, ``lib/<abi>/libconverted.so`` exporting
``Java_dev_radek_generated_MainActivity_runNative``, and the full
``complete-game-v1`` provenance checked by ``radek.apk.validate_apk`` and the
Android app's attachment flow.
"""

from __future__ import annotations

import hashlib
import json
import zipfile
from pathlib import Path

from .apk import (
    Toolchain,
    _android_jar,
    _android_string_escape,
    _generate_debug_keystore,
    run,
    validate_apk,
)
from .archive import InputError, safe_name
from .elf_writer import build_shared_object
from .resources import MACH_MAGICS, fallback_icon

COMPLETE_GAME_CONTRACT = "complete-game-v1"
COMPLETE_GAME_ENTRY = "dev.radek.generated.MainActivity"
COMPLETE_GAME_JNI_SYMBOL = "Java_dev_radek_generated_MainActivity_runNative"
COMPLETE_GAME_BACKEND = "radek-bounded-leaf-v1"
DEFAULT_LAUNCH_MESSAGE = "Converted by RadekiOSConventor"

MAX_RESOURCE_FILES = 4096
MAX_RESOURCE_FILE_BYTES = 32 * 1024 * 1024
MAX_RESOURCE_TOTAL_BYTES = 256 * 1024 * 1024
MAX_LAUNCH_MESSAGE = 512

_CONVERTED_MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{package}"
    android:versionCode="1"
    android:versionName="1.0">
    <application android:label="@string/app_name" android:icon="@drawable/converted_icon"
        android:allowBackup="false">
        <activity android:name="dev.radek.generated.MainActivity" android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
"""

_CONVERTED_STRINGS = """<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">{app_name}</string>
</resources>
"""

_CONVERTED_ACTIVITY = """package dev.radek.generated;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Launcher of a bounded complete-game conversion. Shows the message recovered
 *  from the IPA and runs the translated native entry. */
public final class MainActivity extends Activity {
    private static boolean nativeReady = false;

    static {
        try {
            System.loadLibrary("converted");
            nativeReady = true;
        } catch (Throwable ignored) {
            // The launcher still shows the recovered message below.
        }
    }

    private static native int runNative();

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private JSONObject readMetadata() {
        try (InputStream input = getAssets().open("conversion.json")) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            int total = 0;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > 1048576) return new JSONObject();
                output.write(buffer, 0, count);
            }
            return new JSONObject(new String(output.toByteArray(), StandardCharsets.UTF_8));
        } catch (Exception ignored) {
            return new JSONObject();
        }
    }

    private TextView label(String value, float size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setGravity(Gravity.CENTER);
        if (bold) view.setTypeface(null, android.graphics.Typeface.BOLD);
        view.setPadding(dp(12), dp(8), dp(12), dp(8));
        return view;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        JSONObject metadata = readMetadata();
        String message = metadata.optString("launchMessage", "Converted by RadekiOSConventor");
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(11, 16, 29));
        root.setPadding(dp(24), dp(48), dp(24), dp(48));
        root.addView(label(message, 26, Color.WHITE, true));
        String appName = metadata.optString("applicationName", "");
        if (!appName.isEmpty()) {
            root.addView(label(appName, 14, Color.rgb(160, 178, 199), false));
        }
        String nativeLine;
        if (nativeReady) {
            try {
                nativeLine = "Translated iOS entry executed on Android, returned: " + runNative();
            } catch (Throwable error) {
                nativeLine = "Translated native entry did not run: " + error;
            }
        } else {
            nativeLine = "Translated native library could not be loaded on this device.";
        }
        root.addView(label(nativeLine, 13, Color.rgb(92, 227, 181), false));
        root.addView(label(
                "Bounded complete conversion of one statically proven entry routine. "
                        + "Generated by RadekiOSConventor from an authorized IPA.",
                12, Color.rgb(160, 178, 199), false));
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(11, 16, 29));
        scroll.addView(root);
        setContentView(scroll);
    }
}
"""


def _launch_message_from_cstrings(data: bytes, selected: dict) -> str:
    """Recover the longest printable ASCII string from ``__cstring`` sections."""
    best = ""
    for segment in selected.get("segments", []):
        for section in segment.get("sections", []):
            if section.get("name") != "__cstring":
                continue
            offset = int(section.get("offset", 0))
            size = int(section.get("size", 0))
            if size <= 0 or size > 1024 * 1024 or offset + size > len(data):
                continue
            blob = data[offset : offset + size]
            for chunk in blob.split(b"\x00"):
                if not 3 <= len(chunk) <= MAX_LAUNCH_MESSAGE:
                    continue
                try:
                    text = chunk.decode("ascii")
                except UnicodeDecodeError:
                    continue
                if not all(32 <= ord(ch) < 127 for ch in text):
                    continue
                if len(text) > len(best):
                    best = text
    return best


def _resource_inventory(app: Path, executable_name: str) -> tuple[list[dict], dict[str, bytes]]:
    """Collect every packageable bundle file with its hash, bounded and fail-closed."""
    inventory: list[dict] = []
    payloads: dict[str, bytes] = {}
    total = 0
    files = sorted(p for p in app.rglob("*") if p.is_file())
    if len(files) > MAX_RESOURCE_FILES:
        raise InputError("bundle has too many resource files for the bounded converter")
    for path in files:
        relative = path.relative_to(app).as_posix()
        if relative == executable_name:
            continue
        safe_name(relative)
        payload = path.read_bytes()
        if len(payload) > MAX_RESOURCE_FILE_BYTES:
            raise InputError("bundle resource exceeds the bounded converter size limit: " + relative)
        if payload[:4] in MACH_MAGICS:
            raise InputError("embedded Apple executable is not packageable: " + relative)
        total += len(payload)
        if total > MAX_RESOURCE_TOTAL_BYTES:
            raise InputError("bundle resources exceed the bounded converter total limit")
        inventory.append({"path": relative, "sha256": hashlib.sha256(payload).hexdigest()})
        payloads[relative] = payload
    return inventory, payloads


def assess_complete_conversion(
    app: Path,
    executable_name: str,
    executable_data: bytes,
    mach: dict,
    graph: dict,
    reconstruction: dict,
    selected: dict,
    program,
    icon_status: str,
) -> dict:
    """Statically verify every complete-conversion precondition. Fail closed."""
    reasons: list[str] = []

    def block(reason: str):
        reasons.append(reason)

    if len(graph.get("nodes", [])) != 1:
        block("embedded frameworks/plugins require a linker backend that is not implemented")
    if selected.get("encrypted"):
        block("encrypted slice cannot be converted")
    if selected.get("imports"):
        block("imported symbols require API replacement implementations")
    if selected.get("dependencies"):
        block("linked dependencies are not provided")
    if selected.get("metadata"):
        block("Objective-C/Swift/unwind metadata runtime is not implemented")
    if "chainedFixups" in selected:
        block("chained fixups are not reconstructed")
    if selected.get("bindDecodingComplete") is False:
        block("dyld bind table did not decode completely")
    if selected.get("architecture") not in ("arm64", "armv7", "armv7s", "armv6"):
        block("no supported ARM slice")

    entry = selected.get("entryOffset")
    section = None
    if isinstance(entry, int):
        for segment in selected.get("segments", []):
            for sec in segment.get("sections", []):
                if (
                    sec.get("name") == "__text"
                    and int(sec.get("offset", 0)) <= entry < int(sec.get("offset", 0)) + int(sec.get("size", 0))
                    and int(segment.get("initialProtection", 0)) & 4
                ):
                    section = sec
    if section is None:
        block("entry is not inside an executable __text section")
    elif entry != int(section.get("offset", 0)) or program.source_size != int(section.get("size", 0)):
        block("the proven entry routine does not cover the whole executable __text section")

    images = reconstruction.get("images", []) if reconstruction else []
    if len(images) != 1:
        block("reconstruction must cover exactly one image")
    else:
        slices = images[0].get("slices", [])
        slice_data = next((s for s in slices if s.get("architecture") == selected.get("architecture")), None)
        if slice_data is None:
            block("no reconstruction for the selected slice")
        else:
            if slice_data.get("error"):
                block("reconstruction of the selected slice failed: " + str(slice_data["error"]))
            stats = slice_data.get("disassembly") or {}
            if stats.get("functions") != 1:
                block(
                    "reachable function count is not exactly one proven routine: "
                    f'{stats.get("functions", 0)}'
                )
            if stats.get("unknownInstructions"):
                block("undecoded instructions remain in the reachable code")
            functions = slice_data.get("functions", [])
            if len(functions) != 1 or functions[0].get("address") != slice_data.get("entryPoint"):
                block("the single reconstructed function is not the entry point")
            elif functions[0].get("calls"):
                block("the entry routine makes calls that are not translated")
            apis = slice_data.get("apis") or {}
            if apis.get("used") or apis.get("importCount"):
                block("reachable APIs require generated replacements that do not exist")
            objc = slice_data.get("objectiveC") or {}
            if objc.get("classCount") or objc.get("selectorCount") or objc.get("messageSelectorCount"):
                block("Objective-C runtime metadata is present")
            swift = slice_data.get("swift") or {}
            if swift.get("typeCount") or swift.get("symbolCount"):
                block("Swift runtime metadata is present")
            if slice_data.get("importCount"):
                block("reconstructed imports are present")

    if icon_status == "SUPPORTED":
        block(
            "a recovered launcher icon cannot be byte-reproduced by the host converter; "
            "use the on-device converter for icon-bearing IPAs"
        )

    inventory: list[dict] = []
    payloads: dict[str, bytes] = {}
    launch_message = ""
    if not reasons:
        try:
            inventory, payloads = _resource_inventory(app, executable_name)
        except InputError as exc:
            block(str(exc))
        launch_message = _launch_message_from_cstrings(executable_data, selected)

    return {
        "eligible": not reasons,
        "reasons": reasons,
        "resourceInventory": inventory,
        "resourcePayloads": payloads,
        "launchMessage": launch_message,
    }


def complete_game_metadata(
    application: dict,
    source_sha256: str,
    target_abi: str,
    machine_code: bytes,
    inventory: list[dict],
    launch_message: str,
) -> dict:
    """Assemble the ``complete-game-v1`` provenance checked by both validators."""
    package = "dev.radek.converted.p" + source_sha256[:20]
    return {
        "contract": COMPLETE_GAME_CONTRACT,
        "generator": "RadekiOSConventor",
        "package": package,
        "targetAbi": target_abi,
        "launchMessage": launch_message or DEFAULT_LAUNCH_MESSAGE,
        "applicationName": application.get("name", ""),
        "source": {
            "sha256": source_sha256,
            "originalName": application.get("originalName", ""),
            "bundleId": application.get("bundleId", ""),
        },
        "conversion": {
            "backend": COMPLETE_GAME_BACKEND,
            "targetAbi": target_abi,
            "outputBytes": len(machine_code),
            "machineCodeSha256": hashlib.sha256(machine_code).hexdigest(),
            "entrySymbol": COMPLETE_GAME_JNI_SYMBOL,
        },
        "gameConversion": {
            "status": "COMPLETE",
            "completeGameConversion": True,
            "reachableSourceFunctions": 1,
            "translatedReachableFunctions": 1,
            "untranslatedReachableFunctions": 0,
            "reachableApiCount": 0,
            "generatedApiReplacements": 0,
            "nativeApiPassthroughs": 0,
            "untranslatedReachableApiCount": 0,
            "apiCoverageComplete": True,
            "apiReplacements": [],
            "resourcesComplete": True,
            "lifecycleImplemented": True,
            "sourceIconSha256": "",
            "launcherIconSha256": "",
            "backend": COMPLETE_GAME_BACKEND,
        },
        "resourceInventory": inventory,
    }


def converted_sources(work: Path, package: str, app_label: str) -> Path:
    root = work / "converted-src"
    (root / "res" / "values").mkdir(parents=True, exist_ok=True)
    (root / "res" / "drawable-nodpi").mkdir(parents=True, exist_ok=True)
    (root / "src").mkdir(parents=True, exist_ok=True)
    (root / "AndroidManifest.xml").write_text(
        _CONVERTED_MANIFEST.format(package=package), encoding="utf-8"
    )
    (root / "res" / "values" / "strings.xml").write_text(
        _CONVERTED_STRINGS.format(app_name=_android_string_escape(app_label or "Converted IPA")),
        encoding="utf-8",
    )
    (root / "res" / "drawable-nodpi" / "converted_icon.png").write_bytes(fallback_icon())
    (root / "src" / "MainActivity.java").write_text(_CONVERTED_ACTIVITY, encoding="utf-8")
    return root


def build_complete_game(
    work: Path,
    final_path: Path,
    tools: Toolchain,
    metadata: dict,
    machine_code: bytes,
    target_arch: str,
    resource_payloads: dict[str, bytes],
    log=None,
) -> dict:
    """Assemble, align and sign the bounded complete-game APK.

    The caller runs ``radek.apk.validate_apk`` afterwards; this function only
    builds. Every step uses the real Android toolchain.
    """
    package = metadata["package"]
    target_abi = metadata["targetAbi"]
    sources = converted_sources(work, package, metadata.get("applicationName", ""))
    android_jar = _android_jar(tools)
    build = work / "converted-build"
    build.mkdir(parents=True, exist_ok=True)

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
            "26",
            "--target-sdk-version",
            "35",
            compiled,
        ],
        log=log,
    )

    classes = build / "classes"
    classes.mkdir(parents=True, exist_ok=True)
    java_sources = [str(sources / "src" / "MainActivity.java")]
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
            "26",
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
        raise RuntimeError("d8 produced no classes.dex for the converted launcher")

    library = build_shared_object(machine_code, target_arch, symbol=COMPLETE_GAME_JNI_SYMBOL)

    with zipfile.ZipFile(base_apk, "a", compression=zipfile.ZIP_STORED) as package_zip:
        package_zip.writestr("classes.dex", dex.read_bytes())
        package_zip.writestr(f"lib/{target_abi}/libconverted.so", library)
        package_zip.writestr(
            "assets/conversion.json", json.dumps(metadata, indent=2, ensure_ascii=True)
        )
        for relative in sorted(resource_payloads):
            package_zip.writestr("assets/bundle/" + relative, resource_payloads[relative],
                                 compress_type=zipfile.ZIP_DEFLATED)

    aligned = build / "aligned.apk"
    run([tools.tool("zipalign"), "-f", "-P", "16", "4", base_apk, aligned], log=log)

    keystore, password = _generate_debug_keystore(build, log=log)
    final_path.parent.mkdir(parents=True, exist_ok=True)
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
            "--min-sdk-version",
            "26",
            "--out",
            final_path,
            aligned,
        ],
        log=log,
    )
    return {
        "path": str(final_path),
        "sha256": hashlib.sha256(final_path.read_bytes()).hexdigest(),
        "sizeBytes": final_path.stat().st_size,
        "package": package,
        "entry": COMPLETE_GAME_ENTRY,
        "targetAbi": target_abi,
        "signedWith": "local debug keystore generated for this run",
    }


def validate_complete_game(
    apk: Path,
    tools: Toolchain,
    metadata: dict,
    expected_abi: str | None = None,
    log=None,
) -> dict:
    """Run the strict complete-game validator over a freshly built APK."""
    return validate_apk(
        apk,
        tools,
        metadata["package"],
        COMPLETE_GAME_ENTRY,
        converted=True,
        log=log,
        expected_abi=expected_abi,
    )
