package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/**
 * Conservative native-symbol and semantic API equivalence inventory.
 * A candidate is a static recompilation plan only: this module does not rewrite Mach-O
 * code, bridge Objective-C objects, link a Bionic library, or generate Java.
 */
internal object AndroidApiMapper {
    // Keep high-volume games analyzable while bounding mapper/report growth.
    private const val MAX_SYMBOLS = 100_000

    private val ndkRuntimeLibraries = setOf(
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so", "libEGL.so",
        "libGLESv1_CM.so", "libGLESv2.so", "libaaudio.so", "libmediandk.so", "libvulkan.so",
        "libOpenSLES.so", "libOpenMAXAL.so", "libjnigraphics.so", "libbinder_ndk.so", "libamidi.so",
        "libcamera2ndk.so", "libc++_shared.so", "libnativewindow.so", "libneuralnetworks.so",
        "libsync.so",
    )

    private val bionicLibraries = mapOf(
        "libc.so" to setOf(
            "abort", "abs", "atoi", "atof", "calloc", "clock_gettime", "close", "exit", "fclose", "feof",
            "ferror", "fflush", "fgetc", "fgets", "fopen", "fprintf", "fputc", "fputs", "fread", "free",
            "fseek", "ftell", "fwrite", "getenv", "gettimeofday", "malloc", "memcmp", "memcpy", "memmove",
            "memset", "mkdir", "open", "perror", "printf", "puts", "read", "realloc", "remove", "rename",
            "rmdir", "scanf", "snprintf", "sprintf", "strcmp", "strcpy", "strdup", "strerror", "strlen",
            "strncat", "strncmp", "strncpy", "strnlen", "strrchr", "strchr", "strstr", "strtol", "strtoll",
            "strtoul", "strtoull", "tolower", "toupper", "unlink", "vsnprintf", "write", "__stack_chk_fail",
            "pthread_create", "pthread_join", "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_unlock",
            "pthread_cond_init", "pthread_cond_wait", "pthread_cond_signal", "pthread_once",
            "socket", "connect", "send", "recv", "bind", "listen", "accept", "shutdown",
        ),
        "libdl.so" to setOf("dlopen", "dlsym", "dlclose", "dlerror"),
        "libm.so" to setOf(
            "acos", "asin", "atan", "atan2", "ceil", "cos", "exp", "fabs", "floor", "log", "pow", "sin",
            "sqrt", "tan", "acosf", "asinf", "atanf", "atan2f", "ceilf", "cosf", "expf", "fabsf", "floorf",
            "logf", "powf", "sinf", "sqrtf", "tanf",
        ),
        "libGLESv2.so" to setOf(
            "glActiveTexture", "glAttachShader", "glBindAttribLocation", "glBindBuffer", "glBindFramebuffer",
            "glBindRenderbuffer", "glBindTexture", "glBlendFunc", "glBufferData", "glCheckFramebufferStatus",
            "glClear", "glClearColor", "glCompileShader", "glCreateProgram", "glCreateShader", "glDeleteBuffers",
            "glDeleteFramebuffers", "glDeleteProgram", "glDeleteRenderbuffers", "glDeleteShader", "glDeleteTextures",
            "glDrawArrays", "glDrawElements", "glEnable", "glEnableVertexAttribArray", "glFinish", "glFlush",
            "glFramebufferRenderbuffer", "glFramebufferTexture2D", "glGenBuffers", "glGenFramebuffers",
            "glGenRenderbuffers", "glGenTextures", "glGetAttribLocation", "glGetError", "glGetIntegerv",
            "glGetProgramInfoLog", "glGetProgramiv", "glGetShaderInfoLog", "glGetShaderiv", "glGetUniformLocation",
            "glLinkProgram", "glScissor", "glShaderSource", "glTexImage2D", "glTexParameteri", "glUniform1f",
            "glUniform1i", "glUniform2f", "glUniformMatrix4fv", "glUseProgram", "glVertexAttribPointer", "glViewport",
        ),
        "libEGL.so" to setOf(
            "eglBindAPI", "eglChooseConfig", "eglCreateContext", "eglCreateWindowSurface", "eglDestroyContext",
            "eglDestroySurface", "eglGetConfigAttrib", "eglGetDisplay", "eglGetError", "eglInitialize",
            "eglMakeCurrent", "eglSwapBuffers", "eglTerminate",
        ),
        "libz.so" to setOf(
            "adler32", "compress", "compress2", "compressBound", "crc32", "deflate", "deflateEnd", "inflate",
            "inflateEnd", "uncompress", "zlibVersion",
        ),
    )

