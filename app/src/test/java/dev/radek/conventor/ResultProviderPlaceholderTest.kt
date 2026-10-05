package dev.radek.conventor

import android.content.pm.ProviderInfo
import android.net.Uri
import android.os.ParcelFileDescriptor
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.io.File
import java.security.MessageDigest

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class ResultProviderPlaceholderTest {
    @Test fun servesOnlyTheExplicitPlaceholderNameUnderItsOwnMetadataContract() {
        val context = RuntimeEnvironment.getApplication()
        val directory = File(context.filesDir, "library/1-12345678-1234-1234-1234-123456789abc").apply { mkdirs() }
        val provider = ResultProvider()
        provider.attachInfo(context, ProviderInfo().apply {
            authority = "dev.radek.conventor.results"
            exported = false
            grantUriPermissions = true
        })
        try {
            val report = makeReport(directory)
            File(directory, "report.json").writeText(report.toString())
            val placeholderName = ArtifactNames.placeholderApkFileName(report)
            val uri = Uri.Builder().scheme("content").authority("dev.radek.conventor.results")
                .appendPath(directory.name).appendPath(placeholderName).build()

            val descriptor = provider.openFile(uri, "r")
            val contents = ParcelFileDescriptor.AutoCloseInputStream(descriptor).use { it.readBytes() }
            assertEquals("verified-placeholder-fixture", String(contents))
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test fun placeholderCannotBeServedAtTheCompleteGameHostPath() {
        val context = RuntimeEnvironment.getApplication()
        val directory = File(context.filesDir, "library/2-12345678-1234-1234-1234-123456789abc").apply { mkdirs() }
        val provider = ResultProvider()
        provider.attachInfo(context, ProviderInfo().apply {
            authority = "dev.radek.conventor.results"
            exported = false
            grantUriPermissions = true
        })
        try {
            val report = makeReport(directory)
            val hostName = ArtifactNames.apkFileName(report)
            File(directory, hostName).writeText("placeholder copy")
            report.put("hostConversion", JSONObject()
                .put("status", "ATTACHED")
                .put("completeGameConversion", true)
                .put("contract", "placeholder-v1")
                .put("artifact", hostName))
            File(directory, "report.json").writeText(report.toString())
            val uri = Uri.Builder().scheme("content").authority("dev.radek.conventor.results")
                .appendPath(directory.name).appendPath(hostName).build()
            assertThrows(IllegalArgumentException::class.java) { provider.openFile(uri, "r") }
        } finally {
            directory.deleteRecursively()
        }
    }

    private fun makeReport(directory: File): JSONObject {
        val content = "verified-placeholder-fixture".toByteArray()
        val sourceHash = "b".repeat(64)
        val report = JSONObject()
            .put("source", JSONObject().put("sha256", sourceHash).put("originalName", "Test.ipa"))
            .put("application", JSONObject().put("sha256", sourceHash))
        val fileName = ArtifactNames.placeholderApkFileName(report)
        File(directory, fileName).writeBytes(content)
        val digest = MessageDigest.getInstance("SHA-256").digest(content)
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        val certificateHash = "d".repeat(64)
        report.put("placeholderConversion", JSONObject()
            .put("status", "GENERATED")
            .put("placeholderOnly", true)
            .put("completeGameConversion", false)
            .put("gameCodeIncluded", false)
            .put("gameCodeRecompiled", false)
            .put("gamePlayable", false)
            .put("installableAndroidPackage", true)
            .put("artifact", fileName)
            .put("package", "dev.radek.placeholder.p${sourceHash.take(24)}${certificateHash.take(8)}")
            .put("sourceSha256", sourceHash)
            .put("sha256", digest)
            .put("signing", JSONObject().put("certificateSha256", certificateHash)))
        return report
    }
}
