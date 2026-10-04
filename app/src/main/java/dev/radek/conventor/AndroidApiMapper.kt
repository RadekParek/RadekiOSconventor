package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/**
 * Conservative native-symbol and semantic API equivalence inventory.
 * A candidate is a translation plan only: this module does not rewrite Mach-O
 * code, bridge Objective-C objects, link a Bionic library, or generate Java.
 */
internal object AndroidApiMapper {
    private const val MAX_SYMBOLS = 10_000

    private val ndkRuntimeLibraries = setOf(
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so", "libEGL.so",
        "libGLESv1_CM.so", "libGLESv2.so", "libaaudio.so", "libmediandk.so", "libvulkan.so",
        "libOpenSLES.so", "libOpenMAXAL.so", "libjnigraphics.so", "libbinder_ndk.so", "libamidi.so",
        "libcamera2ndk.so", "libc++_shared.so",
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
        "CADisplayLink" to "android.view.Choreographer",
        "CAAnimation" to "android.animation.Animator",
        "SKScene" to "custom SurfaceView/Canvas renderer (game-loop rewrite required)",
    )

    /** Real, implemented ABI-shaped time wrappers built into the analyzer's libioscompat.so. */
    private val implementedApiReplacements = mapOf(
        "_CFAbsoluteTimeGetCurrent" to "CFAbsoluteTimeGetCurrent",
        "_CACurrentMediaTime" to "CACurrentMediaTime",
        "_mach_absolute_time" to "mach_absolute_time",
        "_mach_timebase_info" to "mach_timebase_info",
    )

    /**
     * If supplied, the NDK resolver checks public platform exports on this device.
     * The compatibility resolver is separate: it verifies one of our concrete,
     * compiled time shims, but neither resolver rewrites or links the IPA.
     */
    fun analyze(
        nodes: JSONArray,
        resolveNdkLibrary: ((String) -> String?)? = null,
        runtimeApiLevel: Int? = null,
        resolveApiReplacement: ((String) -> String?)? = null,
    ): JSONObject {
        val symbols = linkedSetOf<String>()
        var truncated = false
        outer@ for (nodeIndex in 0 until nodes.length()) {
            val analysis = nodes.optJSONObject(nodeIndex)?.optJSONObject("analysis") ?: continue
            val slices = analysis.optJSONArray("slices") ?: continue
            for (sliceIndex in 0 until slices.length()) {
                val imports = slices.optJSONObject(sliceIndex)?.optJSONArray("imports") ?: continue
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
            if (!direct && replacementTarget == null && semanticTarget != null) semanticCandidates++

            val item = JSONObject()
                .put("sourceSymbol", source)
                .put("linkedOrRewritten", false)
                .put("codeGenerated", false)
            when {
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
                    .put("translationStrategy", "real Bionic-backed time shim exists; Mach-O callsite rewrite and game linking are not implemented")
                    .put("reason", when {
                        replacementVerified -> "The concrete implementation export $replacementTarget was resolved from libioscompat.so on this device; the IPA callsite was not rewritten or linked."
                        resolveApiReplacement != null -> "A concrete implementation is built into the analyzer runtime, but its export was not resolved on this device; no IPA callsite rewrite or game link was performed."
                        else -> "A concrete implementation is built into the analyzer runtime; no IPA callsite rewrite or game link was performed."
                    })
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
                    .put("translationStrategy", "potential direct NDK symbol link; caller ABI and relocation still require verification")
                    .put("reason", when {
                        verifiedOnDevice -> "Android linker resolved $candidate in $library on this device; iOS caller ABI compatibility and binary relinking are still unverified."
                        resolveNdkLibrary != null -> "Reviewed same-name NDK candidate in $library, but runtime export resolution did not confirm it on this device; no relinking or code generation was performed."
                        else -> "Reviewed same-name Android NDK candidate in $library; no runtime export check, binary relinking or code generation was performed."
                    })
                semanticTarget != null -> item
                    .put("classification", "SEMANTIC_REWRITE_CANDIDATE")
                    .put("targetApi", semanticTarget)
                    .put("translationStrategy", "source/object/lifecycle rewrite required")
                    .put("reason", "Android API family candidate only; Objective-C object layout, method semantics and lifecycle are not binary-compatible.")
                else -> {
                    unmappedSymbols++
                    item
                        .put("classification", "UNMAPPED")
                        .put("reason", classifyUnsupported(source))
                }
            }
            result.put(item)
        }
        val total = symbols.size
        val classificationComplete = !truncated && result.length() == total
        return JSONObject()
            .put("schemaVersion", 5)
            .put("measure", "Direct candidates are same-named Android NDK/system or shared C++ runtime symbols from the reviewed catalog or exact runtime export lookup. Runtime time-shim counts identify real exports in libioscompat.so, but none proves IPA callsite rewriting or game linking. Caller ABI compatibility, relocation and linking remain separate. Classification coverage is triage, not implementation coverage; no count represents playable Android code.")
            .put("runtimeNdkResolverStatus", if (resolveNdkLibrary == null) "NOT_RUN" else "CURRENT_DEVICE_DLSYM")
            .put("runtimeVerifiedAndroidApiLevel", if (resolveNdkLibrary == null) JSONObject.NULL else (runtimeApiLevel ?: JSONObject.NULL))
            .put("runtimeVerifiedNdkCandidates", runtimeVerifiedCandidates)
            .put("runtimeVerifiedCoveragePercent", if (total == 0) 0 else runtimeVerifiedCandidates * 100 / total)
            .put("runtimeApiReplacementResolverStatus", if (resolveApiReplacement == null) "NOT_RUN" else "CURRENT_DEVICE_COMPAT_DLSYM")
            .put("implementedApiReplacementCount", implementedReplacementCandidates)
            .put("runtimeVerifiedApiReplacementCount", runtimeVerifiedApiReplacements)
            .put("distinctImportSymbols", total)
            .put("classifiedImportSymbols", result.length())
            .put("classificationCoveragePercent", if (total == 0 || !classificationComplete) 0 else 100)
            .put("classificationStatus", if (classificationComplete) "COMPLETE" else "TRUNCATED")
            .put("mappedNameCandidates", directCandidates)
            .put("candidateCoveragePercent", if (total == 0) 0 else directCandidates * 100 / total)
            .put("semanticRewriteCandidates", semanticCandidates)
            .put("semanticRewriteCoveragePercent", if (total == 0) 0 else semanticCandidates * 100 / total)
            .put("unmappedSymbolCount", unmappedSymbols)
            .put("implementedTranslationCount", 0)
            .put("implementedTranslationCoveragePercent", 0)
            .put("generatedTranslationCount", 0)
            .put("truncated", truncated)
            .put("symbols", result)
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
        symbol.startsWith("_swift", ignoreCase = true) || symbol.contains("Swift", ignoreCase = true) -> "Swift runtime/ABI translation is not implemented."
        symbol.startsWith("_UI") || symbol.startsWith("_CG") || symbol.startsWith("_CA") || symbol.startsWith("_MTL") ->
            "Apple UI/graphics/Metal APIs require a real Android renderer or object/lifecycle rewrite; none was generated."
        symbol.startsWith("_AV") || symbol.startsWith("_Audio") || symbol.startsWith("_AL") ->
            "Apple audio/video API has no verified Android implementation in this converter."
        else -> "No reviewed Android API mapping exists for this imported symbol."
    }
}
