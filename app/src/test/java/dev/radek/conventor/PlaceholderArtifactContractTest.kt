package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.nio.file.Files
import java.security.MessageDigest

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class PlaceholderArtifactContractTest {
    @Test fun validatesOnlyTheExplicitPlaceholderArtifactAndItsDigest() {
        val directory = Files.createTempDirectory("placeholder-result").toFile()
        try {
            val report = reportFor(directory)
            val name = ArtifactNames.placeholderApkFileName(report)
            assertEquals("Example-preview.apk", name)
            assertEquals(name, PlaceholderArtifactContract.validate(report, directory, name).name)
            assertThrows(IllegalArgumentException::class.java) {
                PlaceholderArtifactContract.validate(report, directory, ArtifactNames.apkFileName(report))
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test fun refusesModifiedOrMisrepresentedPlaceholderFiles() {
        val directory = Files.createTempDirectory("placeholder-result").toFile()
        try {
            val report = reportFor(directory)
            val name = ArtifactNames.placeholderApkFileName(report)
            directory.resolve(name).writeText("changed")
            assertThrows(IllegalArgumentException::class.java) {
                PlaceholderArtifactContract.validate(report, directory, name)
            }

            directory.resolve(name).writeBytes(placeholderBytes)
            report.getJSONObject("placeholderConversion").put("completeGameConversion", true)
            assertThrows(IllegalArgumentException::class.java) {
                PlaceholderArtifactContract.validate(report, directory, name)
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    private fun reportFor(directory: java.io.File): JSONObject {
        val hash = "a".repeat(64)
        val report = JSONObject()
            .put("source", JSONObject().put("sha256", hash).put("originalName", "Example.ipa"))
            .put("application", JSONObject().put("sha256", hash))
        val name = ArtifactNames.placeholderApkFileName(report)
        directory.resolve(name).writeBytes(placeholderBytes)
        val digest = MessageDigest.getInstance("SHA-256").digest(placeholderBytes)
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        val certificateHash = "c".repeat(64)
        report.put("placeholderConversion", JSONObject()
            .put("status", "GENERATED")
            .put("placeholderOnly", true)
            .put("completeGameConversion", false)
            .put("gameCodeTranslated", false)
            .put("gameCodeIncluded", false)
            .put("gamePlayable", false)
            .put("installableAndroidPackage", true)
            .put("artifact", name)
            .put("package", "dev.radek.placeholder.p${hash.take(24)}${certificateHash.take(8)}")
            .put("sourceSha256", hash)
            .put("sha256", digest)
            .put("signing", JSONObject().put("certificateSha256", certificateHash)))
        return report
    }

    private val placeholderBytes = "signed-APK-placeholder-test-fixture".toByteArray()
}
