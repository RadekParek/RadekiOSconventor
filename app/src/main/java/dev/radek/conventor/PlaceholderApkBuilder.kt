package dev.radek.conventor

import android.content.Context
import android.graphics.BitmapFactory
import com.android.apksig.ApkSigner
import com.android.apksig.ApkVerifier
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest

/** Builds an explicitly non-playable Android shell from a bundled, source-free template APK. */
internal class PlaceholderApkBuilder(private val context: Context) {
    companion object {
        private const val MAX_TEMPLATE_ENTRY_BYTES = 32L * 1024 * 1024
        private const val MAX_ICON_BYTES = 16L * 1024 * 1024
        private val DEX_NAME_REGEX = Regex("""classes[0-9]+\.dex""")
        private val PNG_SIGNATURE = byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
    }

    private data class TemplateEntries(
        val manifest: ByteArray,
        val resources: ByteArray,
        val dexes: List<Pair<String, ByteArray>>,
        val fallbackIcon: ByteArray,
        val iconEntryPath: String,
    )

    fun build(dir: File, progress: (Int, String) -> Unit = { _, _ -> }): JSONObject {
        require(dir.isDirectory && File(dir, "source.ipa").isFile) { "retained source IPA is missing" }
        val reportFile = File(dir, "report.json")
        val report = JSONObject(reportFile.readText())
        val app = report.optJSONObject("application") ?: error("application metadata missing")
        val source = report.optJSONObject("source") ?: error("source metadata missing")
        val sourceHash = source.optString("sha256")
        require(sourceHash.matches(Regex("[0-9a-f]{64}")) && app.optString("sha256") == sourceHash) {
            "source IPA hash metadata is invalid"
        }
        val actualHash = sha256(File(dir, "source.ipa"))
        require(actualHash == sourceHash) { "retained IPA does not match the analysis report" }
        val name = sanitizeLabel(app.optString("name").ifBlank { app.optString("bundleId").substringAfterLast('.').ifBlank { "Imported iOS app" } })
        val bundleId = app.optString("bundleId").take(255)
        val resultName = ArtifactNames.placeholderApkFileName(report)
        val resultFile = File(dir, resultName)
        require(resultFile.parentFile?.canonicalFile == dir.canonicalFile) { "invalid placeholder result path" }

        val reportContext = Library(context)
        fun setProgress(percent: Int, status: String, message: String) {
            report.put("placeholderBuildProgress", JSONObject()
                .put("percent", percent.coerceIn(0, 100))
                .put("status", status)
                .put("message", message)
                .put("updatedAt", java.time.Instant.now().toString()))
            reportContext.save(dir, report)
            progress(percent.coerceIn(0, 100), message)
        }

        val unsignedFile = File(dir, ".placeholder-unsigned.apk")
        val signedFile = File(dir, ".placeholder-signed.apk")
        val backupFile = File(dir, ".$resultName.backup")
        val finalPending = File(dir, ".$resultName.pending")
        var finalized = false
        var resultInstalled = false
        if (backupFile.isFile) {
            if (!resultFile.exists()) backupFile.renameTo(resultFile) else backupFile.delete()
        }
        unsignedFile.delete(); signedFile.delete(); finalPending.delete()
        try {
            setProgress(3, "BUILDING", "Preparing an installable preview shell. iOS game code is not translated.")
            val identity = PlaceholderSigningIdentity.loadOrCreate(
                File(context.noBackupFilesDir, "placeholder-apk-signing-identity.bin"),
            )
            val certificateHash = sha256(identity.certificate.encoded)
            val packageName = "dev.radek.placeholder.p${sourceHash.take(24)}${certificateHash.take(8)}"
            require(packageName.length <= 127)
            setProgress(12, "BUILDING", "Loading the Android preview shell and installation signer")
            setProgress(18, "BUILDING", "Reading the bundled Android preview resources")
            val templateEntries = TemplateEntries(
                manifest = readAsset("placeholder-template/AndroidManifest.xml", 2L * 1024 * 1024),
                resources = readAsset("placeholder-template/resources.arsc", MAX_TEMPLATE_ENTRY_BYTES),
                dexes = readTemplateDexes("placeholder-template"),
                fallbackIcon = readAsset("placeholder-template/fallback-icon.png", MAX_ICON_BYTES),
                iconEntryPath = readAsset("placeholder-template/icon-entry-path.txt", 1024).toString(Charsets.UTF_8),
            )
            val manifest = templateEntries.manifest
            val resourceTable = templateEntries.resources
            val templateFallbackIcon = templateEntries.fallbackIcon
            require(templateEntries.dexes.isNotEmpty() &&
                templateEntries.dexes.all { (_, bytes) -> validDex(bytes) } &&
                templateFallbackIcon.size >= PNG_SIGNATURE.size &&
                templateFallbackIcon.copyOfRange(0, PNG_SIGNATURE.size).contentEquals(PNG_SIGNATURE) && validPng(templateFallbackIcon)) {
                "preview template is missing a valid launcher DEX or fallback icon"
            }
            val iconEntryPath = SafeZip.validateName(templateEntries.iconEntryPath)
            require(iconEntryPath == templateEntries.iconEntryPath && iconEntryPath.startsWith("res/") &&
                iconEntryPath.substringAfterLast('/') == "generated_placeholder_icon.png") {
                "preview template icon resource path is invalid"
            }
            val appName = sanitizeLabel(name)
            val customizedManifest = BinaryXmlManifest.customize(manifest, packageName, appName)
            val customizedResources = ResourceTablePackagePatcher.customize(resourceTable, packageName)
            setProgress(36, "BUILDING", "Branded the Android package with the IPA name and a unique package id")

            val iconStatus = report.optJSONObject("icon")?.optString("status").orEmpty()
            val iconFile = File(dir, "icon.png")
            val candidateIcon = if (iconFile.isFile && iconFile.length() in 1..MAX_ICON_BYTES) {
                iconFile.readBytes().takeIf { bytes ->
                    bytes.size >= PNG_SIGNATURE.size && bytes.copyOfRange(0, PNG_SIGNATURE.size).contentEquals(PNG_SIGNATURE) && validPng(bytes)
                }
            } else null
            val iconBytes = candidateIcon ?: templateFallbackIcon
            val iconSource = when {
                candidateIcon == null -> "TEMPLATE_FALLBACK"
                iconStatus == "SUPPORTED" -> "RECOVERED_IPA_ICON"
                else -> "GENERATED_APP_NAME_ICON"
            }
            val originalIconAvailable = iconSource == "RECOVERED_IPA_ICON"
            val apiMapping = report.optJSONObject("apiMapping") ?: JSONObject()
            val distinctImportSymbols = apiMapping.optInt("distinctImportSymbols", 0).coerceAtLeast(0)
            val classifiedImportSymbols = apiMapping.optInt("classifiedImportSymbols", 0).coerceAtLeast(0)
            val classificationCoveragePercent = apiMapping.optInt("classificationCoveragePercent", 0).coerceIn(0, 100)
            val runtimeVerifiedNdkCandidates = apiMapping.optInt("runtimeVerifiedNdkCandidates", 0).coerceAtLeast(0)
            val runtimeVerifiedAndroidApiLevel = apiMapping.optInt("runtimeVerifiedAndroidApiLevel", 0).coerceAtLeast(0)
            val directApiCandidates = apiMapping.optInt("mappedNameCandidates", 0).coerceAtLeast(0)
            val semanticApiCandidates = apiMapping.optInt("semanticRewriteCandidates", 0).coerceAtLeast(0)
            val unmappedApiSymbols = apiMapping.optInt("unmappedSymbolCount", 0).coerceAtLeast(0)
            val apiLevelNote = if (runtimeVerifiedAndroidApiLevel > 0) " on Android API $runtimeVerifiedAndroidApiLevel" else ""
            val analysisSummary = "Static analysis only: $classifiedImportSymbols/$distinctImportSymbols symbols triaged ($classificationCoveragePercent%); $directApiCandidates direct-name candidates ($runtimeVerifiedNdkCandidates runtime exports resolved$apiLevelNote), $semanticApiCandidates semantic targets, $unmappedApiSymbols unmapped. No game code or API implementation was translated."
            // Shown on the generated launcher; the full summary above stays in the
            // artifact's machine-readable metadata.
            val analysisStats = "Static analysis only: $classifiedImportSymbols/$distinctImportSymbols symbols triaged ($classificationCoveragePercent%); $directApiCandidates direct-name candidates ($runtimeVerifiedNdkCandidates runtime exports resolved$apiLevelNote), $semanticApiCandidates semantic targets, $unmappedApiSymbols unmapped."
            val analysisInfo = JSONObject()
                .put("distinctImportSymbols", distinctImportSymbols)
                .put("classifiedImportSymbols", classifiedImportSymbols)
                .put("classificationCoveragePercent", classificationCoveragePercent)
                .put("runtimeVerifiedNdkCandidates", runtimeVerifiedNdkCandidates)
                .put("runtimeVerifiedAndroidApiLevel", runtimeVerifiedAndroidApiLevel)
                .put("directApiCandidates", directApiCandidates)
                .put("semanticApiCandidates", semanticApiCandidates)
                .put("unmappedApiSymbols", unmappedApiSymbols)
                .put("translatedGameFunctions", 0)
                .put("apiReplacementImplementations", 0)
            val infoJson = JSONObject()
                .put("gameName", appName)
                .put("bundleId", bundleId)
                .put("iconSource", iconSource)
                .put("analysisSummary", analysisSummary)
                .put("analysisStats", analysisStats)
                .put("analysisOnly", analysisInfo)
                .put("placeholderOnly", true)
                .put("gameCodeIncluded", false)
                .toString().toByteArray(Charsets.UTF_8)

            setProgress(52, "BUILDING", "Packaging aligned Android resources and an honest non-playable preview screen")
            val dexNames = templateEntries.dexes.map { it.first }.toSet()
            val expectedEntries = setOf(
                "AndroidManifest.xml",
                "resources.arsc",
                iconEntryPath,
                "assets/ipa-icon.png",
                "assets/placeholder-info.json",
            ) + dexNames
            val alignedEntries = buildMap<String, Int> {
                put("AndroidManifest.xml", AlignedApkZip.ALIGNMENT)
                put("resources.arsc", AlignedApkZip.ALIGNMENT)
                put(iconEntryPath, AlignedApkZip.ALIGNMENT)
                put("assets/ipa-icon.png", AlignedApkZip.ALIGNMENT)
                dexNames.forEach { put(it, AlignedApkZip.ALIGNMENT) }
            }
            AlignedApkZip.write(
                unsignedFile,
                listOf(AlignedApkZip.Entry("AndroidManifest.xml", customizedManifest)) +
                    templateEntries.dexes.map { (dexName, dexBytes) -> AlignedApkZip.Entry(dexName, dexBytes) } +
                    listOf(
                        AlignedApkZip.Entry("resources.arsc", customizedResources),
                        AlignedApkZip.Entry(iconEntryPath, iconBytes),
                        AlignedApkZip.Entry("assets/ipa-icon.png", iconBytes),
                        AlignedApkZip.Entry("assets/placeholder-info.json", infoJson, compressed = true),
                    ),
            )
            require(unsignedFile.isFile && unsignedFile.length() > 0) { "could not assemble placeholder package" }
            AlignedApkZip.verify(unsignedFile, expectedEntries, alignedEntries)

            setProgress(68, "SIGNING", "Signing the preview APK for Android installation")
            val signerConfig = ApkSigner.SignerConfig.Builder(
                "RadekiOS placeholder",
                identity.privateKey,
                listOf(identity.certificate),
            ).build()
            ApkSigner.Builder(listOf(signerConfig))
                .setInputApk(unsignedFile)
                .setOutputApk(signedFile)
                .setMinSdkVersion(26)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                .build()
                .sign()
            require(signedFile.isFile && signedFile.length() > 0) { "APK signing produced no output" }

            setProgress(88, "VERIFYING", "Checking the generated APK signature and package structure")
            val verification = ApkVerifier.Builder(signedFile).build().verify()
            require(verification.isVerified) {
                "generated placeholder APK signature verification failed: ${verification.errors.joinToString("; ")}"
            }
            require(verification.signerCertificates.isNotEmpty()) { "generated APK has no signer certificate" }
            @Suppress("DEPRECATION")
            val packageInfo = context.packageManager.getPackageArchiveInfo(
                signedFile.path,
                android.content.pm.PackageManager.GET_ACTIVITIES or android.content.pm.PackageManager.GET_SIGNATURES,
            ) ?: error("Android could not parse the generated placeholder APK")
            require(packageInfo.packageName == packageName) { "Android parsed an unexpected placeholder package id" }
            require(packageInfo.activities.orEmpty().any { it.name == "dev.radek.generated.GeneratedPlaceholderActivity" }) {
                "placeholder launcher activity is missing from the parsed package"
            }
            require(packageInfo.applicationInfo?.loadLabel(context.packageManager)?.toString() == appName) {
                "Android did not parse the IPA application name from the placeholder manifest"
            }

            require(signedFile.copyTo(finalPending, overwrite = true).isFile) { "could not stage signed APK" }
            if (resultFile.exists()) {
                require(!backupFile.exists() || backupFile.delete()) { "cannot remove stale placeholder backup" }
                require(resultFile.renameTo(backupFile)) { "cannot preserve the previous placeholder APK" }
            }
            require(finalPending.renameTo(resultFile)) { "could not save generated placeholder APK" }
            resultInstalled = true
            val resultHash = sha256(resultFile)
            require(sha256(verification.signerCertificates.first().encoded) == certificateHash) {
                "generated APK signer changed during packaging"
            }
            val audit = InstallAudit.inspect(context, resultFile, packageName)
            require(audit.blockers.isEmpty()) {
                "generated APK would not install: ${audit.blockers.joinToString("; ")}"
            }
            val conversion = JSONObject()
                .put("status", "GENERATED")
                .put("completeGameConversion", false)
                .put("placeholderOnly", true)
                .put("gameCodeTranslated", false)
                .put("gameCodeIncluded", false)
                .put("gamePlayable", false)
                .put("installableAndroidPackage", true)
                .put("artifact", resultFile.name)
                .put("package", packageName)
                .put("applicationName", appName)
                .put("sourceBundleId", bundleId)
                .put("sourceSha256", sourceHash)
                .put("sha256", resultHash)
                .put("bytes", resultFile.length())
                .put("iconSource", iconSource)
                .put("iconSha256", sha256(iconBytes))
                .put("installAudit", JSONObject()
                    .put("installable", audit.installable)
                    .put("warnings", JSONArray().apply { audit.warnings.forEach { put(it) } }))
                .put("originalIconAvailable", originalIconAvailable)
                .put("analysisOnly", analysisInfo)
                .put("signing", JSONObject()
                    .put("schemes", org.json.JSONArray().put("v1").put("v2").put("v3"))
                    .put("certificateSha256", certificateHash))
                .put("translationStatus", "NONE")
                .put("translatedGameFunctions", 0)
                .put("apiReplacementImplementations", 0)
                .put("completedAt", java.time.Instant.now().toString())
            report.put("placeholderConversion", conversion)
            report.put("placeholderBuildProgress", JSONObject()
                .put("percent", 100)
                .put("status", "GENERATED")
                .put("message", "Installable preview APK generated. It contains no translated game code and is not playable.")
                .put("updatedAt", java.time.Instant.now().toString()))
            reportContext.save(dir, report)
            finalized = true
            backupFile.delete()
            progress(100, "Installable preview ready; game code was not translated and the game will not run")
            return conversion
        } catch (error: Throwable) {
            if (!finalized) {
                if (resultInstalled) resultFile.delete()
                if (backupFile.isFile && !resultFile.exists()) backupFile.renameTo(resultFile)
            }
            report.put("placeholderBuildProgress", JSONObject()
                .put("percent", 0)
                .put("status", "FAILED")
                .put("message", error.message ?: error.javaClass.simpleName)
                .put("updatedAt", java.time.Instant.now().toString()))
            try { reportContext.save(dir, report) } catch (_: Exception) { }
            throw error
        } finally {
            unsignedFile.delete()
            signedFile.delete()
            File(dir, ".${resultName}.pending").delete()
        }
    }

