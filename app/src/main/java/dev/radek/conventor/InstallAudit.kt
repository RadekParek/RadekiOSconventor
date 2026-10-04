package dev.radek.conventor

import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Build
import java.io.File
import java.io.RandomAccessFile
import java.util.zip.ZipEntry
import java.util.zip.ZipFile

/**
 * Pre-flight check that a generated APK can actually be installed.
 *
 * Android's installer collapses every failure into the single sentence "app not
 * installed", which is useless for diagnosing a generated package. This audit
 * runs the structural checks the installer itself performs — parse, signing,
 * SDK levels, native-library layout — plus the one conflict the installer
 * cannot explain away: a package already installed under a different signing
 * certificate. Callers surface [Report.issues] instead of guessing.
 */
internal object InstallAudit {
    /** Android 14+ refuses to install an APK whose targetSdkVersion is below this. */
    const val MIN_INSTALLABLE_TARGET_SDK = 23

    data class Report(
        val packageName: String,
        /** Problems that make installation impossible; the APK must not be offered. */
        val blockers: List<String>,
        /** Problems the user can act on, e.g. an installed conflicting package. */
        val warnings: List<String>,
    ) {
        val installable: Boolean get() = blockers.isEmpty()
    }

    fun inspect(context: android.content.Context, apk: File, expectedPackage: String): Report {
        val blockers = ArrayList<String>()
        val warnings = ArrayList<String>()
        if (!apk.isFile || apk.length() <= 0) {
            return Report(expectedPackage, listOf("the APK result is missing"), warnings)
        }

        @Suppress("DEPRECATION")
        val parsed = context.packageManager.getPackageArchiveInfo(
            apk.path,
            PackageManager.GET_ACTIVITIES or PackageManager.GET_SIGNATURES,
        )
        if (parsed == null) {
            blockers += "Android could not parse this APK; the manifest or resource table is malformed"
            return Report(expectedPackage, blockers, warnings)
        }
        if (parsed.packageName != expectedPackage) {
            blockers += "the APK declares package ${parsed.packageName} but $expectedPackage was expected"
        }
        val application = parsed.applicationInfo
        if (application == null) {
            blockers += "Android parsed no <application> element from this APK"
        } else {
            if (application.targetSdkVersion < MIN_INSTALLABLE_TARGET_SDK) {
                blockers += "the APK targets SDK ${application.targetSdkVersion}; Android ${Build.VERSION.RELEASE} requires at least $MIN_INSTALLABLE_TARGET_SDK"
            }
            val minSdk = if (Build.VERSION.SDK_INT >= 24) application.minSdkVersion else 0
            if (minSdk > Build.VERSION.SDK_INT) {
                blockers += "the APK requires Android SDK $minSdk but this device is SDK ${Build.VERSION.SDK_INT}"
            }
            if (application.icon == 0) {
                warnings += "the APK declares no launcher icon; Android will substitute a default one"
            }
        }
        val activities = parsed.activities.orEmpty()
        if (activities.isEmpty()) {
            blockers += "Android parsed no activities from this APK, so it has nothing to launch"
        } else if (activities.none { it.exported }) {
            blockers += "no exported activity was parsed from this APK, so Android cannot launch it"
        }
        val declaredProviders = parsed.providers.orEmpty()
        if (declaredProviders.isNotEmpty()) {
            warnings += "the APK declares ${declaredProviders.size} content provider(s); a duplicate authority on this device blocks installation"
        }

        blockers += nativeLibraryIssues(apk)
        warnings += installedConflict(context, parsed, expectedPackage)
        if (!context.packageManager.canRequestPackageInstalls()) {
            warnings += "this app is not allowed to install packages yet; Android will ask for that permission first"
        }
        return Report(parsed.packageName, blockers, warnings)
    }

