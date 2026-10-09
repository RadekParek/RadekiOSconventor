package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.Collections
import java.util.zip.ZipFile

/** Validates and stages the host-produced portable-C handoff for the game APK builder. */
internal object TranslatedGameRuntimeInput {
    private const val INPUT_CONTRACT = "translated-game-runtime-input-v1"
    private const val PAYLOAD_CONTRACT = "translated-game-payload-v1"
    private const val ENTRY_SYMBOL = "Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame"
    private const val ABI = "arm64-v8a"
    private const val LIBRARY = "libtranslated_game.so"
    private const val PAYLOAD = "translated-game-payload.zip"
    private const val MAX_INPUT_BYTES = 1024L * 1024 * 1024
    private const val MAX_LIBRARY_BYTES = 128L * 1024 * 1024
    private const val MAX_PAYLOAD_BYTES = 512L * 1024 * 1024
    private const val MAX_REPORT_BYTES = 16L * 1024 * 1024
    private const val MAX_MANIFEST_BYTES = 1024L * 1024
    private val ALLOWED_NEEDED = setOf("libc.so", "libm.so", "libdl.so")

    data class Staged(
        val library: File,
        val payload: File,
        val metadata: JSONObject,
        val verification: JSONObject,
        val report: JSONObject,
    )

    /** Returns null when no translated handoff was supplied; malformed input throws. */
    fun stage(input: File, destination: File, expectedExecutableSha256: String): Staged? {
        if (!input.exists()) return null
        require(input.isFile && input.length() in 1L..MAX_INPUT_BYTES) {
            "translated game-runtime input bundle is missing, empty, or too large"
        }
        require(expectedExecutableSha256.matches(Regex("[0-9a-f]{64}"))) {
            "selected ARM executable SHA-256 is invalid"
        }
        require(destination.isDirectory || destination.mkdirs()) {
            "cannot create translated-runtime staging directory"
        }

        val expectedNames = setOf(
            "manifest.json", LIBRARY, PAYLOAD, "android-link-report.json", "rt_report.json",
        )
        val extracted = LinkedHashMap<String, File>()
        ZipFile(input).use { archive ->
            val entries = Collections.list(archive.entries())
            require(entries.size == expectedNames.size && entries.map { it.name }.toSet() == expectedNames) {
                "translated game-runtime input ZIP has unexpected or duplicate entries"
            }
            require(entries.none { it.isDirectory }) { "translated game-runtime input ZIP contains a directory entry" }
            entries.forEach { SafeZip.validateName(it.name) }

            val outerManifestBytes = readEntry(archive, "manifest.json", MAX_MANIFEST_BYTES)
            val outerManifest = JSONObject(String(outerManifestBytes, Charsets.UTF_8))
            require(outerManifest.optString("contract") == INPUT_CONTRACT &&
                outerManifest.optInt("schemaVersion", 0) == 1) {
                "translated game-runtime input contract is unsupported"
            }
            require(outerManifest.optString("targetAbi") == ABI) {
                "translated game-runtime input ABI does not match the ARM64 game-runtime APK"
            }
            require(outerManifest.optString("entryPointSymbol") == ENTRY_SYMBOL &&
                outerManifest.optString("sourceExecutableSha256") == expectedExecutableSha256) {
                "translated runtime input is not bound to this IPA executable and GameBootActivity entry point"
            }
            val bundledVerification = outerManifest.optJSONObject("linkVerification")
                ?: error("translated input is missing Android link verification")
            require(bundledVerification.optString("status") == "VERIFIED_ANDROID_SHARED_LIBRARY" &&
                bundledVerification.optBoolean("architectureVerified", false) &&
                bundledVerification.optBoolean("dependenciesVerified", false) &&
                bundledVerification.optBoolean("exportsVerified", false) &&
                bundledVerification.optBoolean("translationEntryPointVerified", false) &&
                bundledVerification.optBoolean("allVerificationsPassed", false)) {
                "translated Android architecture/class, dependency, export, or JNI-entry verification did not pass"
            }

            val fileManifest = outerManifest.optJSONObject("files")
                ?: error("translated game-runtime input is missing its file-hash manifest")
            for (name in expectedNames - "manifest.json") {
                val entry = archive.getEntry(name) ?: error("translated input is missing $name")
                val fileRecord = fileManifest.optJSONObject(name) ?: error("translated input has no hash record for $name")
                val limit = when (name) {
                    LIBRARY -> MAX_LIBRARY_BYTES
                    PAYLOAD -> MAX_PAYLOAD_BYTES
                    "rt_report.json" -> MAX_REPORT_BYTES
                    else -> MAX_MANIFEST_BYTES
                }
                require(entry.size in 0L..limit && fileRecord.optLong("sizeBytes", -1) == entry.size) {
                    "$name exceeds its limit or has a mismatched size record"
                }
                val target = File(destination, name)
                copyEntry(archive, entry, target, limit)
                val digest = sha256(target)
                require(digest == fileRecord.optString("sha256")) {
                    "$name does not match its SHA-256 record"
                }
                extracted[name] = target
            }
        }

        val library = extracted.getValue(LIBRARY)
        val payload = extracted.getValue(PAYLOAD)
        val linkReport = JSONObject(extracted.getValue("android-link-report.json").readText(Charsets.UTF_8))
        val translation = JSONObject(extracted.getValue("rt_report.json").readText(Charsets.UTF_8))
        val librarySha = sha256(library)
        val payloadSha = sha256(payload)
        val translationReportSha = sha256(extracted.getValue("rt_report.json"))

        requireArm64SharedObjectHeader(library)
        require(linkReport.optString("status") == "VERIFIED_ANDROID_SHARED_LIBRARY" &&
            linkReport.optBoolean("attempted", false) &&
            linkReport.optBoolean("ndkLinkVerified", false) &&
            linkReport.optString("targetAbi") == ABI &&
            linkReport.optString("elfArchitecture") == ABI &&
            linkReport.optInt("elfClass", 0) == 64 &&
            linkReport.optBoolean("architectureVerified", false) &&
            linkReport.optBoolean("dependenciesVerified", false) &&
            linkReport.optBoolean("exportsVerified", false) &&
            linkReport.optBoolean("translationEntryPointVerified", false) &&
            linkReport.optBoolean("allVerificationsPassed", false) &&
            linkReport.optString("entryPointSymbol") == ENTRY_SYMBOL &&
            linkReport.optString("artifactSha256") == librarySha &&
            linkReport.optLong("artifactSizeBytes", -1) == library.length()) {
            "Android shared library does not match its verified architecture, dependency, export, and JNI-entry report"
        }
        val dependencies = linkReport.optJSONArray("dynamicDependencies") ?: JSONArray()
        require((0 until dependencies.length()).all { dependencies.optString(it) in ALLOWED_NEEDED }) {
            "translated Android library has a dependency outside the reviewed Android system allowlist"
        }

        require(translation.optString("sourceExecutableSha256") == expectedExecutableSha256 &&
            translation.optString("translationEntryPointSymbol") == ENTRY_SYMBOL) {
            "portable-C translation report is not bound to the selected IPA executable and JNI entry point"
        }
        val functionCount = translation.optInt("functions", -1)
        val functionSymbols = translation.optJSONArray("translatedFunctionSymbols")
            ?: error("portable-C report is missing its translated function symbols")
        require(functionCount > 0 && functionSymbols.length() == functionCount &&
            translation.optInt("functionFailures", -1) == 0 &&
            (0 until functionSymbols.length()).all { functionSymbols.optString(it).isNotBlank() } &&
            (0 until functionSymbols.length()).map { functionSymbols.optString(it) }.toSet().size == functionCount) {
            "portable-C translated-function manifest is incomplete or inconsistent"
        }
        require(linkReport.optInt("translatedFunctionCount", -1) == functionCount &&
            linkReport.optInt("linkedTranslatedFunctionCount", -1) == functionCount &&
            linkReport.optLong("translatedUniqueTextBytes", -1) == translation.optLong("translatedUniqueTextBytes", -2) &&
            linkReport.optLong("executableTextBytes", -1) == translation.optLong("executableTextBytes", -2)) {
            "verified ELF coverage does not match the portable-C translation report"
        }

        val payloadManifest = validatePayload(
            payload = payload,
            expectedExecutableSha256 = expectedExecutableSha256,
            translationReportSha256 = translationReportSha,
            librarySha256 = librarySha,
            functionCount = functionCount,
        )
        val memoryImage = payloadManifest.getJSONObject("memoryImage")
        val metadata = JSONObject()
            .put("contract", PAYLOAD_CONTRACT)
            .put("targetAbi", ABI)
            .put("entryPointSymbol", ENTRY_SYMBOL)
            .put("sourceExecutableSha256", expectedExecutableSha256)
            .put("translationReportSha256", translationReportSha)
            .put("androidLibrarySha256", librarySha)
            .put("libraryApkPath", "lib/$ABI/$LIBRARY")
            .put("payloadAssetName", PAYLOAD)
            .put("payloadSha256", payloadSha)
            .put("payloadBytes", payload.length())
            .put("translatedFunctionCount", functionCount)
            .put("translatedUniqueTextBytes", translation.optLong("translatedUniqueTextBytes", 0))
            .put("executableTextBytes", translation.optLong("executableTextBytes", 0))
            .put("memoryImageSha256", memoryImage.optString("sha256"))
            .put("memoryImageBytes", memoryImage.optLong("sizeBytes", 0))
            .put("architectureVerified", true)
            .put("dependenciesVerified", true)
            .put("exportsVerified", true)
            .put("translationEntryPointVerified", true)
            .put("allVerificationsPassed", true)
            .put("rendererConnected", false)
            .put("pixelsVerified", false)
            .put("gameplayVerified", false)
        return Staged(
            library = library,
            payload = payload,
            metadata = metadata,
            verification = linkReport,
            report = translation,
        )
    }

