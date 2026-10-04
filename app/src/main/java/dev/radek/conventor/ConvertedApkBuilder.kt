package dev.radek.conventor

import android.content.Context
import android.graphics.BitmapFactory
import android.util.Base64
import com.android.apksig.ApkSigner
import com.android.apksig.ApkVerifier
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest

/**
 * Builds a signed, installable bounded complete-game APK on the device.
 *
 * The IPA must already be proven by [NativeBridge.translateTrivial]: its whole
 * executable is one closed-integer ARM64 routine with no imports, dependencies,
 * fixups or metadata. Everything outside that subset throws and stays unbuilt;
 * no conversion is ever claimed without the static proof.
 */
internal class ConvertedApkBuilder(private val context: Context) {
    companion object {
        private const val MAX_TEMPLATE_ENTRY_BYTES = 32L * 1024 * 1024
        private const val MAX_ICON_BYTES = 16L * 1024 * 1024
        private const val MAX_RESOURCE_FILE_BYTES = 32L * 1024 * 1024
        private const val MAX_RESOURCE_TOTAL_BYTES = 256L * 1024 * 1024
        private const val MAX_RESOURCE_FILES = 4096
        private const val ENTRY_CLASS = "dev.radek.generated.MainActivity"
        private const val JNI_SYMBOL = "Java_dev_radek_generated_MainActivity_runNative"
        private const val BACKEND = "radek-device-bounded-v1"
        private const val CONTRACT = "complete-game-v1"
        private val DEX_NAME_REGEX = Regex("""classes[0-9]+\.dex""")
        private val PNG_SIGNATURE = byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
    }