    /**
     * Native libraries must be stored uncompressed and page-aligned; a
     * compressed or misaligned library cannot be mmapped and, depending on the
     * device, aborts extraction with an opaque install failure.
     */
    private fun nativeLibraryIssues(apk: File): List<String> {
        val issues = ArrayList<String>()
        try {
            ZipFile(apk).use { zip ->
                val libraries = zip.entries().asSequence()
                    .filter { !it.isDirectory && it.name.startsWith("lib/") && it.name.count { c -> c == '/' } == 2 }
                    .toList()
                if (libraries.isEmpty()) return emptyList()
                // One pass over the local headers: a game bundle carries thousands
                // of assets, so re-scanning per library would be quadratic.
                val locations = localHeaderOffsets(apk, libraries.map { it.name }.toSet())
                RandomAccessFile(apk, "r").use { input ->
                    for (entry in libraries) {
                        val position = locations[entry.name] ?: continue
                        val dataOffset = position.offset + 30 + position.nameLength + position.extraLength
                        if (entry.method != ZipEntry.STORED) {
                            issues += "${entry.name} is compressed; Android native libraries must be stored uncompressed"
                        } else if (dataOffset % AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT != 0L) {
                            issues += "${entry.name} is not ${AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT}-byte aligned inside the APK"
                        } else if (entry.size < 64) {
                            issues += "${entry.name} is too small to be a valid ELF shared object"
                        } else {
                            input.seek(dataOffset)
                            val magic = ByteArray(4).also { input.readFully(it) }
                            if (!magic.contentEquals(byteArrayOf(0x7f, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte()))) {
                                issues += "${entry.name} is not a valid ELF shared object"
                            }
                        }
                    }
                }
            }
        } catch (_: Exception) {
            issues += "the APK could not be scanned for native libraries"
        }
        return issues
    }

    private data class LocalHeader(val offset: Long, val nameLength: Int, val extraLength: Int)

    private fun localHeaderOffsets(apk: File, wanted: Set<String>): Map<String, LocalHeader> {
        val found = LinkedHashMap<String, LocalHeader>()
        RandomAccessFile(apk, "r").use { input ->
            var offset = 0L
            while (offset + 30 <= input.length() && found.size < wanted.size) {
                input.seek(offset)
                if (readLe32(input) != 0x04034b50L) break
                input.seek(offset + 26)
                val nameLength = readLe16(input)
                val extraLength = readLe16(input)
                input.seek(offset + 30)
                val name = String(ByteArray(nameLength).also { input.readFully(it) }, Charsets.UTF_8)
                if (name in wanted) found[name] = LocalHeader(offset, nameLength, extraLength)
                input.seek(offset + 18)
                val compressedSize = readLe32(input)
                offset += 30 + nameLength + extraLength + compressedSize
            }
        }
        return found
    }

    private fun installedConflict(
        context: android.content.Context,
        parsed: PackageInfo,
        expectedPackage: String,
    ): List<String> {
        val installed = try {
            @Suppress("DEPRECATION")
            context.packageManager.getPackageInfo(expectedPackage, PackageManager.GET_SIGNATURES)
        } catch (_: Exception) {
            null
        } ?: return emptyList()
        val warnings = ArrayList<String>()
        val installedFingerprint = signatureFingerprint(installed)
        val builtFingerprint = signatureFingerprint(parsed)
        if (installedFingerprint != null && builtFingerprint != null && installedFingerprint != builtFingerprint) {
            warnings += "$expectedPackage is already installed with a different signing key; uninstall it before installing this build"
        } else {
            warnings += "$expectedPackage is already installed; Android will update it in place"
        }
        return warnings
    }

    private fun signatureFingerprint(info: PackageInfo): String? {
        @Suppress("DEPRECATION")
        val signatures = info.signatures ?: return null
        if (signatures.isEmpty()) return null
        return signatures.joinToString(",") { java.security.MessageDigest.getInstance("SHA-256")
            .digest(it.toByteArray()).joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) } }
            .take(64)
    }


    private fun readLe16(input: RandomAccessFile): Int {
        val low = input.readUnsignedByte()
        val high = input.readUnsignedByte()
        return low or (high shl 8)
    }

    private fun readLe32(input: RandomAccessFile): Long =
        readLe16(input).toLong() or (readLe16(input).toLong() shl 16)
}
