"""Generate a tiny, explicit set of Darwin time-API replacement sources.

These wrappers are implemented and host-tested in ``native/src``. This module
copies only wrappers whose callers are statically reachable from the selected
Mach-O entry through reconstructed internal calls. It does not rewrite call
sites, prove dynamic Objective-C dispatch, link the source into the translated
entry library, or claim complete APK/game coverage.
"""

from __future__ import annotations

import hashlib
from pathlib import Path


# Darwin import -> (implementation symbol, per-symbol selection macro).
#
# The "time" entries are implemented in native/src/apple_time_compat.cpp; the
# "cf" and "libc" entries come from native/src/radek_ios_shims.cpp and mirror the
# RADEK_IOS_SHIM_TABLE macro in native/include/radek_ios_shims.h.
# tests/test_api_translation.py asserts this table stays identical to that macro,
# so the host generator, the on-device registry and the JNI resolver cannot drift.
_SUPPORTED = {
    "_CFAbsoluteTimeGetCurrent": (
        "CFAbsoluteTimeGetCurrent",
        "RADEK_API_CFAbsoluteTimeGetCurrent",
    ),
    "_CACurrentMediaTime": ("CACurrentMediaTime", "RADEK_API_CACurrentMediaTime"),
    "_mach_absolute_time": ("mach_absolute_time", "RADEK_API_mach_absolute_time"),
    "_mach_timebase_info": ("mach_timebase_info", "RADEK_API_mach_timebase_info"),
    "_CFAllocatorGetDefault": ("radek_compat_CFAllocatorGetDefault", "RADEK_API_radek_compat_CFAllocatorGetDefault"),
    "_CFRetain": ("radek_compat_CFRetain", "RADEK_API_radek_compat_CFRetain"),
    "_CFRelease": ("radek_compat_CFRelease", "RADEK_API_radek_compat_CFRelease"),
    "_CFGetRetainCount": ("radek_compat_CFGetRetainCount", "RADEK_API_radek_compat_CFGetRetainCount"),
    "_CFStringCreateWithCString": ("radek_compat_CFStringCreateWithCString", "RADEK_API_radek_compat_CFStringCreateWithCString"),
    "_CFStringGetLength": ("radek_compat_CFStringGetLength", "RADEK_API_radek_compat_CFStringGetLength"),
    "_CFStringGetCString": ("radek_compat_CFStringGetCString", "RADEK_API_radek_compat_CFStringGetCString"),
    "_CFStringGetCStringPtr": ("radek_compat_CFStringGetCStringPtr", "RADEK_API_radek_compat_CFStringGetCStringPtr"),
    "_CFStringGetMaximumSizeForEncoding": ("radek_compat_CFStringGetMaximumSizeForEncoding", "RADEK_API_radek_compat_CFStringGetMaximumSizeForEncoding"),
    "_CFStringCompare": ("radek_compat_CFStringCompare", "RADEK_API_radek_compat_CFStringCompare"),
    "_CFStringGetSystemEncoding": ("radek_compat_CFStringGetSystemEncoding", "RADEK_API_radek_compat_CFStringGetSystemEncoding"),
    "_CFDataCreate": ("radek_compat_CFDataCreate", "RADEK_API_radek_compat_CFDataCreate"),
    "_CFDataGetBytePtr": ("radek_compat_CFDataGetBytePtr", "RADEK_API_radek_compat_CFDataGetBytePtr"),
    "_CFDataGetLength": ("radek_compat_CFDataGetLength", "RADEK_API_radek_compat_CFDataGetLength"),
    "_CFArrayCreateMutable": ("radek_compat_CFArrayCreateMutable", "RADEK_API_radek_compat_CFArrayCreateMutable"),
    "_CFArrayAppendValue": ("radek_compat_CFArrayAppendValue", "RADEK_API_radek_compat_CFArrayAppendValue"),
    "_CFArrayGetCount": ("radek_compat_CFArrayGetCount", "RADEK_API_radek_compat_CFArrayGetCount"),
    "_CFArrayGetValueAtIndex": ("radek_compat_CFArrayGetValueAtIndex", "RADEK_API_radek_compat_CFArrayGetValueAtIndex"),
    "_CFDictionaryCreateMutable": ("radek_compat_CFDictionaryCreateMutable", "RADEK_API_radek_compat_CFDictionaryCreateMutable"),
    "_CFDictionarySetValue": ("radek_compat_CFDictionarySetValue", "RADEK_API_radek_compat_CFDictionarySetValue"),
    "_CFDictionaryGetValue": ("radek_compat_CFDictionaryGetValue", "RADEK_API_radek_compat_CFDictionaryGetValue"),
    "_CFDictionaryGetCount": ("radek_compat_CFDictionaryGetCount", "RADEK_API_radek_compat_CFDictionaryGetCount"),
    "_CFNumberCreate": ("radek_compat_CFNumberCreate", "RADEK_API_radek_compat_CFNumberCreate"),
    "_CFNumberGetValue": ("radek_compat_CFNumberGetValue", "RADEK_API_radek_compat_CFNumberGetValue"),
    "_CFDateCreate": ("radek_compat_CFDateCreate", "RADEK_API_radek_compat_CFDateCreate"),
    "_CFDateGetAbsoluteTime": ("radek_compat_CFDateGetAbsoluteTime", "RADEK_API_radek_compat_CFDateGetAbsoluteTime"),
    "_CFDateGetTimeIntervalSinceDate": ("radek_compat_CFDateGetTimeIntervalSinceDate", "RADEK_API_radek_compat_CFDateGetTimeIntervalSinceDate"),
    "_CFAbsoluteTimeGetGregorianDate": ("radek_compat_CFAbsoluteTimeGetGregorianDate", "RADEK_API_radek_compat_CFAbsoluteTimeGetGregorianDate"),
    "_malloc": ("radek_compat_malloc", "RADEK_API_radek_compat_malloc"),
    "_calloc": ("radek_compat_calloc", "RADEK_API_radek_compat_calloc"),
    "_realloc": ("radek_compat_realloc", "RADEK_API_radek_compat_realloc"),
    "_free": ("radek_compat_free", "RADEK_API_radek_compat_free"),
    "_memcpy": ("radek_compat_memcpy", "RADEK_API_radek_compat_memcpy"),
    "_memmove": ("radek_compat_memmove", "RADEK_API_radek_compat_memmove"),
    "_memset": ("radek_compat_memset", "RADEK_API_radek_compat_memset"),
    "_memcmp": ("radek_compat_memcmp", "RADEK_API_radek_compat_memcmp"),
    "_memchr": ("radek_compat_memchr", "RADEK_API_radek_compat_memchr"),
    "_strlen": ("radek_compat_strlen", "RADEK_API_radek_compat_strlen"),
    "_strcpy": ("radek_compat_strcpy", "RADEK_API_radek_compat_strcpy"),
    "_strncpy": ("radek_compat_strncpy", "RADEK_API_radek_compat_strncpy"),
    "_strlcpy": ("radek_compat_strlcpy", "RADEK_API_radek_compat_strlcpy"),
    "_strlcat": ("radek_compat_strlcat", "RADEK_API_radek_compat_strlcat"),
    "_strcmp": ("radek_compat_strcmp", "RADEK_API_radek_compat_strcmp"),
    "_strncmp": ("radek_compat_strncmp", "RADEK_API_radek_compat_strncmp"),
    "_strdup": ("radek_compat_strdup", "RADEK_API_radek_compat_strdup"),
    "_strchr": ("radek_compat_strchr", "RADEK_API_radek_compat_strchr"),
    "_strrchr": ("radek_compat_strrchr", "RADEK_API_radek_compat_strrchr"),
    "_strstr": ("radek_compat_strstr", "RADEK_API_radek_compat_strstr"),
    "_strtol": ("radek_compat_strtol", "RADEK_API_radek_compat_strtol"),
    "_strtod": ("radek_compat_strtod", "RADEK_API_radek_compat_strtod"),
    "_atoi": ("radek_compat_atoi", "RADEK_API_radek_compat_atoi"),
    "_atof": ("radek_compat_atof", "RADEK_API_radek_compat_atof"),
    "_strerror": ("radek_compat_strerror", "RADEK_API_radek_compat_strerror"),
    "_snprintf": ("radek_compat_snprintf", "RADEK_API_radek_compat_snprintf"),
    "_vsnprintf": ("radek_compat_vsnprintf", "RADEK_API_radek_compat_vsnprintf"),
    "_fopen": ("radek_compat_fopen", "RADEK_API_radek_compat_fopen"),
    "_fclose": ("radek_compat_fclose", "RADEK_API_radek_compat_fclose"),
    "_fread": ("radek_compat_fread", "RADEK_API_radek_compat_fread"),
    "_fwrite": ("radek_compat_fwrite", "RADEK_API_radek_compat_fwrite"),
    "_fputs": ("radek_compat_fputs", "RADEK_API_radek_compat_fputs"),
    "_fgets": ("radek_compat_fgets", "RADEK_API_radek_compat_fgets"),
    "_fflush": ("radek_compat_fflush", "RADEK_API_radek_compat_fflush"),
    "_fprintf": ("radek_compat_fprintf", "RADEK_API_radek_compat_fprintf"),
    "_printf": ("radek_compat_printf", "RADEK_API_radek_compat_printf"),
    "_puts": ("radek_compat_puts", "RADEK_API_radek_compat_puts"),
    "_remove": ("radek_compat_remove", "RADEK_API_radek_compat_remove"),
    "_feof": ("radek_compat_feof", "RADEK_API_radek_compat_feof"),
    "_ftell": ("radek_compat_ftell", "RADEK_API_radek_compat_ftell"),
    "_fseek": ("radek_compat_fseek", "RADEK_API_radek_compat_fseek"),
    "_time": ("radek_compat_time", "RADEK_API_radek_compat_time"),
    "_gettimeofday": ("radek_compat_gettimeofday", "RADEK_API_radek_compat_gettimeofday"),
    "_clock_gettime": ("radek_compat_clock_gettime", "RADEK_API_radek_compat_clock_gettime"),
    "_nanosleep": ("radek_compat_nanosleep", "RADEK_API_radek_compat_nanosleep"),
    "_localtime_r": ("radek_compat_localtime_r", "RADEK_API_radek_compat_localtime_r"),
    "_gmtime_r": ("radek_compat_gmtime_r", "RADEK_API_radek_compat_gmtime_r"),
    "_mktime": ("radek_compat_mktime", "RADEK_API_radek_compat_mktime"),
    "_getenv": ("radek_compat_getenv", "RADEK_API_radek_compat_getenv"),
    "_setenv": ("radek_compat_setenv", "RADEK_API_radek_compat_setenv"),
    "_unsetenv": ("radek_compat_unsetenv", "RADEK_API_radek_compat_unsetenv"),
    "_getpid": ("radek_compat_getpid", "RADEK_API_radek_compat_getpid"),
    "_qsort": ("radek_compat_qsort", "RADEK_API_radek_compat_qsort"),
    "_bsearch": ("radek_compat_bsearch", "RADEK_API_radek_compat_bsearch"),
    "_abs": ("radek_compat_abs", "RADEK_API_radek_compat_abs"),
    "_labs": ("radek_compat_labs", "RADEK_API_radek_compat_labs"),
    "_rand": ("radek_compat_rand", "RADEK_API_radek_compat_rand"),
    "_srand": ("radek_compat_srand", "RADEK_API_radek_compat_srand"),
    "_sqrt": ("radek_compat_sqrt", "RADEK_API_radek_compat_sqrt"),
    "_fabs": ("radek_compat_fabs", "RADEK_API_radek_compat_fabs"),
    "_floor": ("radek_compat_floor", "RADEK_API_radek_compat_floor"),
    "_ceil": ("radek_compat_ceil", "RADEK_API_radek_compat_ceil"),
    "_pow": ("radek_compat_pow", "RADEK_API_radek_compat_pow"),
    "_sin": ("radek_compat_sin", "RADEK_API_radek_compat_sin"),
    "_cos": ("radek_compat_cos", "RADEK_API_radek_compat_cos"),
    "_tan": ("radek_compat_tan", "RADEK_API_radek_compat_tan"),
    "_atan2": ("radek_compat_atan2", "RADEK_API_radek_compat_atan2"),
    "_fmod": ("radek_compat_fmod", "RADEK_API_radek_compat_fmod"),
    "_pthread_mutex_init": ("radek_compat_pthread_mutex_init", "RADEK_API_radek_compat_pthread_mutex_init"),
    "_pthread_mutex_lock": ("radek_compat_pthread_mutex_lock", "RADEK_API_radek_compat_pthread_mutex_lock"),
    "_pthread_mutex_unlock": ("radek_compat_pthread_mutex_unlock", "RADEK_API_radek_compat_pthread_mutex_unlock"),
    "_pthread_mutex_destroy": ("radek_compat_pthread_mutex_destroy", "RADEK_API_radek_compat_pthread_mutex_destroy"),
    "_pthread_cond_init": ("radek_compat_pthread_cond_init", "RADEK_API_radek_compat_pthread_cond_init"),
    "_pthread_cond_wait": ("radek_compat_pthread_cond_wait", "RADEK_API_radek_compat_pthread_cond_wait"),
    "_pthread_cond_signal": ("radek_compat_pthread_cond_signal", "RADEK_API_radek_compat_pthread_cond_signal"),
    "_pthread_cond_broadcast": ("radek_compat_pthread_cond_broadcast", "RADEK_API_radek_compat_pthread_cond_broadcast"),
    "_pthread_cond_destroy": ("radek_compat_pthread_cond_destroy", "RADEK_API_radek_compat_pthread_cond_destroy"),
    "_pthread_self": ("radek_compat_pthread_self", "RADEK_API_radek_compat_pthread_self"),
}


