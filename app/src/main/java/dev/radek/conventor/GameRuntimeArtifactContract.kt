package dev.radek.conventor

import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

/** Separate metadata and path contract for game-runtime boot-attempt APKs (contract "game-runtime-v1"). */
internal object GameRuntimeArtifactContract {
    const val CONTRACT = "game-runtime-v1"
    const val PACKAGE_PREFIX = "dev.radek.gameruntime.p"

    fun validate(report: JSONObject, directory: File, requestedName: String): File {
        val conversion = report.optJSONObject("gameRuntimeConversion")
            ?: error("no game-runtime APK is recorded")
        require(conversion.optString("status") == "GENERATED") { "game-runtime APK is not ready" }
        require(conversion.optString("contract") == CONTRACT) { "result is not a game-runtime APK" }
        require(conversion.optBoolean("bootAttemptIncluded", false)) { "game-runtime APK does not contain a boot attempt" }
        require(!conversion.optBoolean("completeGameConversion", true)) { "game-runtime APK cannot be a complete-game conversion" }
        require(!conversion.optBoolean("gamePlayable", true)) { "game-runtime APK cannot claim gameplay" }
        require(!conversion.optBoolean("gameCodeRecompiled", true)) {
            "game-runtime metadata must not claim statically recompiled game code"
        }
        require(conversion.optBoolean("installableAndroidPackage", false)) { "game-runtime package was not verified for installation" }

        val expectedName = ArtifactNames.gameApkFileName(report)
        require(requestedName == expectedName && conversion.optString("artifact") == expectedName) {
            "unsupported game-runtime result name"
        }
        val sourceHash = report.optJSONObject("source")?.optString("sha256").orEmpty()
        val appHash = report.optJSONObject("application")?.optString("sha256").orEmpty()
        require(sourceHash.matches(Regex("[0-9a-f]{64}")) && sourceHash == appHash && conversion.optString("sourceSha256") == sourceHash) {
            "game-runtime APK does not match the analyzed IPA"
        }
        val signing = conversion.optJSONObject("signing") ?: error("game-runtime signing identity metadata is missing")
        val certificateHash = signing.optString("certificateSha256")
        require(certificateHash.matches(Regex("[0-9a-f]{64}"))) { "game-runtime signer fingerprint is invalid" }
        val expectedPackage = ArtifactNames.androidPackageName(
            report.optJSONObject("application")?.optString("bundleId").orEmpty(),
            sourceHash,
            certificateHash)
        require(conversion.optString("package") == expectedPackage) { "game-runtime package id does not match its IPA and signer" }
        val expectedDigest = conversion.optString("sha256")
        require(expectedDigest.matches(Regex("[0-9a-f]{64}"))) { "game-runtime APK digest is missing" }

        val root = directory.canonicalFile
        val result = File(directory, expectedName).canonicalFile
        require(result.path.startsWith(root.path + File.separator) && result.isFile) { "game-runtime APK file is missing" }
        require(sha256(result) == expectedDigest) { "game-runtime APK digest does not match its report" }
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
