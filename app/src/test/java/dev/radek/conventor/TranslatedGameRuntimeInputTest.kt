package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class TranslatedGameRuntimeInputTest {
    private val entryPoint = "Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame"
    private val sourceHash = "a".repeat(64)

    @Test
    fun verifiedArm64HandoffIsStagedAndBoundToTheSelectedExecutable() {
        val root = Files.createTempDirectory("translated-runtime-input").toFile()
        try {
            val input = createInputBundle(root, sourceHash)
            val staged = File(root, "staged")

            val result = requireNotNull(
                TranslatedGameRuntimeInput.stage(input, staged, sourceHash),
            )

            assertTrue(result.library.isFile)
            assertTrue(result.payload.isFile)
            assertTrue(result.metadata.optBoolean("architectureVerified"))
            assertTrue(result.metadata.optBoolean("dependenciesVerified"))
            assertTrue(result.metadata.optBoolean("exportsVerified"))
            assertTrue(result.metadata.optBoolean("translationEntryPointVerified"))
            assertEquals(entryPoint, result.metadata.optString("entryPointSymbol"))
            assertFalse(result.metadata.optBoolean("gameplayVerified"))
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun mismatchedExecutableHashIsRejectedBeforeStaging() {
        val root = Files.createTempDirectory("translated-runtime-mismatch").toFile()
        try {
            val input = createInputBundle(root, sourceHash)
            val error = runCatching {
                TranslatedGameRuntimeInput.stage(input, File(root, "staged"), "b".repeat(64))
            }.exceptionOrNull()
            assertTrue(error is IllegalArgumentException)
        } finally {
            root.deleteRecursively()
        }
    }

    private fun createInputBundle(root: File, executableHash: String): File {
        val library = ConvertedElfWriter.buildSharedObject(
            byteArrayOf(0x1f, 0x20, 0x03, 0xd5.toByte()),
            entryPoint,
        )
        val librarySha = sha256(library)
        val memory = "translated memory".toByteArray()
        val memorySha = sha256(memory)
        val reportBytes = JSONObject()
            .put("sourceExecutableSha256", executableHash)
            .put("translationEntryPointSymbol", entryPoint)
            .put("functions", 1)
            .put("functionFailures", 0)
            .put("translatedFunctionSymbols", JSONArray().put(entryPoint))
            .put("translatedUniqueTextBytes", 4)
            .put("executableTextBytes", 16)
            .toString().toByteArray()
        val reportSha = sha256(reportBytes)
        val payloadManifest = JSONObject()
            .put("contract", "translated-game-payload-v1")
            .put("schemaVersion", 1)
            .put("targetAbi", "arm64-v8a")
            .put("entryPointSymbol", entryPoint)
            .put("sourceExecutableSha256", executableHash)
            .put("translationReportSha256", reportSha)
            .put("androidLibrarySha256", librarySha)
            .put("translatedFunctionCount", 1)
            .put("translatedUniqueTextBytes", 4)
            .put("executableTextBytes", 16)
            .put("memoryImage", JSONObject()
                .put("name", "rt_mem.bin")
                .put("sizeBytes", memory.size)
                .put("sha256", memorySha))
            .toString().toByteArray()
        val payload = zipOf(mapOf("manifest.json" to payloadManifest, "rt_mem.bin" to memory))
        val payloadSha = sha256(payload)
        val linkReportBytes = JSONObject()
            .put("status", "VERIFIED_ANDROID_SHARED_LIBRARY")
            .put("attempted", true)
            .put("ndkLinkVerified", true)
            .put("targetAbi", "arm64-v8a")
            .put("elfArchitecture", "arm64-v8a")
            .put("elfClass", 64)
            .put("architectureVerified", true)
            .put("dependenciesVerified", true)
            .put("exportsVerified", true)
            .put("translationEntryPointVerified", true)
            .put("allVerificationsPassed", true)
            .put("entryPointSymbol", entryPoint)
            .put("artifactSha256", librarySha)
            .put("artifactSizeBytes", library.size)
            .put("dynamicDependencies", JSONArray())
            .put("translatedFunctionCount", 1)
            .put("linkedTranslatedFunctionCount", 1)
            .put("translatedUniqueTextBytes", 4)
            .put("executableTextBytes", 16)
            .toString().toByteArray()

        val files = JSONObject()
            .put("libtranslated_game.so", record(library))
            .put("translated-game-payload.zip", record(payload))
            .put("android-link-report.json", record(linkReportBytes))
            .put("rt_report.json", record(reportBytes))
        val inputManifest = JSONObject()
            .put("contract", "translated-game-runtime-input-v1")
            .put("schemaVersion", 1)
            .put("targetAbi", "arm64-v8a")
            .put("sourceExecutableSha256", executableHash)
            .put("entryPointSymbol", entryPoint)
            .put("files", files)
            .put("linkVerification", JSONObject()
                .put("status", "VERIFIED_ANDROID_SHARED_LIBRARY")
                .put("architectureVerified", true)
                .put("dependenciesVerified", true)
                .put("exportsVerified", true)
                .put("translationEntryPointVerified", true)
                .put("allVerificationsPassed", true))
            .toString().toByteArray()
        val input = File(root, "input.zip")
        input.writeBytes(zipOf(mapOf(
            "manifest.json" to inputManifest,
            "libtranslated_game.so" to library,
            "translated-game-payload.zip" to payload,
            "android-link-report.json" to linkReportBytes,
            "rt_report.json" to reportBytes,
        )))
        return input
    }

    private fun record(bytes: ByteArray): JSONObject = JSONObject()
        .put("sizeBytes", bytes.size)
        .put("sha256", sha256(bytes))

    private fun zipOf(entries: Map<String, ByteArray>): ByteArray {
        val output = ByteArrayOutputStream()
        ZipOutputStream(output).use { zip ->
            entries.forEach { (name, bytes) ->
                zip.putNextEntry(ZipEntry(name))
                zip.write(bytes)
                zip.closeEntry()
            }
        }
        return output.toByteArray()
    }

    private fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
        .digest(bytes).joinToString("") { "%02x".format(it.toInt() and 0xff) }
}