# Which translation unit and helper machinery each shim needs.
#   time -> native/src/apple_time_compat.cpp
#   cf   -> native/src/radek_ios_shims.cpp + the shared CoreFoundation runtime
#   libc -> native/src/radek_ios_shims.cpp
_FAMILY = {
    "_CFAbsoluteTimeGetCurrent": "time",
    "_CACurrentMediaTime": "time",
    "_mach_absolute_time": "time",
    "_mach_timebase_info": "time",
    "_CFAllocatorGetDefault": "cf",
    "_CFRetain": "cf",
    "_CFRelease": "cf",
    "_CFGetRetainCount": "cf",
    "_CFStringCreateWithCString": "cf",
    "_CFStringGetLength": "cf",
    "_CFStringGetCString": "cf",
    "_CFStringGetCStringPtr": "cf",
    "_CFStringGetMaximumSizeForEncoding": "cf",
    "_CFStringCompare": "cf",
    "_CFStringGetSystemEncoding": "cf",
    "_CFDataCreate": "cf",
    "_CFDataGetBytePtr": "cf",
    "_CFDataGetLength": "cf",
    "_CFArrayCreateMutable": "cf",
    "_CFArrayAppendValue": "cf",
    "_CFArrayGetCount": "cf",
    "_CFArrayGetValueAtIndex": "cf",
    "_CFDictionaryCreateMutable": "cf",
    "_CFDictionarySetValue": "cf",
    "_CFDictionaryGetValue": "cf",
    "_CFDictionaryGetCount": "cf",
    "_CFNumberCreate": "cf",
    "_CFNumberGetValue": "cf",
    "_CFDateCreate": "cf",
    "_CFDateGetAbsoluteTime": "cf",
    "_CFDateGetTimeIntervalSinceDate": "cf",
    "_CFAbsoluteTimeGetGregorianDate": "cf",
    "_malloc": "libc",
    "_calloc": "libc",
    "_realloc": "libc",
    "_free": "libc",
    "_memcpy": "libc",
    "_memmove": "libc",
    "_memset": "libc",
    "_memcmp": "libc",
    "_memchr": "libc",
    "_strlen": "libc",
    "_strcpy": "libc",
    "_strncpy": "libc",
    "_strlcpy": "libc",
    "_strlcat": "libc",
    "_strcmp": "libc",
    "_strncmp": "libc",
    "_strdup": "libc",
    "_strchr": "libc",
    "_strrchr": "libc",
    "_strstr": "libc",
    "_strtol": "libc",
    "_strtod": "libc",
    "_atoi": "libc",
    "_atof": "libc",
    "_strerror": "libc",
    "_snprintf": "libc",
    "_vsnprintf": "libc",
    "_fopen": "libc",
    "_fclose": "libc",
    "_fread": "libc",
    "_fwrite": "libc",
    "_fputs": "libc",
    "_fgets": "libc",
    "_fflush": "libc",
    "_fprintf": "libc",
    "_printf": "libc",
    "_puts": "libc",
    "_remove": "libc",
    "_feof": "libc",
    "_ftell": "libc",
    "_fseek": "libc",
    "_time": "libc",
    "_gettimeofday": "libc",
    "_clock_gettime": "libc",
    "_nanosleep": "libc",
    "_localtime_r": "libc",
    "_gmtime_r": "libc",
    "_mktime": "libc",
    "_getenv": "libc",
    "_setenv": "libc",
    "_unsetenv": "libc",
    "_getpid": "libc",
    "_qsort": "libc",
    "_bsearch": "libc",
    "_abs": "libc",
    "_labs": "libc",
    "_rand": "libc",
    "_srand": "libc",
    "_sqrt": "libc",
    "_fabs": "libc",
    "_floor": "libc",
    "_ceil": "libc",
    "_pow": "libc",
    "_sin": "libc",
    "_cos": "libc",
    "_tan": "libc",
    "_atan2": "libc",
    "_fmod": "libc",
    "_pthread_mutex_init": "libc",
    "_pthread_mutex_lock": "libc",
    "_pthread_mutex_unlock": "libc",
    "_pthread_mutex_destroy": "libc",
    "_pthread_cond_init": "libc",
    "_pthread_cond_wait": "libc",
    "_pthread_cond_signal": "libc",
    "_pthread_cond_broadcast": "libc",
    "_pthread_cond_destroy": "libc",
    "_pthread_self": "libc",
}