    /** These need source-level/ABI rewrites; none are direct native symbol aliases. */
    private val semanticTargets = mapOf(
        "UIApplication" to "android.app.Application + Activity lifecycle",
        "UIViewController" to "android.app.Activity or androidx.fragment.app.Fragment",
        "UIView" to "android.view.View",
        "UIWindow" to "android.view.Window",
        "UILabel" to "android.widget.TextView",
        "UIButton" to "android.widget.Button",
        "UIImage" to "android.graphics.Bitmap or android.graphics.drawable.Drawable",
        "UIImageView" to "android.widget.ImageView",
        "UIScrollView" to "android.widget.ScrollView",
        "UITableView" to "RecyclerView + LayoutManager + Adapter",
        "UICollectionView" to "RecyclerView + LayoutManager + Adapter",
        "UITextField" to "android.widget.EditText",
        "UITextView" to "android.widget.EditText or android.widget.TextView",
        "UIScreen" to "android.util.DisplayMetrics + WindowManager",
        "UIColor" to "android.graphics.Color",
        "UIFont" to "android.graphics.Typeface",
        "NSString" to "java.lang.String",
        "NSArray" to "java.util.List",
        "NSDictionary" to "java.util.Map",
        "NSData" to "byte[]",
        "NSBundle" to "Android assets/resources + package metadata",
        "NSFileManager" to "java.io.File + Android scoped-storage APIs",
        "NSUserDefaults" to "android.content.SharedPreferences or DataStore",
        "NSNotificationCenter" to "Lifecycle-aware callbacks or Android broadcasts",
        "NSTimer" to "Handler, ScheduledExecutorService, or Choreographer",
        "NSURLSession" to "HttpURLConnection or an approved Android HTTP client",
        "NSJSONSerialization" to "org.json or kotlinx.serialization",
        "AVAudioPlayer" to "android.media.MediaPlayer or SoundPool",
        "AVAudioSession" to "android.media.AudioManager + AudioAttributes",
        "CADisplayLink" to "C callback bridge driven by Android Choreographer (not the Objective-C CADisplayLink ABI)",
        "CAAnimation" to "android.animation.Animator",
        "SKScene" to "custom SurfaceView/Canvas renderer (game-loop rewrite required)",
    )