    private fun validatePayload(
        payload: File,
        expectedExecutableSha256: String,
        translationReportSha256: String,
        librarySha256: String,
        functionCount: Int,
    ): JSONObject {
        val expectedNames = setOf("manifest.json", "rt_mem.bin")
        ZipFile(payload).use { archive ->
            val entries = Collections.list(archive.entries())
            require(entries.size == expectedNames.size && entries.map { it.name }.toSet() == expectedNames &&
                entries.none { it.isDirectory }) {
                "translated memory payload ZIP has unexpected or duplicate entries"
            }
            val manifest = JSONObject(String(readEntry(archive, "manifest.json", MAX_MANIFEST_BYTES), Charsets.UTF_8))
            require(manifest.optString("contract") == PAYLOAD_CONTRACT &&
                manifest.optInt("schemaVersion", 0) == 1 &&
                manifest.optString("targetAbi") == ABI &&
                manifest.optString("entryPointSymbol") == ENTRY_SYMBOL &&
                manifest.optString("sourceExecutableSha256") == expectedExecutableSha256 &&
                manifest.optString("translationReportSha256") == translationReportSha256 &&
                manifest.optString("androidLibrarySha256") == librarySha256 &&
                manifest.optInt("translatedFunctionCount", -1) == functionCount) {
                "translated memory payload provenance does not match the verified Android library"
            }
            val memory = archive.getEntry("rt_mem.bin") ?: error("translated memory image is missing")
            val imageManifest = manifest.optJSONObject("memoryImage")
                ?: error("translated memory payload has no image manifest")
            require(imageManifest.optString("name") == "rt_mem.bin" &&
                memory.size in 1L..MAX_PAYLOAD_BYTES &&
                imageManifest.optLong("sizeBytes", -1) == memory.size) {
                "translated memory image size or name is invalid"
            }
            val digest = MessageDigest.getInstance("SHA-256")
            archive.getInputStream(memory).use { input ->
                val buffer = ByteArray(64 * 1024)
                var total = 0L
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    total += count
                    require(total <= MAX_PAYLOAD_BYTES) { "translated memory image exceeds the 512 MiB limit" }
                    digest.update(buffer, 0, count)
                }
                require(total == memory.size) { "translated memory image was truncated" }
            }
            val actualHash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
            require(actualHash == imageManifest.optString("sha256")) {
                "translated memory image hash does not match its manifest"
            }
            return manifest
        }
    }

    private fun readEntry(archive: ZipFile, name: String, maximum: Long): ByteArray {
        val entry = archive.getEntry(name) ?: error("translated runtime bundle is missing $name")
        require(!entry.isDirectory && entry.size in 0L..maximum) { "$name exceeds its size limit" }
        val output = ByteArrayOutputStream(minOf(entry.size.toInt().coerceAtLeast(0), maximum.toInt()))
        archive.getInputStream(entry).use { input ->
            val buffer = ByteArray(8192)
            var total = 0L
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                total += count
                require(total <= maximum) { "$name exceeds its size limit" }
                output.write(buffer, 0, count)
            }
            require(total == entry.size) { "$name was truncated" }
        }
        return output.toByteArray()
    }

    private fun copyEntry(archive: ZipFile, entry: java.util.zip.ZipEntry, destination: File, maximum: Long) {
        require(destination.parentFile?.isDirectory == true || destination.parentFile?.mkdirs() == true) {
            "cannot create translated-runtime staging directory"
        }
        var total = 0L
        try {
            archive.getInputStream(entry).use { input ->
                FileOutputStream(destination).use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        total += count
                        require(total <= maximum) { "${entry.name} exceeds its size limit" }
                        output.write(buffer, 0, count)
                    }
                    output.fd.sync()
                }
            }
            require(total == entry.size) { "${entry.name} was truncated" }
        } catch (error: Throwable) {
            destination.delete()
            throw error
        }
    }

    private fun requireArm64SharedObjectHeader(file: File) {
        require(file.length() >= 64) { "translated native library is too small to be an ELF shared object" }
        RandomAccessFile(file, "r").use { input ->
            val ident = ByteArray(20)
            input.readFully(ident)
            require(ident.copyOfRange(0, 4).contentEquals(
                byteArrayOf(0x7f, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte()),
            ) && ident[4].toInt() == 2 && ident[5].toInt() == 1) {
                "translated native library is not a little-endian ELF64 object"
            }
            val type = (ident[16].toInt() and 0xff) or ((ident[17].toInt() and 0xff) shl 8)
            val machine = (ident[18].toInt() and 0xff) or ((ident[19].toInt() and 0xff) shl 8)
            require(type == 3 && machine == 183) { "translated native library is not an AArch64 ET_DYN shared object" }
        }
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().buffered().use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