def selection_defines(selected) -> list[str]:
    """Per-symbol (and CoreFoundation runtime) defines for a generated source."""
    defines = ["#define RADEK_API_REPLACEMENTS_ONLY 1"]
    defines += ["#define " + _SUPPORTED[name][1] + " 1" for name in selected]
    if any(_FAMILY[name] == "cf" for name in selected):
        defines.append("#define RADEK_API_NEEDS_CF_RUNTIME 1")
    defines.append("")
    return defines


def _as_address(value) -> int | None:
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError):
        return None


def _entry_reachable_functions(slice_data: dict) -> set[str]:
    """Return reconstructed function names reachable from the selected entry.

    Internal edges are followed only when the reconstruction marks them
    non-external and resolves their target to a reconstructed function. Unknown
    edges are not guessed into reachable paths.
    """
    functions = slice_data.get("functions", []) or []
    entry = _as_address(slice_data.get("entryPoint"))
    if entry is None:
        return set()

    names = {item.get("name") for item in functions if isinstance(item, dict) and item.get("name")}
    by_address = {
        address: item["name"]
        for item in functions
        if isinstance(item, dict)
        and item.get("name")
        and (address := _as_address(item.get("address"))) is not None
    }
    roots = {by_address[entry]} if entry in by_address else set()
    if not roots:
        return set()

    adjacency: dict[str, set[str]] = {}
    for edge in ((slice_data.get("callGraph") or {}).get("edges") or []):
        if not isinstance(edge, dict) or edge.get("external") is not False:
            continue
        caller = edge.get("from")
        if caller not in names:
            continue
        callee = edge.get("to")
        if callee not in names:
            target = _as_address(edge.get("address"))
            callee = by_address.get(target)
        if callee in names:
            adjacency.setdefault(caller, set()).add(callee)

    reachable = set(roots)
    pending = list(roots)
    while pending:
        caller = pending.pop()
        for callee in adjacency.get(caller, set()):
            if callee not in reachable:
                reachable.add(callee)
                pending.append(callee)
    return reachable