    /**
     * Real, host-tested implementation bodies exported by libioscompat.so.
     *
     * The four time shims live in native/src/apple_time_compat.cpp; other C,
     * POSIX and CoreFoundation entries come from RADEK_IOS_SHIM_TABLE in
     * native/include/radek_ios_shims.h, which is also expanded by
     * native/src/ioscompat_registry.cpp and native/src/jni.cpp. A dlsym hit here
     * proves an export exists; it does not rewrite an IPA callsite.
     */
    private val implementedApiReplacements = mapOf(
        "_CFAbsoluteTimeGetCurrent" to "CFAbsoluteTimeGetCurrent",
        "_CACurrentMediaTime" to "CACurrentMediaTime",
        "_mach_absolute_time" to "mach_absolute_time",
        "_mach_timebase_info" to "mach_timebase_info",
        "_CFAllocatorGetDefault" to "radek_compat_CFAllocatorGetDefault",
        "_CFRetain" to "radek_compat_CFRetain",
        "_CFRelease" to "radek_compat_CFRelease",
        "_CFGetRetainCount" to "radek_compat_CFGetRetainCount",
        "_CFStringCreateWithCString" to "radek_compat_CFStringCreateWithCString",
        "_CFStringGetLength" to "radek_compat_CFStringGetLength",
        "_CFStringGetCString" to "radek_compat_CFStringGetCString",
        "_CFStringGetCStringPtr" to "radek_compat_CFStringGetCStringPtr",
        "_CFStringGetMaximumSizeForEncoding" to "radek_compat_CFStringGetMaximumSizeForEncoding",
        "_CFStringCompare" to "radek_compat_CFStringCompare",
        "_CFStringGetSystemEncoding" to "radek_compat_CFStringGetSystemEncoding",
        "_CFDataCreate" to "radek_compat_CFDataCreate",
        "_CFDataGetBytePtr" to "radek_compat_CFDataGetBytePtr",
        "_CFDataGetLength" to "radek_compat_CFDataGetLength",
        "_CFArrayCreateMutable" to "radek_compat_CFArrayCreateMutable",
        "_CFArrayAppendValue" to "radek_compat_CFArrayAppendValue",
        "_CFArrayGetCount" to "radek_compat_CFArrayGetCount",
        "_CFArrayGetValueAtIndex" to "radek_compat_CFArrayGetValueAtIndex",
        "_CFDictionaryCreateMutable" to "radek_compat_CFDictionaryCreateMutable",
        "_CFDictionarySetValue" to "radek_compat_CFDictionarySetValue",
        "_CFDictionaryGetValue" to "radek_compat_CFDictionaryGetValue",
        "_CFDictionaryGetCount" to "radek_compat_CFDictionaryGetCount",
        "_CFNumberCreate" to "radek_compat_CFNumberCreate",
        "_CFNumberGetValue" to "radek_compat_CFNumberGetValue",
        "_CFDateCreate" to "radek_compat_CFDateCreate",
        "_CFDateGetAbsoluteTime" to "radek_compat_CFDateGetAbsoluteTime",
        "_CFDateGetTimeIntervalSinceDate" to "radek_compat_CFDateGetTimeIntervalSinceDate",
        "_CFAbsoluteTimeGetGregorianDate" to "radek_compat_CFAbsoluteTimeGetGregorianDate",
        "_CFRunLoopGetCurrent" to "radek_compat_CFRunLoopGetCurrent",
        "_CFRunLoopGetMain" to "radek_compat_CFRunLoopGetMain",
        "_CFRunLoopRun" to "radek_compat_CFRunLoopRun",
        "_CFRunLoopRunInMode" to "radek_compat_CFRunLoopRunInMode",
        "_CFRunLoopStop" to "radek_compat_CFRunLoopStop",
        "_CFRunLoopWakeUp" to "radek_compat_CFRunLoopWakeUp",
        "_malloc" to "radek_compat_malloc",
        "_calloc" to "radek_compat_calloc",
        "_realloc" to "radek_compat_realloc",
        "_free" to "radek_compat_free",
        "_memcpy" to "radek_compat_memcpy",
        "_memmove" to "radek_compat_memmove",
        "_memset" to "radek_compat_memset",
        "_memcmp" to "radek_compat_memcmp",
        "_memchr" to "radek_compat_memchr",
        "_strlen" to "radek_compat_strlen",
        "_strcpy" to "radek_compat_strcpy",
        "_strncpy" to "radek_compat_strncpy",
        "_strlcpy" to "radek_compat_strlcpy",
        "_strlcat" to "radek_compat_strlcat",
        "_strcmp" to "radek_compat_strcmp",
        "_strncmp" to "radek_compat_strncmp",
        "_strdup" to "radek_compat_strdup",
        "_strchr" to "radek_compat_strchr",
        "_strrchr" to "radek_compat_strrchr",
        "_strstr" to "radek_compat_strstr",
        "_strtol" to "radek_compat_strtol",
        "_strtod" to "radek_compat_strtod",
        "_atoi" to "radek_compat_atoi",
        "_atof" to "radek_compat_atof",
        "_strerror" to "radek_compat_strerror",
        "_snprintf" to "radek_compat_snprintf",
        "_vsnprintf" to "radek_compat_vsnprintf",
        "_fopen" to "radek_compat_fopen",
        "_fclose" to "radek_compat_fclose",
        "_fread" to "radek_compat_fread",
        "_fwrite" to "radek_compat_fwrite",
        "_fputs" to "radek_compat_fputs",
        "_fgets" to "radek_compat_fgets",
        "_fflush" to "radek_compat_fflush",
        "_fprintf" to "radek_compat_fprintf",
        "_printf" to "radek_compat_printf",
        "_puts" to "radek_compat_puts",
        "_remove" to "radek_compat_remove",
        "_feof" to "radek_compat_feof",
        "_ftell" to "radek_compat_ftell",
        "_fseek" to "radek_compat_fseek",
        "_time" to "radek_compat_time",
        "_gettimeofday" to "radek_compat_gettimeofday",
        "_clock_gettime" to "radek_compat_clock_gettime",
        "_nanosleep" to "radek_compat_nanosleep",
        "_localtime_r" to "radek_compat_localtime_r",
        "_gmtime_r" to "radek_compat_gmtime_r",
        "_mktime" to "radek_compat_mktime",
        "_getenv" to "radek_compat_getenv",
        "_setenv" to "radek_compat_setenv",
        "_unsetenv" to "radek_compat_unsetenv",
        "_getpid" to "radek_compat_getpid",
        "_qsort" to "radek_compat_qsort",
        "_bsearch" to "radek_compat_bsearch",
        "_abs" to "radek_compat_abs",
        "_labs" to "radek_compat_labs",
        "_rand" to "radek_compat_rand",
        "_srand" to "radek_compat_srand",
        "_sqrt" to "radek_compat_sqrt",
        "_fabs" to "radek_compat_fabs",
        "_floor" to "radek_compat_floor",
        "_ceil" to "radek_compat_ceil",
        "_pow" to "radek_compat_pow",
        "_sin" to "radek_compat_sin",
        "_cos" to "radek_compat_cos",
        "_tan" to "radek_compat_tan",
        "_atan2" to "radek_compat_atan2",
        "_fmod" to "radek_compat_fmod",
        "_pthread_mutex_init" to "radek_compat_pthread_mutex_init",
        "_pthread_mutex_lock" to "radek_compat_pthread_mutex_lock",
        "_pthread_mutex_unlock" to "radek_compat_pthread_mutex_unlock",
        "_pthread_mutex_destroy" to "radek_compat_pthread_mutex_destroy",
        "_pthread_cond_init" to "radek_compat_pthread_cond_init",
        "_pthread_cond_wait" to "radek_compat_pthread_cond_wait",
        "_pthread_cond_signal" to "radek_compat_pthread_cond_signal",
        "_pthread_cond_broadcast" to "radek_compat_pthread_cond_broadcast",
        "_pthread_cond_destroy" to "radek_compat_pthread_cond_destroy",
        "_pthread_self" to "radek_compat_pthread_self",
    )

