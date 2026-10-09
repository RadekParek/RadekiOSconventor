package dev.radek.conventor

import android.content.Context
import android.graphics.BitmapFactory
import com.android.apksig.ApkSigner
import com.android.apksig.ApkVerifier
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest

/**
 * Builds a signed, installable game-runtime boot-attempt APK on the device
 * (contract "game-runtime-v1").
 *
 * The APK embeds one authorized 32-bit ARM Mach-O main executable plus the
 * bundle resources, the tested guest-CPU runtime, and its shared Unicorn
 * dependency. Its launcher runs the real guest boot and stops at the first
 * actually-used unimplemented import while leaving a diagnostic screen open;
 * it never shows a preview, menu, or gameplay UI, and this builder never
 * claims a conversion, static recompilation, or playability.
 */
internal class GameRuntimeApkBuilder(private val context: Context) {
    companion object {
        private const val MAX_TEMPLATE_ENTRY_BYTES = 32L * 1024 * 1024
        private const val MAX_ICON_BYTES = 16L * 1024 * 1024
        internal const val MAX_RESOURCE_FILE_BYTES = 1024L * 1024 * 1024
        /** Report-size guard only: every payload is packaged, this caps the JSON inventory. */
        private const val MAX_REPORTED_RESOURCES = 20_000
        internal const val MAX_RESOURCE_FILES = 200_000
        private const val MAX_EXECUTABLE_BYTES = 256L * 1024 * 1024
        private const val MACHO_HEADER_BYTES = 64 * 1024
        private const val MAX_FAT_SLICES = 64
        private const val CPU_TYPE_ARM = 12
        private const val ENTRY_CLASS = "dev.radek.gameruntime.GameBootActivity"
        private const val JNI_SYMBOL = "Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt"
        private const val ABI = CompatibilityRuntime.ABI
        private const val BACKEND = "radek-device-gameruntime-v1"
        private const val CONTRACT = GameRuntimeArtifactContract.CONTRACT
        private const val EXECUTABLE_ASSET_PATH = "assets/gameboot/main-executable.bin"
        private val DEX_NAME_REGEX = Regex("""classes[0-9]+\.dex""")
        private val PNG_SIGNATURE = byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
    }

