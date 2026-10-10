package dev.radek.conventor

import org.json.JSONObject

/**
 * Publishes Android-linked code-byte progress for a game-runtime APK.
 *
 * Kept separate from the APK builder so the progress rules can be unit tested
 * without an Android Context.
 */
internal object TranslatedAndroidLink {
    /** Status written only when the packaged APK contains the verified translated library. */
    const val STATUS = "VERIFIED_ANDROID_APK_LIBRARY"
    private const val ABI = "arm64-v8a"

    /**
     * Writes the Android-linked code-byte progress for the library entry.
     *
     * Progress is recorded only when the packaged game-runtime APK actually
     * contains the translated ARM64 library. Its coverage comes from the same
     * portable-C metadata that the input validator bound to that library (it
     * requires the link report and the translation report to agree on function
     * and byte counts), so nothing here is taken from an unverified claim. When
     * a later build has no usable translated library, a previously recorded link
     * is reset so the card cannot keep showing progress for an APK that no
     * longer contains the code. This never throws: the APK is already installed
     * by the time it is called.
     */
    fun record(report: JSONObject, translated: TranslatedGameRuntimeInput.Staged?) {
        val metadata = translated?.metadata
        val linkedBytes = metadata?.optLong("translatedUniqueTextBytes", 0) ?: 0L
        val executableBytes = metadata?.optLong("executableTextBytes", 0) ?: 0L
        val functions = metadata?.optInt("translatedFunctionCount", 0) ?: 0
        val consistent = metadata != null && functions > 0 && executableBytes > 0 &&
            linkedBytes in 0L..executableBytes
        if (translated == null || metadata == null || !consistent) {
            resetIfPreviouslyLinked(report)
            return
        }
        val percent = Math.round(100.0 * linkedBytes / executableBytes * 1_000_000.0) / 1_000_000.0
        val artifact = metadata.optString("libraryApkPath")
        val basis = "$functions translated function(s) are packaged as $artifact inside the game-runtime APK " +
            "($linkedBytes/$executableBytes unique source __text bytes). The verified ELF exports, " +
            "dependency allowlist, and JNI entry were revalidated on device; the translated runner has no " +
            "EGL/GLES renderer, so pixels, gameplay, and playability are not verified."
        report.put(
            "androidLink",
            JSONObject()
                .put("status", STATUS)
                .put("attempted", true)
                .put("ndkLinkVerified", translated.verification.optBoolean("ndkLinkVerified", false))
                .put("targetAbi", ABI)
                .put("artifact", artifact)
                .put("artifactSha256", metadata.optString("androidLibrarySha256"))
                .put("architectureVerified", metadata.optBoolean("architectureVerified", false))
                .put("dependenciesVerified", metadata.optBoolean("dependenciesVerified", false))
                .put("exportsVerified", metadata.optBoolean("exportsVerified", false))
                .put("translationEntryPointVerified", metadata.optBoolean("translationEntryPointVerified", false))
                .put("allVerificationsPassed", metadata.optBoolean("allVerificationsPassed", false))
                .put("translatedFunctionCount", functions)
                .put("linkedTranslatedFunctionCount", functions)
                .put("androidLinkedTextBytes", linkedBytes)
                .put("executableTextBytes", executableBytes)
                .put("androidLinkedTextPercent", percent)
                .put("linkedIntoGame", true)
                .put("apkProduced", true)
                .put("completeGameConversion", false)
                .put("gameplayVerified", false)
                .put("basis", basis),
        )
        report.put(
            "portProgress",
            JSONObject()
                .put("percent", percent)
                .put("status", "PARTIAL_ANDROID_APK_TRANSLATED_CODE_LINKED")
                .put(
                    "metric",
                    "unique successfully translated __text instruction bytes whose generated function symbols " +
                        "are packaged in the verified Android library of the game-runtime APK / executable __text bytes",
                )
                .put("androidLinkVerified", true)
                .put("recompiledFunctions", functions)
                .put("totalTranslatedFunctions", functions)
                .put("recompiledTextBytes", linkedBytes)
                .put("totalTextBytes", executableBytes)
                .put("androidArtifact", artifact)
                .put("targetAbi", ABI)
                .put("linkedIntoGame", true)
                .put("linkedIntoAndroidSharedLibrary", true)
                .put("apkProduced", true)
                .put("completeGameConversion", false)
                .put("basis", basis),
        )
    }

    private fun resetIfPreviouslyLinked(report: JSONObject) {
        if (report.optJSONObject("androidLink")?.optString("status") != STATUS) return
        report.put(
            "androidLink",
            JSONObject()
                .put("status", "NOT_ATTEMPTED")
                .put("androidLinkedTextBytes", 0)
                .put("androidLinkedTextPercent", 0)
                .put("linkedIntoGame", false)
                .put("apkProduced", false)
                .put("completeGameConversion", false),
        )
        report.put(
            "portProgress",
            JSONObject()
                .put("percent", 0)
                .put("status", "NO_ANDROID_LINK_VERIFIED")
                .put("androidLinkVerified", false)
                .put("linkedIntoGame", false)
                .put("basis", "The most recent game-runtime APK was built without a verified translated library."),
        )
    }
}