def reachable_imports(reconstruction: dict) -> set[str]:
    """Collect supported imports called by a function reachable from LC_MAIN."""
    names: set[str] = set()
    for image in reconstruction.get("images", []) or []:
        for slice_data in image.get("slices", []) or []:
            callers = _entry_reachable_functions(slice_data)
            if not callers:
                continue
            for use in ((slice_data.get("apis") or {}).get("used") or []):
                if not isinstance(use, dict):
                    continue
                name = use.get("name")
                use_callers = use.get("callers") or []
                if isinstance(name, str) and name in _SUPPORTED and any(caller in callers for caller in use_callers):
                    names.add(name)
    return names


def generate(reconstruction: dict, output: Path) -> dict:
    """Write compilable replacement source for exact supported entry-reachable APIs."""
    selected = sorted(reachable_imports(reconstruction))
    if not selected:
        return {
            "status": "NO_ENTRY_REACHABLE_IMPLEMENTED_API",
            "attempted": False,
            "generatedApiReplacements": 0,
            "linkedApiReplacements": 0,
            "totalReachableApiCount": 0,
            "untranslatedReachableApiCount": 0,
            "codeGenerated": False,
            "completeGameConversion": False,
            "message": (
                "No reconstructed call from the selected Mach-O entry reached the small implemented "
                "time-API subset; name matches and semantic targets remain analysis-only."
            ),
            "replacements": [],
        }

    source_root = Path(__file__).resolve().parent.parent / "native"
    source_dir = output / "api-replacements"
    source_dir.mkdir(parents=True, exist_ok=True)
    implementation = source_root / "src" / "apple_time_compat.cpp"
    shim_implementation = source_root / "src" / "radek_ios_shims.cpp"
    for header_name in ("apple_time_compat.h", "radek_ios_shims.h"):
        (source_dir / header_name).write_bytes(
            (source_root / "include" / header_name).read_bytes()
        )

    generated_source = source_dir / "api-replacements.cpp"
    generated_source.write_text(
        "\n".join(selection_defines(selected))
        + implementation.read_text(encoding="utf-8")
        + "\n"
        + shim_implementation.read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    source_hash = hashlib.sha256(generated_source.read_bytes()).hexdigest()

    replacements = []
    for source_symbol in selected:
        target_symbol, _ = _SUPPORTED[source_symbol]
        replacements.append(
            {
                "sourceSymbol": source_symbol,
                "targetAndroidApi": f"libioscompat.so:{target_symbol}",
                "targetSymbol": target_symbol,
                "implementationArtifact": "api-replacements/api-replacements.cpp",
                "implementationSha256": source_hash,
                "codeGenerated": True,
                "linkedIntoGame": False,
                "linkedIntoApk": False,
                "reachableInSourceImage": True,
                "reachableFromEntry": True,
            }
        )

    return {
        "status": "IMPLEMENTATIONS_GENERATED_NOT_LINKED",
        "attempted": True,
        "generatedApiReplacements": len(replacements),
        "linkedApiReplacements": 0,
        "totalReachableApiCount": len(selected),
        "untranslatedReachableApiCount": 0,
        "codeGenerated": True,
        "sourcePath": "api-replacements/api-replacements.cpp",
        "headerPath": "api-replacements/apple_time_compat.h",
        "headerPaths": [
            "api-replacements/apple_time_compat.h",
            "api-replacements/radek_ios_shims.h",
        ],
        "implementationTemplate": "native/src/apple_time_compat.cpp",
        "implementationTemplates": [
            "native/src/apple_time_compat.cpp",
            "native/src/radek_ios_shims.cpp",
        ],
        "completeGameConversion": False,
        "message": (
            f"Generated {len(replacements)} real time-API replacement implementation(s) from entry-reachable "
            "imports. The sources are not linked into the translated entry library or an APK."
        ),
        "replacements": replacements,
    }