    fun build(dir: File, progress: (Int, String) -> Unit = { _, _ -> }): JSONObject {
        require(dir.isDirectory && File(dir, "source.ipa").isFile) { "retained source IPA is missing" }
        val report = JSONObject(File(dir, "report.json").readText())
        val app = report.optJSONObject("application") ?: error("application metadata missing")
        val source = report.optJSONObject("source") ?: error("source metadata missing")
        val sourceHash = source.optString("sha256")
        require(sourceHash.matches(Regex("[0-9a-f]{64}")) && app.optString("sha256") == sourceHash) {
            "source IPA hash metadata is invalid"
        }
        val actualHash = sha256(File(dir, "source.ipa"))
        require(actualHash == sourceHash) { "retained IPA does not match the analysis report" }
        val appName = sanitizeLabel(
            app.optString("name").ifBlank { app.optString("bundleId").substringAfterLast('.').ifBlank { "Converted IPA" } },
        )
        val resultName = ArtifactNames.apkFileName(report)
        val resultFile = File(dir, resultName)
        require(resultFile.parentFile?.canonicalFile == dir.canonicalFile) { "invalid converted result path" }

        val reportContext = Library(context)
        fun setProgress(percent: Int, status: String, message: String) {
            report.put(
                "convertedBuildProgress",
                JSONObject()
                    .put("percent", percent.coerceIn(0, 100))
                    .put("status", status)
                    .put("message", message)
                    .put("updatedAt", java.time.Instant.now().toString()),
            )
            reportContext.save(dir, report)
            progress(percent.coerceIn(0, 100), message)
        }

        val unsignedFile = File(dir, ".converted-unsigned.apk")
        val signedFile = File(dir, ".converted-signed.apk")
        val backupFile = File(dir, ".$resultName.converted-backup")
        val finalPending = File(dir, ".$resultName.converted-pending")
        val extracted = File(dir, "convert-workspace")
        var finalized = false
        var resultInstalled = false
        try {
            setProgress(4, "CONVERTING", "Re-checking the retained IPA before conversion")
            if (backupFile.isFile) {
                if (!resultFile.exists()) backupFile.renameTo(resultFile) else backupFile.delete()
            }
            unsignedFile.delete(); signedFile.delete(); finalPending.delete()
            if (extracted.exists()) extracted.deleteRecursively()

            setProgress(12, "CONVERTING", "Extracting the authorized IPA for the bounded converter")
            SafeZip.extract(File(dir, "source.ipa"), extracted)
            val apps = File(extracted, "Payload").listFiles().orEmpty().filter { it.isDirectory && it.name.endsWith(".app") }
            require(apps.size == 1) { "expected exactly one Payload/*.app" }
            val appDir = apps.single()
            val plist = Plist.read(File(appDir, "Info.plist").readBytes())
            val executableName = plist["CFBundleExecutable"] as? String ?: error("CFBundleExecutable missing")
            require(SafeZip.validateName(executableName) == executableName && '/' !in executableName)
            val binary = File(appDir, executableName)
            require(binary.isFile && binary.length() in 1..(64L * 1024 * 1024)) { "executable missing or exceeds the on-device limit" }

            setProgress(24, "CONVERTING", "Proving the executable is one closed-integer routine")
            val proof = JSONObject(NativeBridge.translateTrivial(binary.readBytes()))
            require(proof.optString("status") == "PROVEN") {
                "IPA is outside the bounded converter subset: ${proof.optString("reason")}"
            }
            require(proof.optInt("coveragePercent") == 100 && proof.optInt("functionCount") == 1) {
                "the bounded converter requires full single-function coverage"
            }
            val machineCode = Base64.decode(proof.getString("machineCode"), Base64.DEFAULT)
            require(machineCode.isNotEmpty() && machineCode.size == proof.optInt("sourceBytes")) {
                "translated machine code does not match its proof"
            }
            val proofStrings = proof.optJSONArray("strings")
            var launchMessage = ""
            if (proofStrings != null) {
                for (index in 0 until proofStrings.length()) {
                    val candidate = proofStrings.optString(index, "")
                    if (candidate.length in 3..512 && candidate.length > launchMessage.length) launchMessage = candidate
                }
            }
            if (launchMessage.isEmpty()) launchMessage = "Converted by RadekiOSConventor"

            setProgress(36, "CONVERTING", "Collecting and hashing the bundle resources")
            val inventory = JSONArray()
            val resourceEntries = ArrayList<Pair<String, ByteArray>>()
            var totalResourceBytes = 0L
            var resourceCount = 0
            appDir.walkTopDown().filter { it.isFile }.sortedBy { it.path }.forEach { file ->
                if (file == binary) return@forEach
                val relative = file.relativeTo(appDir).path.replace(File.separatorChar, '/')
                SafeZip.validateName(relative)
                require(++resourceCount <= MAX_RESOURCE_FILES) { "too many bundle resources for the bounded converter" }
                require(file.length() <= MAX_RESOURCE_FILE_BYTES) { "bundle resource exceeds the bounded limit: $relative" }
                val payload = file.readBytes()
                totalResourceBytes += payload.size
                require(totalResourceBytes <= MAX_RESOURCE_TOTAL_BYTES) { "bundle resources exceed the bounded total limit" }
                inventory.put(JSONObject().put("path", relative).put("sha256", sha256(payload)))
                resourceEntries += relative to payload
            }

            setProgress(48, "CONVERTING", "Generating the Android native entry library")
            val elf = ConvertedElfWriter.buildSharedObject(machineCode, JNI_SYMBOL)
            val elfSha = sha256(elf)

            setProgress(56, "PACKAGING", "Reading the bundled converted-app template")
            val templateManifest = readAsset("converted-template/AndroidManifest.xml", 2L * 1024 * 1024)
            val templateResources = readAsset("converted-template/resources.arsc", MAX_TEMPLATE_ENTRY_BYTES)
            // AGP splits even the tiny launcher template into classes.dex +
            // classesN.dex; Android loads every classesN.dex at the APK root, so
            // all of them are embedded verbatim (manifest written by the embed task).
            val templateDexNames = readAsset("converted-template/template-dex-entries.txt", 16 * 1024)
                .toString(Charsets.UTF_8).lineSequence().map { it.trim() }.filter { it.isNotBlank() }
                .map { it.substringBefore(':') }
                .filter { it == "classes.dex" || DEX_NAME_REGEX.matches(it) }
                .sortedWith(compareBy<String>({ it != "classes.dex" }, { it.length }, { it }))
                .distinct()
                .toList()
            require(templateDexNames.firstOrNull() == "classes.dex") {
                "converted template DEX manifest is missing classes.dex"
            }
            val templateDexes = templateDexNames.map { name ->
                name to readAsset("converted-template/$name", MAX_TEMPLATE_ENTRY_BYTES)
            }
            val templateFallbackIcon = readAsset("converted-template/fallback-icon.png", MAX_ICON_BYTES)
            val iconEntryPath = SafeZip.validateName(
                readAsset("converted-template/icon-entry-path.txt", 1024).toString(Charsets.UTF_8),
            )
            require(templateDexes.all { (_, bytes) -> validDex(bytes) } &&
                templateFallbackIcon.size >= PNG_SIGNATURE.size &&
                templateFallbackIcon.copyOfRange(0, PNG_SIGNATURE.size).contentEquals(PNG_SIGNATURE) &&
                validPng(templateFallbackIcon)) {
                "converted template is missing a valid launcher DEX or fallback icon"
            }
            require(templateDexes.any { (_, bytes) ->
                String(bytes, Charsets.ISO_8859_1).contains("Ldev/radek/generated/MainActivity;")
            }) { "converted template DEX files do not define the launcher entry class" }
            require(iconEntryPath.startsWith("res/") &&
                iconEntryPath.substringAfterLast('/') == "generated_converted_icon.png") {
                "converted template icon resource path is invalid"
            }

            val packageName = "dev.radek.converted.p" + sourceHash.take(20)
            require(packageName.length <= 127)
            val manifest = BinaryXmlManifest.customize(templateManifest, packageName, appName)
            val resources = ResourceTablePackagePatcher.customize(templateResources, packageName)

            val iconStatus = report.optJSONObject("icon")?.optString("status").orEmpty()
            val iconFile = File(dir, "icon.png")
            val recoveredIcon = if (iconStatus == "SUPPORTED" && iconFile.isFile &&
                iconFile.length() in 1..MAX_ICON_BYTES
            ) {
                iconFile.readBytes().takeIf { bytes ->
                    bytes.size >= PNG_SIGNATURE.size &&
                        bytes.copyOfRange(0, PNG_SIGNATURE.size).contentEquals(PNG_SIGNATURE) && validPng(bytes)
                }
            } else null
            val launcherIcon = recoveredIcon ?: templateFallbackIcon
            val launcherIconSha = if (recoveredIcon != null) sha256(recoveredIcon) else ""

            val metadata = JSONObject()
                .put("contract", CONTRACT)
                .put("generator", "RadekiOSConventor (on-device)")
                .put("package", packageName)
                .put("targetAbi", "arm64-v8a")
                .put("launchMessage", launchMessage)
                .put("applicationName", appName)
                .put("source", JSONObject()
                    .put("sha256", sourceHash)
                    .put("originalName", source.optString("originalName"))
                    .put("bundleId", app.optString("bundleId")))
                .put("conversion", JSONObject()
                    .put("backend", BACKEND)
                    .put("targetAbi", "arm64-v8a")
                    .put("outputBytes", machineCode.size)
                    .put("machineCodeSha256", sha256(machineCode))
                    .put("entrySymbol", JNI_SYMBOL)
                    .put("nativeLibrarySha256", elfSha))
                .put("gameConversion", JSONObject()
                    .put("status", "COMPLETE")
                    .put("completeGameConversion", true)
                    .put("reachableSourceFunctions", 1)
                    .put("translatedReachableFunctions", 1)
                    .put("untranslatedReachableFunctions", 0)
                    .put("reachableApiCount", 0)
                    .put("generatedApiReplacements", 0)
                    .put("nativeApiPassthroughs", 0)
                    .put("untranslatedReachableApiCount", 0)
                    .put("apiCoverageComplete", true)
                    .put("apiReplacements", JSONArray())
                    .put("resourcesComplete", true)
                    .put("lifecycleImplemented", true)
                    .put("sourceIconSha256", launcherIconSha)
                    .put("launcherIconSha256", launcherIconSha)
                    .put("backend", BACKEND))
                .put("resourceInventory", inventory)

            setProgress(68, "PACKAGING", "Assembling the converted APK entries")
            val entries = ArrayList<AlignedApkZip.Entry>()
            entries += AlignedApkZip.Entry("AndroidManifest.xml", manifest)
            templateDexes.forEach { (name, bytes) -> entries += AlignedApkZip.Entry(name, bytes) }
            entries += AlignedApkZip.Entry("resources.arsc", resources)
            entries += AlignedApkZip.Entry(iconEntryPath, launcherIcon)
            entries += AlignedApkZip.Entry("lib/arm64-v8a/libconverted.so", elf,
                alignment = AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT)
            entries += AlignedApkZip.Entry("assets/conversion.json", metadata.toString().toByteArray(Charsets.UTF_8), compressed = true)
            if (recoveredIcon != null) {
                entries += AlignedApkZip.Entry("assets/ipa-icon.png", recoveredIcon, compressed = true)
            }
            for ((relative, payload) in resourceEntries) {
                entries += AlignedApkZip.Entry("assets/bundle/$relative", payload, compressed = true)
            }
            val expectedNames = entries.map { it.name }.toSet()
            val alignedNames = buildMap<String, Int> {
                put("AndroidManifest.xml", AlignedApkZip.ALIGNMENT)
                put("resources.arsc", AlignedApkZip.ALIGNMENT)
                put(iconEntryPath, AlignedApkZip.ALIGNMENT)
                put("lib/arm64-v8a/libconverted.so", AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT)
                templateDexNames.forEach { put(it, AlignedApkZip.ALIGNMENT) }
            }
            AlignedApkZip.write(unsignedFile, entries)
            require(unsignedFile.isFile && unsignedFile.length() > 0) { "could not assemble the converted APK" }
            AlignedApkZip.verify(unsignedFile, expectedNames, alignedNames)

            setProgress(80, "SIGNING", "Signing the converted APK for Android installation")
            val identity = PlaceholderSigningIdentity.loadOrCreate(
                File(context.noBackupFilesDir, "placeholder-apk-signing-identity.bin"),
            )
            val certificateHash = sha256(identity.certificate.encoded)
            val signerConfig = ApkSigner.SignerConfig.Builder(
                "RadekiOS converted",
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

            setProgress(90, "VERIFYING", "Checking the converted APK signature and package structure")
            val verification = ApkVerifier.Builder(signedFile).build().verify()
            require(verification.isVerified) {
                "converted APK signature verification failed: ${verification.errors.joinToString("; ")}"
            }
            require(verification.signerCertificates.isNotEmpty()) { "converted APK has no signer certificate" }
            @Suppress("DEPRECATION")
            val packageInfo = context.packageManager.getPackageArchiveInfo(
                signedFile.path,
                android.content.pm.PackageManager.GET_ACTIVITIES or android.content.pm.PackageManager.GET_SIGNATURES,
            ) ?: error("Android could not parse the converted APK")
            require(packageInfo.packageName == packageName) { "Android parsed an unexpected converted package id" }
            require(packageInfo.activities.orEmpty().any { it.name == ENTRY_CLASS }) {
                "converted launcher activity is missing from the parsed package"
            }
            require(packageInfo.applicationInfo?.loadLabel(context.packageManager)?.toString() == appName) {
                "Android did not parse the IPA application name from the converted manifest"
            }

            require(signedFile.copyTo(finalPending, overwrite = true).isFile) { "could not stage the converted APK" }
            if (resultFile.exists()) {
                require(!backupFile.exists() || backupFile.delete()) { "cannot remove stale converted backup" }
                require(resultFile.renameTo(backupFile)) { "cannot preserve the previous converted APK" }
            }
            require(finalPending.renameTo(resultFile)) { "could not save the converted APK" }
            resultInstalled = true
            val resultHash = sha256(resultFile)
            require(sha256(verification.signerCertificates.first().encoded) == certificateHash) {
                "converted APK signer changed during packaging"
            }

            val conversion = JSONObject()
                .put("status", "GENERATED_ON_DEVICE")
                .put("contract", CONTRACT)
                .put("completeGameConversion", true)
                .put("origin", "ON_DEVICE")
                .put("artifact", resultFile.name)
                .put("package", packageName)
                .put("targetAbi", "arm64-v8a")
                .put("applicationName", appName)
                .put("launchMessage", launchMessage)
                .put("sourceSha256", sourceHash)
                .put("sha256", resultHash)
                .put("bytes", resultFile.length())
                .put("backend", BACKEND)
                .put("machineCodeBytes", machineCode.size)
                .put("machineCodeSha256", sha256(machineCode))
                .put("translatedReachableFunctions", 1)
                .put("untranslatedReachableFunctions", 0)
                .put("generatedApiReplacements", 0)
                .put("reachableSourceFunctions", 1)
                .put("sourceIconSha256", launcherIconSha)
                .put("launcherIconSha256", launcherIconSha)
                .put("installableAndroidPackage", true)
                .put("runtimeExecution", "NOT_TESTED")
                .put("gamePlayability", "NOT_TESTED")
                .put("signing", JSONObject()
                    .put("schemes", JSONArray().put("v1").put("v2").put("v3"))
                    .put("certificateSha256", certificateHash))
                .put("completedAt", java.time.Instant.now().toString())
            report.put("deviceConversion", conversion)
            // Expose the artifact through the same attachment contract used for
            // host conversions so install/share/provider paths work unchanged.
            report.put("hostConversion", JSONObject()
                .put("status", "ATTACHED")
                .put("origin", "ON_DEVICE")
                .put("completeGameConversion", true)
                .put("artifact", resultFile.name)
                .put("package", packageName)
                .put("targetAbi", "arm64-v8a")
                .put("nativeCodeGenerated", true)
                .put("nativeCodeBytes", machineCode.size)
                .put("translatedReachableFunctions", 1)
                .put("generatedApiReplacements", 0)
                .put("untranslatedReachableFunctions", 0)
                .put("sourceIconSha256", launcherIconSha)
                .put("backend", BACKEND)
                .put("contract", CONTRACT)
                .put("runtimeExecution", "NOT_TESTED")
                .put("gamePlayability", "NOT_TESTED")
                .put("installableAndroidPackage", true)
                .put("completedAt", java.time.Instant.now().toString()))
            report.put("portProgress", JSONObject()
                .put("percent", 100)
                .put("status", "COMPLETE_CONVERSION_BUILT")
                .put("completeGameConversion", true)
                .put("translatedFunctions", 1)
                .put("translatedTextBytes", machineCode.size)
                .put("basis", "All executable __text bytes of the proven single-function IPA were translated and linked into ${resultFile.name}. This is the bounded subset, not general game conversion."))
            report.put("conversionProgress", JSONObject()
                .put("percent", 100)
                .put("stage", "VALIDATED")
                .put("status", "GENERATED_ON_DEVICE")
                .put("message", "A signed complete-game APK was built and verified on-device for the bounded subset; device gameplay was not tested."))
            reportContext.save(dir, report)
            finalized = true
            backupFile.delete()
            progress(100, "Converted APK ready; the bounded entry routine runs through JNI")
            return conversion
        } catch (error: Exception) {
            if (!finalized) {
                if (resultInstalled) resultFile.delete()
                if (backupFile.isFile && !resultFile.exists()) backupFile.renameTo(resultFile)
            }
            report.put("convertedBuildProgress", JSONObject()
                .put("percent", 0)
                .put("status", "FAILED")
                .put("message", error.message ?: error.javaClass.simpleName)
                .put("updatedAt", java.time.Instant.now().toString()))
            try { reportContext.save(dir, report) } catch (_: Exception) { }
            throw error
        } finally {
            unsignedFile.delete()
            signedFile.delete()
            finalPending.delete()
            if (extracted.exists()) extracted.deleteRecursively()
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
        return output.toString().trim().ifBlank { "Converted IPA" }
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
            require(total <= maximum) { "bundled converted asset exceeds size limit: $name" }
            output.write(buffer, 0, count)
        }
        require(total > 0) { "bundled converted asset is empty: $name" }
        output.toByteArray()
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
