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

    /** Kept separate from the strict complete-game APK artifact and provider path. */
    fun placeholderApkFileName(report: JSONObject): String =
        apkFileName(report).removeSuffix(".apk") + "-placeholder.apk"
}