    private fun sanitizeLabel(raw: String): String {
        val output = StringBuilder()
        var index = 0
        while (index < raw.length && output.length < 160) {
            val point = raw.codePointAt(index)
            if (!Character.isISOControl(point) && Character.isValidCodePoint(point)) {
                val count = Character.charCount(point)
                if (output.length + count <= 160) output.appendCodePoint(point)
            }
            index += Character.charCount(point)
        }
        return output.toString().trim().ifBlank { "Imported iOS app" }
    }

    private fun validPng(bytes: ByteArray): Boolean {
        return try {
            val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size, options)
            options.outWidth in 1..8192 && options.outHeight in 1..8192
        } catch (_: Exception) {
            false
        }
    }

    private fun validDex(bytes: ByteArray): Boolean =
        bytes.size >= 0x70 && bytes[0] == 'd'.code.toByte() && bytes[1] == 'e'.code.toByte() &&
            bytes[2] == 'x'.code.toByte() && bytes[3] == '\n'.code.toByte() &&
            (4..6).all { (bytes[it].toInt() and 0xff) in 0x30..0x39 } && bytes[7] == 0.toByte()

    private fun readAsset(name: String, maximum: Long): ByteArray = context.assets.open(name).use { input ->
        val output = ByteArrayOutputStream()
        val buffer = ByteArray(65536)
        var total = 0L
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            total += count
            require(total <= maximum) { "bundled placeholder asset exceeds size limit: $name" }
            output.write(buffer, 0, count)
        }
        require(total > 0) { "bundled placeholder asset is empty: $name" }
        output.toByteArray()
    }

    /** Reads every launcher DEX listed by the embed task's manifest, classes.dex first. */
    private fun readTemplateDexes(assetDirectory: String): List<Pair<String, ByteArray>> {
        val names = readAsset("$assetDirectory/template-dex-entries.txt", 16 * 1024)
            .toString(Charsets.UTF_8).lineSequence().map { it.trim() }.filter { it.isNotBlank() }
            .map { it.substringBefore(':') }
            .filter { it == "classes.dex" || DEX_NAME_REGEX.matches(it) }
            .sortedWith(compareBy<String>({ it != "classes.dex" }, { it.length }, { it }))
            .distinct()
            .toList()
        require(names.firstOrNull() == "classes.dex") { "$assetDirectory DEX manifest is missing classes.dex" }
        return names.map { name -> name to readAsset("$assetDirectory/$name", MAX_TEMPLATE_ENTRY_BYTES) }
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

    private fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
        .digest(bytes).joinToString("") { "%02x".format(it.toInt() and 0xff) }
}