    /**
     * If supplied, the NDK resolver checks public platform exports on this device.
     * The compatibility resolver is separate: it verifies one of our concrete,
     * compiled time shims, but neither resolver rewrites or links the IPA.
     * The compat-handler resolver performs dynamic runtime hook registration:
     * symbols that would otherwise stay unmapped receive an explicit stub handler
     * in libioscompat.so. A stub is a resolution target only; it is classified
     * separately and never counted as a verified implementation.
     */
    fun analyze(
        nodes: JSONArray,
        resolveNdkLibrary: ((String) -> String?)? = null,
        runtimeApiLevel: Int? = null,
        resolveApiReplacement: ((String) -> String?)? = null,
        resolveCompatHandler: ((String) -> String?)? = null,
    ): JSONObject {
        val symbols = linkedSetOf<String>()
        var truncated = false
        outer@ for (nodeIndex in 0 until nodes.length()) {
            val analysis = nodes.optJSONObject(nodeIndex)?.optJSONObject("analysis") ?: continue
            val slices = analysis.optJSONArray("slices") ?: continue
            for (sliceIndex in 0 until slices.length()) {
                val slice = slices.optJSONObject(sliceIndex) ?: continue
                if (slice.optBoolean("importsTruncated", false)) truncated = true
                val imports = slice.optJSONArray("imports") ?: continue
                for (importIndex in 0 until imports.length()) {
                    val item = imports.opt(importIndex)
                    val name = when (item) {
                        is JSONObject -> item.optString("name", "")
                        is String -> item
                        else -> ""
                    }.takeIf { it.isNotBlank() } ?: continue
                    if (name !in symbols && symbols.size >= MAX_SYMBOLS) {
                        truncated = true
                        break@outer
                    }
                    symbols += name
                }
            }
        }

        val result = JSONArray()
        var directCandidates = 0
        var runtimeVerifiedCandidates = 0
        var implementedReplacementCandidates = 0
        var runtimeVerifiedApiReplacements = 0
        var semanticCandidates = 0
        var compilerRuntimeCandidates = 0
        var compatStubHandlers = 0
        var compatVerifiedHandlers = 0
        var unmappedSymbols = 0
        symbols.sorted().forEach { source ->
            // Mach-O C symbols conventionally carry one leading underscore. Remove
            // only that decoration before matching; Objective-C symbols are parsed
            // separately and are never guessed into C ABI-compatible functions.
            val candidate = source.removePrefix("_")
            val catalogLibrary = bionicLibraries.entries.firstOrNull { candidate in it.value }?.key
            val resolvedLibrary = if (resolveNdkLibrary == null) null else try {
                resolveNdkLibrary.invoke(candidate)?.takeIf { it in ndkRuntimeLibraries }
            } catch (_: UnsatisfiedLinkError) {
                null
            } catch (_: RuntimeException) {
                null
            }
            val library = resolvedLibrary ?: catalogLibrary
            val semanticTarget = semanticTarget(source)
            val compilerRuntimeCandidate = compilerRuntimeCandidate(source)
            val replacementTarget = implementedApiReplacements[source]
            val resolvedReplacement = if (replacementTarget == null || resolveApiReplacement == null) null else try {
                resolveApiReplacement.invoke(source)
            } catch (_: UnsatisfiedLinkError) {
                null
            } catch (_: RuntimeException) {
                null
            }
            val replacementVerified = resolvedReplacement == "libioscompat.so:$replacementTarget"
            val direct = library != null
            val verifiedOnDevice = resolvedLibrary != null
            if (direct) directCandidates++
            if (verifiedOnDevice) runtimeVerifiedCandidates++
            if (replacementTarget != null) implementedReplacementCandidates++
            if (replacementVerified) runtimeVerifiedApiReplacements++
            if (compilerRuntimeCandidate != null) compilerRuntimeCandidates++
            if (!direct && compilerRuntimeCandidate == null && replacementTarget == null && semanticTarget != null) semanticCandidates++

            val item = JSONObject()
                .put("sourceSymbol", source)
                .put("linkedOrRewritten", false)
                .put("codeGenerated", false)
            when {
                // Bionic already ships these symbols with the identical C ABI, so
                // the NDK provider wins over the compatibility shim of the same
                // name: a same-name platform export is the stronger evidence.
                direct -> item
                    .put("classification", "BIONIC_SYMBOL_CANDIDATE")
                    .put("targetLibrary", library)
                    .put("targetSymbol", candidate)
                    .put("verifiedOnDevice", verifiedOnDevice)
                    .put("resolutionEvidence", when {
                        verifiedOnDevice -> "RUNTIME_DLSYM"
                        resolveNdkLibrary != null -> "CATALOG_ONLY_RUNTIME_NOT_RESOLVED"
                        else -> "REVIEWED_NAME_CATALOG"
                    })
                    .put("staticRecompilationStrategy", "potential direct NDK symbol link; caller ABI and relocation still require verification")
                    .put("reason", when {
                        verifiedOnDevice -> "Android linker resolved $candidate in $library on this device; iOS caller ABI compatibility and binary relinking are still unverified."
                        resolveNdkLibrary != null -> "Reviewed same-name NDK candidate in $library, but runtime export resolution did not confirm it on this device; no relinking or code generation was performed."
                        else -> "Reviewed same-name Android NDK candidate in $library; no runtime export check, binary relinking or code generation was performed."
                    })
                compilerRuntimeCandidate != null -> item
                    .put("classification", "COMPILER_RUNTIME_CANDIDATE")
                    .put("targetLibrary", "NDK compiler-rt/libunwind toolchain runtime")
                    .put("targetSymbol", candidate)
                    .put("resolutionEvidence", "REVIEWED_TOOLCHAIN_CANDIDATE_NOT_LINKED")
                    .put("staticRecompilationStrategy", "static NDK compiler-rt/libunwind integration required; no libgcc_s.so alias or link was generated")
                    .put("reason", "$compilerRuntimeCandidate. Android NDK does not provide a drop-in libgcc_s.so; symbol ABI and exception personality must be validated before a link can be claimed.")
                replacementTarget != null -> item
                    .put("classification", "IMPLEMENTED_API_REPLACEMENT_AVAILABLE")
                    .put("targetLibrary", "libioscompat.so")
                    .put("targetSymbol", replacementTarget)
                    .put("targetAndroidApi", "libioscompat.so:$replacementTarget")
                    .put("implementationCodePresent", true)
                    .put("runtimeVerified", replacementVerified)
                    .put("resolutionEvidence", when {
                        replacementVerified -> "CURRENT_DEVICE_COMPAT_LIBRARY_DLSYM"
                        resolveApiReplacement != null -> "COMPAT_SOURCE_PRESENT_RUNTIME_NOT_RESOLVED"
                        else -> "COMPILED_COMPATIBILITY_RUNTIME"
                    })
                    .put("staticRecompilationStrategy", "concrete compatibility shim exists; Mach-O callsite rewrite and game linking are not implemented")
                    .put("reason", when {
                        replacementVerified -> "The concrete implementation export $replacementTarget was resolved from libioscompat.so on this device; the IPA callsite was not rewritten or linked."
                        resolveApiReplacement != null -> "A concrete implementation is built into the analyzer runtime, but its export was not resolved on this device; no IPA callsite rewrite or game link was performed."
                        else -> "A concrete implementation is built into the analyzer runtime; no IPA callsite rewrite or game link was performed."
                    })
                semanticTarget != null -> item
                    .put("classification", "SEMANTIC_REWRITE_CANDIDATE")
                    .put("targetApi", semanticTarget)
                    .put("staticRecompilationStrategy", "source/object/lifecycle rewrite required")
                    .put("reason", "Android API family candidate only; Objective-C object layout, method semantics and lifecycle are not binary-compatible.")
                else -> {
                    val compatHandler = if (resolveCompatHandler == null) null else try {
                        resolveCompatHandler.invoke(source)
                    } catch (_: UnsatisfiedLinkError) {
                        null
                    } catch (_: RuntimeException) {
                        null
                    }
                    when {
                        compatHandler?.startsWith("stubbed:") == true -> {
                            compatStubHandlers++
                            val handler = compatHandler.removePrefix("stubbed:")
                            item
                                .put("classification", "COMPAT_STUB_HANDLER_REGISTERED")
                                .put("targetLibrary", "libioscompat.so")
                                .put("targetSymbol", handler)
                                .put("implementationCodePresent", false)
                                .put("staticRecompilationStrategy", "explicit unimplemented resolution handler")
                                .put(
                                    "reason",
                                    "A stub resolution handler for $source was registered in the " +
                                        "libioscompat.so registry. The stub records invocations and " +
                                        "returns a safe default; it does not implement the API and no " +
                                        "IPA callsite was rewritten or linked.",
                                )
                        }
                        compatHandler?.startsWith("verified:") == true -> {
                            compatVerifiedHandlers++
                            val handler = compatHandler.removePrefix("verified:")
                            item
                                .put("classification", "COMPAT_VERIFIED_HANDLER_RESOLVED")
                                .put("targetLibrary", "libioscompat.so")
                                .put("targetSymbol", handler)
                                .put("implementationCodePresent", true)
                                .put("staticRecompilationStrategy", "tested implementation body in the compat registry")
                                .put(
                                    "reason",
                                    "The compat registry resolved $source to a tested implementation " +
                                        "body; no IPA callsite was rewritten or linked.",
                                )
                        }
                        else -> {
                            unmappedSymbols++
                            item
                                .put("classification", "UNMAPPED")
                                .put("reason", classifyUnsupported(source))
                        }
                    }
                }
            }
            result.put(item)
        }
        val total = symbols.size
        val compatHandlers = compatStubHandlers + compatVerifiedHandlers
        val classificationComplete = !truncated && result.length() == total
        val runtimeVerifiedImportCoveragePercent = coveragePercent(runtimeVerifiedCandidates, total)
        val runtimeVerifiedCandidateCoveragePercent = if (resolveNdkLibrary == null) 0
            else coveragePercent(runtimeVerifiedCandidates, directCandidates)
        return JSONObject()
            .put("schemaVersion", 7)
            .put("measure", "Direct NDK name candidates count same-named public NDK/system or shared C++ runtime exports found in the reviewed catalog or current-device lookup. Candidate coverage is divided by all distinct imports; exact runtime export verification is reported both against candidate names and against all imports, with separate denominators. A current-device dlsym hit proves only that the public-library export resolves on this device/API level, not that the iOS caller ABI, relocation, callsite rewrite, or game link is compatible. Compiler-rt/libunwind names are toolchain candidates, not a libgcc_s.so alias or completed link. Runtime compatibility-shim counts identify concrete exports in libioscompat.so, but none proves IPA callsite rewriting or game linking. Classification coverage is triage, not implementation coverage; no count represents playable Android code. Registered compat stub handlers are explicit unimplemented resolution targets; only verified handlers identify tested implementation bodies.")
            .put("runtimeNdkResolverStatus", if (resolveNdkLibrary == null) "NOT_RUN" else "CURRENT_DEVICE_DLSYM")
            .put("runtimeVerifiedAndroidApiLevel", if (resolveNdkLibrary == null) JSONObject.NULL else (runtimeApiLevel ?: JSONObject.NULL))
            .put("runtimeVerifiedNdkCandidates", runtimeVerifiedCandidates)
            .put("runtimeVerifiedCandidateCount", directCandidates)
            .put("runtimeVerifiedCandidateCoveragePercent", runtimeVerifiedCandidateCoveragePercent)
            .put("runtimeVerifiedImportCoveragePercent", runtimeVerifiedImportCoveragePercent)
            // Backwards-compatible field: its denominator is all distinct imports.
            .put("runtimeVerifiedCoveragePercent", runtimeVerifiedImportCoveragePercent)
            .put("runtimeApiReplacementResolverStatus", if (resolveApiReplacement == null) "NOT_RUN" else "CURRENT_DEVICE_COMPAT_DLSYM")
            .put("implementedApiReplacementCount", implementedReplacementCandidates)
            .put("runtimeVerifiedApiReplacementCount", runtimeVerifiedApiReplacements)
            .put("distinctImportSymbols", total)
            .put("classifiedImportSymbols", result.length())
            .put("classificationCoveragePercent", if (total == 0 || !classificationComplete) 0 else 100)
            .put("classificationStatus", if (classificationComplete) "COMPLETE" else "TRUNCATED")
            .put("mappedNameCandidates", directCandidates)
            .put("candidateCoveragePercent", coveragePercent(directCandidates, total))
            .put("semanticRewriteCandidates", semanticCandidates)
            .put("semanticRewriteCoveragePercent", coveragePercent(semanticCandidates, total))
            .put("compilerRuntimeCandidateCount", compilerRuntimeCandidates)
            .put("compilerRuntimeCandidateCoveragePercent", coveragePercent(compilerRuntimeCandidates, total))
            .put("compatStubHandlerCount", compatStubHandlers)
            .put("compatVerifiedHandlerCount", compatVerifiedHandlers)
            .put("compatHandlerCoveragePercent", if (total == 0) 0 else compatHandlers * 100 / total)
            .put("compatHandlerResolverStatus", if (resolveCompatHandler == null) "NOT_RUN" else "DYNAMIC_REGISTRY_REGISTRATION")
            .put("unmappedSymbolCount", unmappedSymbols)
            .put("linkedImplementationCount", 0)
            .put("linkedImplementationCoveragePercent", 0)
            .put("generatedApiImplementationCount", 0)
            .put("truncated", truncated)
            .put("symbols", result)
    }

