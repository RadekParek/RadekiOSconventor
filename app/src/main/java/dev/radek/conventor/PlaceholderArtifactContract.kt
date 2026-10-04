package dev.radek.conventor

import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

/** Separate metadata and path contract for explicitly non-playable placeholder APKs. */
internal object PlaceholderArtifactContract {
    fun validate(report: JSONObject, directory: File, requestedName: String): File {
        val conversion = report.optJSONObject("placeholderConversion")
            ?: error("no placeholder APK is recorded")
        require(conversion.optString("status") == "GENERATED") { "placeholder APK is not ready" }
        require(conversion.optBoolean("placeholderOnly", false)) { "result is not a placeholder APK" }
        require(!conversion.optBoolean("completeGameConversion", true)) { "placeholder cannot be a complete-game conversion" }
        require(!conversion.optBoolean("gameCodeIncluded", true) && !conversion.optBoolean("gameCodeTranslated", true)) {
            "placeholder metadata must not claim translated game code"
        }
        require(!conversion.optBoolean("gamePlayable", true)) { "placeholder cannot claim gameplay" }
        require(conversion.optBoolean("installableAndroidPackage", false)) { "placeholder package was not verified for installation" }

        val expectedName = ArtifactNames.placeholderApkFileName(report)
        require(requestedName == expectedName && conversion.optString("artifact") == expectedName) {
            "unsupported placeholder result name"
        }
        val sourceHash = report.optJSONObject("source")?.optString("sha256").orEmpty()
        val appHash = report.optJSONObject("application")?.optString("sha256").orEmpty()
        require(sourceHash.matches(Regex("[0-9a-f]{64}")) && sourceHash == appHash && conversion.optString("sourceSha256") == sourceHash) {
            "placeholder does not match the analyzed IPA"
        }
        val signing = conversion.optJSONObject("signing") ?: error("placeholder signing identity metadata is missing")
        val certificateHash = signing.optString("certificateSha256")
        require(certificateHash.matches(Regex("[0-9a-f]{64}"))) { "placeholder signer fingerprint is invalid" }
        val expectedPackage = "dev.radek.placeholder.p${sourceHash.take(24)}${certificateHash.take(8)}"
        require(conversion.optString("package") == expectedPackage) { "placeholder package id does not match its IPA and signer" }
        val expectedDigest = conversion.optString("sha256")
        require(expectedDigest.matches(Regex("[0-9a-f]{64}"))) { "placeholder APK digest is missing" }

        val root = directory.canonicalFile
        val result = File(directory, expectedName).canonicalFile
        require(result.path.startsWith(root.path + File.separator) && result.isFile) { "placeholder APK file is missing" }
        require(sha256(result) == expectedDigest) { "placeholder APK digest does not match its report" }
        return result
    }

    private fun sha256(file: File): String = file.inputStream().use { input ->
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(65536)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            digest.update(buffer, 0, count)
        }
        digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
