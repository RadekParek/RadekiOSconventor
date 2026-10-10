package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class TranslatedAndroidLinkTest {
    private fun staged(linked: Long, executable: Long, functions: Int) = TranslatedGameRuntimeInput.Staged(
        library = File("libtranslated_game.so"),
        payload = File("translated-game-payload.zip"),
        metadata = JSONObject()
            .put("translatedUniqueTextBytes", linked)
            .put("executableTextBytes", executable)
            .put("translatedFunctionCount", functions)
            .put("libraryApkPath", "lib/arm64-v8a/libtranslated_game.so")
            .put("androidLibrarySha256", "b".repeat(64))
            .put("architectureVerified", true)
            .put("dependenciesVerified", true)
            .put("exportsVerified", true)
            .put("translationEntryPointVerified", true)
            .put("allVerificationsPassed", true),
        verification = JSONObject().put("ndkLinkVerified", true),
        report = JSONObject(),
    )

    @Test
    fun packagedTranslatedLibraryRecordsVerifiedCodeByteProgress() {
        val report = JSONObject()
        TranslatedAndroidLink.record(report, staged(linked = 1_215_020L, executable = 1_243_432L, functions = 2837))

        val link = report.getJSONObject("androidLink")
        assertEquals(TranslatedAndroidLink.STATUS, link.getString("status"))
        assertTrue(link.getBoolean("architectureVerified"))
        assertTrue(link.getBoolean("allVerificationsPassed"))
        assertTrue(link.getBoolean("linkedIntoGame"))
        assertFalse(link.getBoolean("completeGameConversion"))
        assertEquals(1_215_020L, link.getLong("androidLinkedTextBytes"))
        assertEquals(97.715034, link.getDouble("androidLinkedTextPercent"), 1e-6)

        val port = report.getJSONObject("portProgress")
        assertEquals(97.715034, port.getDouble("percent"), 1e-6)
        assertTrue(port.getBoolean("androidLinkVerified"))
        assertEquals(2837, port.getInt("recompiledFunctions"))
    }

    @Test
    fun inconsistentCoverageNeverRecordsProgress() {
        val report = JSONObject()
        TranslatedAndroidLink.record(report, staged(linked = 10L, executable = 4L, functions = 1))
        assertEquals(null, report.optJSONObject("androidLink"))
        assertEquals(null, report.optJSONObject("portProgress"))
    }

    @Test
    fun rebuildWithoutTranslatedLibraryResetsPreviouslyRecordedLink() {
        val report = JSONObject()
        TranslatedAndroidLink.record(report, staged(linked = 50L, executable = 100L, functions = 3))
        TranslatedAndroidLink.record(report, null)

        assertEquals("NOT_ATTEMPTED", report.getJSONObject("androidLink").getString("status"))
        assertEquals(0.0, report.getJSONObject("portProgress").getDouble("percent"), 0.0)
        assertFalse(report.getJSONObject("androidLink").getBoolean("apkProduced"))
    }

    @Test
    fun unrelatedAndroidLinkStatusIsLeftAlone() {
        val report = JSONObject().put("androidLink", JSONObject().put("status", "BLOCKED_NO_ANDROID_NDK"))
        TranslatedAndroidLink.record(report, null)
        assertEquals("BLOCKED_NO_ANDROID_NDK", report.getJSONObject("androidLink").getString("status"))
    }
}