    /** Round a ratio to the nearest whole percent without overflowing Int. */
    internal fun coveragePercent(numerator: Int, denominator: Int): Int {
        if (denominator <= 0 || numerator <= 0) return 0
        val denominatorLong = denominator.toLong()
        return ((numerator.toLong() * 100L + denominatorLong / 2L) / denominatorLong)
            .toInt()
            .coerceIn(0, 100)
    }

    internal fun compiledCompatibilityProvider(source: String): String? =
        implementedApiReplacements[source]?.let { "libioscompat.so:$it" }

    private fun compilerRuntimeCandidate(source: String): String? {
        val name = source.trimStart('_')
        if (name.startsWith("Unwind_") || name.startsWith("gcc_personality_v0") ||
            name.startsWith("gxx_personality_v0") || name.startsWith("aeabi_unwind_") ||
            name.startsWith("gnu_unwind_")) {
            return "NDK libunwind/libc++abi (unwind ABI candidate; not linked)"
        }
        val builtins = listOf(
            "aeabi_", "divdi3", "udivdi3", "moddi3", "umoddi3", "muldi3", "ashldi3", "ashrdi3",
            "lshrdi3", "udivmoddi4", "divti3", "udivti3", "modti3", "umodti3", "multi3", "muloti4",
            "ashlti3", "ashrti3", "lshrti3", "addvti3", "subvti3", "absvti2", "cmpdi2", "ucmpdi2",
            "clear_cache", "register_frame", "deregister_frame", "fix", "float",
        )
        return if (builtins.any { prefix -> name.startsWith(prefix) }) {
            "NDK compiler-rt builtins (toolchain link candidate; not linked)"
        } else null
    }

    private fun semanticTarget(source: String): String? {
        val markers = listOf("OBJC_CLASS_", "OBJC_METACLASS_")
        for (marker in markers) {
            if (!source.contains(marker)) continue
            val name = source.substringAfter(marker).substringAfterLast('$').removePrefix("_")
            return semanticTargets[name]
        }
        return null
    }

    private fun classifyUnsupported(symbol: String): String = when {
        symbol.contains("objc", ignoreCase = true) -> "Objective-C runtime ABI and message dispatch are not implemented."
        symbol.startsWith("_swift", ignoreCase = true) || symbol.contains("Swift", ignoreCase = true) -> "Swift runtime/ABI static recompilation is not implemented."
        symbol.startsWith("_UI") || symbol.startsWith("_CG") || symbol.startsWith("_CA") || symbol.startsWith("_MTL") ->
            "Apple UI/graphics/Metal APIs require a real Android renderer or object/lifecycle rewrite; none was generated."
        symbol.startsWith("_AV") || symbol.startsWith("_Audio") || symbol.startsWith("_AL") ->
            "Apple audio/video API has no verified Android implementation in this converter."
        else -> "No reviewed Android API mapping exists for this imported symbol."
    }
}