    private data class MachoSlice(
        val format: String,
        val cpuType: Int,
        val cpuSubtype: Int,
        val sliceCount: Int,
        val selectedSlice: Int,
        val file: File,
    )

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
            app.optString("name").ifBlank { app.optString("bundleId").substringAfterLast('.').ifBlank { "Game boot attempt" } },
        )
        val resultName = ArtifactNames.gameApkFileName(report)
        val resultFile = File(dir, resultName)
        require(resultFile.parentFile?.canonicalFile == dir.canonicalFile) { "invalid game-runtime result path" }

        val reportContext = Library(context)
        fun setProgress(percent: Int, status: String, message: String) {
            report.put(
                "gameRuntimeBuildProgress",
                JSONObject()
                    .put("percent", percent.coerceIn(0, 100))
                    .put("status", status)
                    .put("message", message)
                    .put("updatedAt", java.time.Instant.now().toString()),
            )
            reportContext.save(dir, report)
            progress(percent.coerceIn(0, 100), message)
        }

        val unsignedFile = File(dir, ".gameruntime-unsigned.apk")
        val signedFile = File(dir, ".gameruntime-signed.apk")
        val backupFile = File(dir, ".$resultName.gameruntime-backup")
        val finalPending = File(dir, ".$resultName.gameruntime-pending")
        val extracted = File(dir, "gameruntime-workspace")
        val staged = File(dir, "gameruntime-stage")
        val identity = PlaceholderSigningIdentity.loadOrCreate(
            File(context.noBackupFilesDir, "placeholder-apk-signing-identity.bin"),
        )
        val certificateHash = sha256(identity.certificate.encoded)
        var finalized = false
        var resultInstalled = false
        try {
            setProgress(4, "CONVERTING", "Re-checking the retained IPA before the game-runtime build")
            if (backupFile.isFile) {
                if (!resultFile.exists()) backupFile.renameTo(resultFile) else backupFile.delete()
            }
            unsignedFile.delete(); signedFile.delete(); finalPending.delete()
            if (extracted.exists()) extracted.deleteRecursively()
            if (staged.exists()) staged.deleteRecursively()
            staged.mkdirs()

            setProgress(12, "CONVERTING", "Extracting the authorized IPA for the game-runtime packager")
            SafeZip.extract(File(dir, "source.ipa"), extracted)
            val apps = File(extracted, "Payload").listFiles().orEmpty().filter { it.isDirectory && it.name.endsWith(".app") }
            require(apps.size == 1) { "expected exactly one Payload/*.app" }
            val appDir = apps.single()
            val plist = Plist.read(File(appDir, "Info.plist").readBytes())
            val executableName = plist["CFBundleExecutable"] as? String ?: error("CFBundleExecutable missing")
            require(SafeZip.validateName(executableName) == executableName && '/' !in executableName)
            val binary = File(appDir, executableName)
            require(binary.isFile && binary.length() in 1..MAX_EXECUTABLE_BYTES) { "executable missing or exceeds the on-device 256 MiB limit" }

            setProgress(24, "CONVERTING", "Selecting the 32-bit ARM Mach-O slice for the boot attempt")
            val slice = selectArmSlice(binary, File(staged, "executable"))
            val executableSha = sha256(slice.file)

            setProgress(36, "CONVERTING", "Collecting and hashing the bundle resources")
            val inventory = JSONArray()
            val resourceEntries = ArrayList<Pair<String, File>>()
            var totalResourceBytes = 0L
            var resourceCount = 0
            // Payloads stay in the extracted tree and are streamed from there
            // straight into the archive: no second on-disk copy, and nothing
            // held in RAM. The old fixed total cap rejected larger games; the
            // only remaining bound is the device's own free storage, checked
            // below before the archive is written.
            appDir.walkTopDown().filter { it.isFile }.sortedBy { it.path }.forEach { file ->
                if (file == binary) return@forEach
                val relative = file.relativeTo(appDir).path.replace(File.separatorChar, '/')
                SafeZip.validateName(relative)
                require(++resourceCount <= MAX_RESOURCE_FILES) {
                    "too many bundle resources for the game-runtime packager ($resourceCount > $MAX_RESOURCE_FILES)"
                }
                require(file.length() <= MAX_RESOURCE_FILE_BYTES) { "bundle resource exceeds the limit: $relative" }
                totalResourceBytes += file.length()
                if (inventory.length() < MAX_REPORTED_RESOURCES) {
                    inventory.put(JSONObject().put("path", relative).put("sha256", sha256(file)))
                }
                resourceEntries += relative to file
            }
            if (resourceEntries.isEmpty()) error("the bundle contains no resources to package")
            SafeZip.requireStorage(dir, totalResourceBytes)

            setProgress(48, "CONVERTING", "Staging the tested guest-CPU boot-attempt runtime")
            val compatibilityLibraries = CompatibilityRuntime.extractGameRuntimeInstalled(context, File(staged, "runtime"))
            require(compatibilityLibraries.any { it.soname == CompatibilityRuntime.GAMERUNTIME_SONAME }) {
                "the tested guest-CPU boot-attempt runtime could not be staged"
            }

            setProgress(56, "PACKAGING", "Reading the bundled game-runtime template")
            val templateManifest = readAsset("gameruntime-template/AndroidManifest.xml", 2L * 1024 * 1024)
            val templateResources = readAsset("gameruntime-template/resources.arsc", MAX_TEMPLATE_ENTRY_BYTES)
            // AGP splits even the tiny launcher template into classes.dex +
            // classesN.dex; Android loads every classesN.dex at the APK root, so
            // all of them are embedded verbatim (manifest written by the embed task).
            val templateDexNames = readAsset("gameruntime-template/template-dex-entries.txt", 16 * 1024)
                .toString(Charsets.UTF_8).lineSequence().map { it.trim() }.filter { it.isNotBlank() }
                .map { it.substringBefore(':') }
                .filter { it == "classes.dex" || DEX_NAME_REGEX.matches(it) }
                .sortedWith(compareBy<String>({ it != "classes.dex" }, { it.length }, { it }))
                .distinct()
                .toList()
            require(templateDexNames.firstOrNull() == "classes.dex") {
                "game-runtime template DEX manifest is missing classes.dex"
            }
            val templateDexes = templateDexNames.map { name ->
                name to readAsset("gameruntime-template/$name", MAX_TEMPLATE_ENTRY_BYTES)
            }
            val templateFallbackIcon = readAsset("gameruntime-template/fallback-icon.png", MAX_ICON_BYTES)
            val iconEntryPath = SafeZip.validateName(
                readAsset("gameruntime-template/icon-entry-path.txt", 1024).toString(Charsets.UTF_8),
            )
            require(templateDexes.all { (_, bytes) -> validDex(bytes) } &&
                templateFallbackIcon.size >= PNG_SIGNATURE.size &&
                templateFallbackIcon.copyOfRange(0, PNG_SIGNATURE.size).contentEquals(PNG_SIGNATURE) &&
                validPng(templateFallbackIcon)) {
                "game-runtime template is missing a valid launcher DEX or fallback icon"
            }
            val templateDexText = templateDexes.joinToString("") { (_, bytes) -> String(bytes, Charsets.ISO_8859_1) }
            require(templateDexText.contains("Ldev/radek/gameruntime/GameBootActivity;")) {
                "game-runtime template DEX files do not define the boot launcher entry class"
            }
            require(iconEntryPath.startsWith("res/") &&
                iconEntryPath.substringAfterLast('/') == "generated_gameruntime_icon.png") {
                "game-runtime template icon resource path is invalid"
            }

            // The package id carries the signing certificate's hash, exactly like
            // the other generated APKs. Android rejects an update whose certificates
            // changed; binding the id to the key means a regenerated identity
            // produces a fresh package instead of the installer's opaque
            // "app not installed" SIGNATURE_MISMATCH.
            val packageName = "${GameRuntimeArtifactContract.PACKAGE_PREFIX}${sourceHash.take(20)}${certificateHash.take(8)}"
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
                .put("applicationName", appName)
                .put("source", JSONObject()
                    .put("sha256", sourceHash)
                    .put("originalName", source.optString("originalName"))
                    .put("bundleId", app.optString("bundleId")))
                .put("executable", JSONObject()
                    .put("name", executableName)
                    .put("sha256", executableSha)
                    .put("bytes", slice.file.length())
                    .put("machoFormat", slice.format)
                    .put("cpuType", slice.cpuType)
                    .put("cpuSubtype", slice.cpuSubtype)
                    .put("fatSliceCount", slice.sliceCount)
                    .put("selectedSlice", slice.selectedSlice))
                .put("bootAttempt", JSONObject()
                    .put("backend", BACKEND)
                    .put("entryActivity", ENTRY_CLASS)
                    .put("jniSymbol", JNI_SYMBOL)
                    .put("behavior", "Shows up to three recovered splash frames for 900 ms each before the guest starts; then runs the real guest boot and keeps diagnostics on the first runtime boundary. No preview, menu, or gameplay UI is claimed."))
                .put("storage", JSONObject()
                    .put("appDataBase", "Context.getExternalFilesDir(null) (Android/data/<package>/files)")
                    .put("directoriesCreatedAtLaunch", JSONArray()
                        .put("Documents").put("Library").put("Library/Caches")
                        .put("tmp").put("diagnostics"))
                    .put("obbDirectoryCreatedAtLaunch", "Context.getObbDir()")
                    .put("bundleAssetsInApk", true)
                    .put("expansionObbRequired", false)
                    .put("runtimeReport", "diagnostics/runtime-report.json")
                    .put("persistentLog", "diagnostics/boot.log"))
                .put("compatibilityRuntime", JSONObject()
                    .put("status", "SHIPPED_IN_APK_LIB")
                    .put("libraries", JSONArray().apply {
                        compatibilityLibraries.forEach { library ->
                            put(JSONObject().put("soname", library.soname).put("sha256", library.sha256))
                        }
                    }))
                .put("resourceInventory", inventory)
                .put("resources", JSONObject()
                    .put("files", resourceCount)
                    .put("bytes", totalResourceBytes)
                    .put("inventoryEntries", inventory.length())
                    .put("inventoryTruncated", inventory.length() < resourceCount))

            setProgress(68, "PACKAGING", "Assembling the game-runtime APK entries")
            val entries = ArrayList<AlignedApkZip.Entry>()
            entries += AlignedApkZip.Entry("AndroidManifest.xml", manifest)
            templateDexes.forEach { (name, bytes) -> entries += AlignedApkZip.Entry(name, bytes) }
            entries += AlignedApkZip.Entry("resources.arsc", resources)
            entries += AlignedApkZip.Entry(iconEntryPath, launcherIcon)
            compatibilityLibraries.forEach { library ->
                entries += AlignedApkZip.Entry.stream(
                    library.apkPath,
                    library.file,
                    compressed = false,
                    alignment = AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT,
                )
            }
            entries += AlignedApkZip.Entry("assets/gameboot.json", metadata.toString().toByteArray(Charsets.UTF_8), compressed = true)
            entries += AlignedApkZip.Entry.stream(EXECUTABLE_ASSET_PATH, slice.file, compressed = true)
            if (recoveredIcon != null) {
                entries += AlignedApkZip.Entry("assets/ipa-icon.png", recoveredIcon, compressed = true)
            }
            for ((relative, payload) in resourceEntries) {
                entries += AlignedApkZip.Entry.stream("assets/bundle/$relative", payload, compressed = true)
            }
            val expectedNames = entries.map { it.name }.toSet()
            val alignedNames = buildMap<String, Int> {
                put("AndroidManifest.xml", AlignedApkZip.ALIGNMENT)
                put("resources.arsc", AlignedApkZip.ALIGNMENT)
                put(iconEntryPath, AlignedApkZip.ALIGNMENT)
                compatibilityLibraries.forEach { put(it.apkPath, AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT) }
                templateDexNames.forEach { put(it, AlignedApkZip.ALIGNMENT) }
            }
            AlignedApkZip.write(unsignedFile, entries)
            require(unsignedFile.isFile && unsignedFile.length() > 0) { "could not assemble the game-runtime APK" }
            AlignedApkZip.verify(unsignedFile, expectedNames, alignedNames)

            setProgress(80, "SIGNING", "Signing the game-runtime APK for Android installation")
            val signerConfig = ApkSigner.SignerConfig.Builder(
                "RadekiOS game-runtime",
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
                // apksig 8.7+ otherwise rewrites ZIP alignment padding while
                // signing. Preserve the validated 16 KiB offsets in the signed APK.
                .setAlignmentPreserved(true)
                .setLibraryPageAlignmentBytes(AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT)
                .build()
                .sign()
            require(signedFile.isFile && signedFile.length() > 0) { "APK signing produced no output" }
            AlignedApkZip.verifyAlignedEntries(
                signedFile,
                alignedNames.filterKeys { it.startsWith("lib/") },
            )

            setProgress(90, "VERIFYING", "Checking the game-runtime APK signature and package structure")
            val verification = ApkVerifier.Builder(signedFile).build().verify()
            require(verification.isVerified) {
                "game-runtime APK signature verification failed: ${verification.errors.joinToString("; ")}"
            }
            require(verification.signerCertificates.isNotEmpty()) { "game-runtime APK has no signer certificate" }
            @Suppress("DEPRECATION")
            val packageInfo = context.packageManager.getPackageArchiveInfo(
                signedFile.path,
                android.content.pm.PackageManager.GET_ACTIVITIES or android.content.pm.PackageManager.GET_SIGNATURES,
            ) ?: error("Android could not parse the game-runtime APK")
            require(packageInfo.packageName == packageName) { "Android parsed an unexpected game-runtime package id" }
            require(packageInfo.activities.orEmpty().any { it.name == ENTRY_CLASS }) {
                "game-runtime boot activity is missing from the parsed package"
            }
            require(packageInfo.applicationInfo?.loadLabel(context.packageManager)?.toString() == appName) {
                "Android did not parse the IPA application name from the game-runtime manifest"
            }

            require(signedFile.copyTo(finalPending, overwrite = true).isFile) { "could not stage the game-runtime APK" }
            if (resultFile.exists()) {
                require(!backupFile.exists() || backupFile.delete()) { "cannot remove stale game-runtime backup" }
                require(resultFile.renameTo(backupFile)) { "cannot preserve the previous game-runtime APK" }
            }
            require(finalPending.renameTo(resultFile)) { "could not save the game-runtime APK" }
            resultInstalled = true
            val resultHash = sha256(resultFile)
            require(sha256(verification.signerCertificates.first().encoded) == certificateHash) {
                "game-runtime APK signer changed during packaging"
            }
            val audit = InstallAudit.inspect(context, resultFile, packageName)
            require(audit.blockers.isEmpty()) {
                "game-runtime APK would not install: ${audit.blockers.joinToString("; ")}"
            }

            val conversion = JSONObject()
                .put("status", "GENERATED")
                .put("contract", CONTRACT)
                .put("bootAttemptIncluded", true)
                .put("completeGameConversion", false)
                .put("gamePlayable", false)
                .put("gameCodeRecompiled", false)
                .put("origin", "ON_DEVICE")
                .put("artifact", resultFile.name)
                .put("package", packageName)
                .put("targetAbi", "arm64-v8a")
                .put("applicationName", appName)
                .put("sourceSha256", sourceHash)
                .put("sha256", resultHash)
                .put("bytes", resultFile.length())
                .put("backend", BACKEND)
                .put("executableName", executableName)
                .put("resources", JSONObject()
                    .put("files", resourceCount)
                    .put("bytes", totalResourceBytes))
                .put("executableBytes", slice.file.length())
                .put("executableSha256", executableSha)
                .put("machoFormat", slice.format)
                .put("executableCpuType", slice.cpuType)
                .put("linkedRuntimeLibraries", JSONArray().apply {
                    compatibilityLibraries.forEach { put(it.soname) }
                })
                .put("compatibilityRuntimeLinked", true)
                .put("sourceIconSha256", launcherIconSha)
                .put("launcherIconSha256", launcherIconSha)
                .put("installableAndroidPackage", true)
                .put("runtimeExecution", "NOT_TESTED")
                .put("gamePlayability", "NOT_TESTED")
                .put("bootBehavior", "ATTEMPTS_GUEST_BOOT_WITH_UNLIMITED_DEVICE_EXECUTION_THEN_LEAVES_DIAGNOSTICS_OPEN_AT_REAL_RUNTIME_BOUNDARY")
                .put("signing", JSONObject()
                    .put("schemes", JSONArray().put("v1").put("v2").put("v3"))
                    .put("certificateSha256", certificateHash))
                .put("installAudit", JSONObject()
                    .put("installable", audit.installable)
                    .put("warnings", JSONArray().apply { audit.warnings.forEach { put(it) } }))
                .put("completedAt", java.time.Instant.now().toString())
            report.put("gameRuntimeConversion", conversion)
            reportContext.save(dir, report)
            finalized = true
            backupFile.delete()
            progress(100, "Game-runtime APK ready; device execution is unlimited and diagnostics remain available at a real runtime boundary")
            return conversion
        } catch (error: Throwable) {
            // An OutOfMemoryError is an Error, not an Exception: catching it
            // here is what stops a large bundle from killing the app instead of
            // reporting a failure the user can act on.
            if (!finalized) {
                if (resultInstalled) resultFile.delete()
                if (backupFile.isFile && !resultFile.exists()) backupFile.renameTo(resultFile)
            }
            val failure = if (error is OutOfMemoryError) {
                "The device ran out of memory while packaging this bundle; free space and retry."
            } else {
                error.message ?: error.javaClass.simpleName
            }
            report.put("gameRuntimeBuildProgress", JSONObject()
                .put("percent", 0)
                .put("status", "FAILED")
                .put("message", failure)
                .put("updatedAt", java.time.Instant.now().toString()))
            try { reportContext.save(dir, report) } catch (_: Throwable) { }
            if (error is OutOfMemoryError) throw IllegalStateException(failure)
            throw error
        } finally {
            unsignedFile.delete()
            signedFile.delete()
            finalPending.delete()
            if (extracted.exists()) extracted.deleteRecursively()
            if (staged.exists()) staged.deleteRecursively()
        }
    }

    /**
     * Validates the main executable header and stages the bytes the boot
     * attempt will run: the whole file for thin Mach-O, or the first 32-bit
     * ARM slice of a FAT binary. FAT slices are copied as a byte range without
     * ever buffering the full executable in memory.
     */
    private fun selectArmSlice(binary: File, destination: File): MachoSlice {
        destination.parentFile?.mkdirs()
        val header = ByteArray(MACHO_HEADER_BYTES)
        val headerLength = binary.inputStream().use { input ->
            var total = 0
            while (total < header.size) {
                val count = input.read(header, total, header.size - total)
                if (count < 0) break
                total += count
            }
            total
        }
        require(headerLength >= 8) { "executable is too small to be a Mach-O image" }
        // Magics are compared as unsigned 32-bit values in Long: the raw
        // patterns do not fit a signed Int literal.
        val magic = u32le(header, 0).toLong() and 0xFFFFFFFFL
        if (magic == 0xFEEDFACEL || magic == 0xFEEDFACFL) {
            require(headerLength >= 28) { "thin Mach-O header is truncated" }
            val cpuType = u32le(header, 4)
            require(cpuType == CPU_TYPE_ARM) {
                "thin Mach-O cpu type $cpuType is outside the 32-bit ARM boot-attempt subset"
            }
            binary.copyTo(destination, overwrite = true)
            require(destination.length() == binary.length()) { "executable could not be staged" }
            return MachoSlice(
                format = if (magic == 0xFEEDFACEL) "thin-macho32" else "thin-macho64",
                cpuType = cpuType,
                cpuSubtype = u32le(header, 8),
                sliceCount = 1,
                selectedSlice = 0,
                file = destination,
            )
        }
        val magicBe = u32be(header, 0).toLong() and 0xFFFFFFFFL
        require(magicBe == 0xCAFEBABEL || magicBe == 0xCAFEBABFL) {
            "executable is not a Mach-O or FAT image (magic ${"%08x".format(magic)})"
        }
        val fat64 = magicBe == 0xCAFEBABFL
        val sliceCount = u32be(header, 4).toLong() and 0xFFFFFFFFL
        require(sliceCount in 1L..MAX_FAT_SLICES.toLong()) { "FAT slice count $sliceCount is invalid" }
        val recordBytes = if (fat64) 32 else 20
        require(headerLength >= 8 + sliceCount * recordBytes.toLong()) { "FAT header is truncated" }
        var selected = -1
        var sliceOffset = 0L
        var sliceSize = 0L
        var sliceSubtype = 0
        for (index in 0 until sliceCount.toInt()) {
            val base = 8 + index * recordBytes
            val cpuType = u32be(header, base).toLong() and 0xFFFFFFFFL
            if (cpuType == CPU_TYPE_ARM.toLong()) {
                selected = index
                sliceSubtype = u32be(header, base + 4)
                sliceOffset = if (fat64) u64be(header, base + 8) else u32be(header, base + 8).toLong()
                sliceSize = if (fat64) u64be(header, base + 16) else u32be(header, base + 12).toLong()
                break
            }
        }
        require(selected >= 0) { "FAT image has no 32-bit ARM slice for the boot attempt" }
        require(sliceSize in 1..MAX_EXECUTABLE_BYTES && sliceOffset >= 0 &&
            sliceOffset + sliceSize <= binary.length()) {
            "FAT ARM slice range is invalid"
        }
        RandomAccessFile(binary, "r").use { input ->
            destination.outputStream().use { output ->
                input.seek(sliceOffset)
                val buffer = ByteArray(65536)
                var remaining = sliceSize
                while (remaining > 0) {
                    val count = input.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                    require(count > 0) { "FAT ARM slice is truncated" }
                    output.write(buffer, 0, count)
                    remaining -= count
                }
            }
        }
        require(destination.length() == sliceSize) { "FAT ARM slice could not be staged" }
        return MachoSlice(
            format = if (fat64) "fat64" else "fat32",
            cpuType = CPU_TYPE_ARM,
            cpuSubtype = sliceSubtype,
            sliceCount = sliceCount.toInt(),
            selectedSlice = selected,
            file = destination,
        )
    }

    private fun u32le(bytes: ByteArray, offset: Int): Int =
        (bytes[offset].toInt() and 0xff) or
            ((bytes[offset + 1].toInt() and 0xff) shl 8) or
            ((bytes[offset + 2].toInt() and 0xff) shl 16) or
            ((bytes[offset + 3].toInt() and 0xff) shl 24)

    private fun u32be(bytes: ByteArray, offset: Int): Int =
        ((bytes[offset].toInt() and 0xff) shl 24) or
            ((bytes[offset + 1].toInt() and 0xff) shl 16) or
            ((bytes[offset + 2].toInt() and 0xff) shl 8) or
            (bytes[offset + 3].toInt() and 0xff)

    private fun u64be(bytes: ByteArray, offset: Int): Long =
        ((bytes[offset].toLong() and 0xff) shl 56) or
            ((bytes[offset + 1].toLong() and 0xff) shl 48) or
            ((bytes[offset + 2].toLong() and 0xff) shl 40) or
            ((bytes[offset + 3].toLong() and 0xff) shl 32) or
            ((bytes[offset + 4].toLong() and 0xff) shl 24) or
            ((bytes[offset + 5].toLong() and 0xff) shl 16) or
            ((bytes[offset + 6].toLong() and 0xff) shl 8) or
            (bytes[offset + 7].toLong() and 0xff)

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
        return output.toString().trim().ifBlank { "Game boot attempt" }
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
            require(total <= maximum) { "bundled game-runtime asset exceeds size limit: $name" }
            output.write(buffer, 0, count)
        }
        require(total > 0) { "bundled game-runtime asset is empty: $name" }
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
