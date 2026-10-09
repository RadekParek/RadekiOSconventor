package dev.radek.conventor

import org.json.JSONObject

/** Safe APK filename derived from the selected IPA, shared by import, attachment and provider paths. */
internal object ArtifactNames {
    fun apkFileName(report: JSONObject): String {
        val original = report.optJSONObject("source")?.optString("originalName")?.takeIf { it.isNotBlank() }
            ?: report.optJSONObject("application")?.optString("name")?.takeIf { it.isNotBlank() }
            ?: "ConvertedIPA"
        val basename = original.replace('\\', '/').substringAfterLast('/')
        val stem = if (basename.endsWith(".ipa", ignoreCase = true)) basename.dropLast(4)
            else basename.substringBeforeLast('.', basename)
        val safe = stem.map { ch -> if (ch.isLetterOrDigit() || ch in " ._-") ch else '_' }
            .joinToString("").trim(' ', '.', '_', '-')
            .take(80).trim(' ', '.', '_', '-')
            .ifBlank { "ConvertedIPA" }
        return "$safe.apk"
    }

    /**
     * Non-playable preview shell name. Kept separate from the strict
     * complete-game APK artifact and provider path; the word "placeholder" is
     * deliberately absent from every user-visible artifact name.
     */
    fun placeholderApkFileName(report: JSONObject): String =
        apkFileName(report).removeSuffix(".apk") + "-preview.apk"

    /**
     * Boot-attempt game APK name (contract "game-runtime-v1"). Kept separate
     * from both the strict complete-game APK and the non-playable preview
     * shell so provider/install/share paths can validate each by exact name.
     */
    fun gameApkFileName(report: JSONObject): String =
        apkFileName(report).removeSuffix(".apk") + "-game.apk"

    // ---- Android package id, version code and expansion OBB ----------------

    /**
     * Android package id derived from the game's own bundle identifier, so a
     * converted game is installed and addressed under the id it shipped with
     * (`com.clickgamer.AngryBirds`) instead of a converter-generated hash.
     *
     * Android requires at least two dot-separated segments, each starting with
     * a letter. An IPA whose bundle id cannot satisfy that falls back to the
     * previous hash-derived id rather than producing an uninstallable package.
     */
    fun androidPackageName(bundleId: String, sourceHash: String, certificateHash: String): String {
        val sanitized = bundleId
            .map { ch -> if (ch.isLetterOrDigit() || ch == '.' || ch == '_') ch else '.' }
            .joinToString("")
            .trim('.')
        val segments = sanitized.split('.').filter { it.isNotBlank() }
        val usable = segments.size >= 2 &&
            segments.all { it.first().isLetter() } &&
            sanitized.length <= 127
        return if (usable) {
            sanitized
        } else {
            "${GameRuntimeArtifactContract.PACKAGE_PREFIX}${sourceHash.take(20)}${certificateHash.take(8)}"
        }
    }

    /**
     * Positive integer version code derived from `CFBundleVersion`, used in the
     * expansion-file name. The first three dot-separated components are packed
     * as major*10000 + minor*100 + patch (`1.0` -> 10000, `1.4` -> 10400,
     * `1.4.2` -> 10402), so it is monotonic in the original version and stable
     * across rebuilds of the same IPA. A blank or unparsable version yields 1.
     */
    fun versionCodeOf(bundleVersion: String): Int {
        val parts = bundleVersion.trim().split('.')
        var code = 0
        for (index in 0 until 3) {
            val value = parts.getOrNull(index)
                ?.takeWhile { it.isDigit() }
                ?.toIntOrNull()
                ?.coerceIn(0, 99)
                ?: 0
            code = code * 100 + value
        }
        return if (code <= 0) 1 else code
    }

    /**
     * Platform-standard expansion-file name, e.g.
     * `main.1.com.clickgamer.AngryBirds.obb`, which is what Android's own OBB
     * resolution (`Context.getObbDir()`) expects to find.
     */
    fun obbFileName(packageName: String, versionCode: Int): String =
        "main.$versionCode.$packageName.obb"
}
