package dev.radek.conventor

import android.content.Context
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.zip.ZipFile

/**
 * Copies reviewed ARM64 compatibility libraries from the converter into generated APKs.
 *
 * The bounded converter packages libioscompat.so. The game boot-attempt package
 * also copies libcompat_runtime_v1.so, which statically links the Dynarmic
 * ARM32 execution backend; Android resolves these libraries from the generated
 * APK's native library directory, not from an analyzer-only path.
 */
internal object CompatibilityRuntime {
    const val ABI = "arm64-v8a"
    const val SONAME = "libioscompat.so"
    /** Guest-CPU boot-attempt runtime (contract "game-runtime-v1") shipped by :compat-runtime-v1. */
    const val GAMERUNTIME_SONAME = "libcompat_runtime_v1.so"
    private const val LIBCXX_SONAME = "libc++_shared.so"
    private const val MAX_LIBRARY_BYTES = 128L * 1024 * 1024
    private const val ELF_HEADER_BYTES = 64
    private const val EM_AARCH64 = 183

    data class Library(
        val soname: String,
        val apkPath: String,
        val file: File,
        val sha256: String,
    )

    fun extractInstalled(context: Context, destination: File): List<Library> {
        val info = context.applicationInfo
        val installedApks = listOfNotNull(info.sourceDir?.takeIf { it.isNotBlank() }?.let(::File)) +
            info.splitSourceDirs.orEmpty().map(::File)
        return extractFromApks(installedApks.distinctBy { it.absolutePath }, destination)
    }

    /** Package-private visibility keeps extraction directly regression-testable. */
    internal fun extractFromApk(sourceApk: File, destination: File): List<Library> =
        extractFromApks(listOf(sourceApk), destination)

    /**
     * Copies the reviewed ARM64 guest-CPU runtime (with its statically linked
     * Dynarmic backend) into generated game APKs. libioscompat remains
     * exclusive to the bounded converter path.
     */
    fun extractGameRuntimeInstalled(context: Context, destination: File): List<Library> {
        val info = context.applicationInfo
        val installedApks = listOfNotNull(info.sourceDir?.takeIf { it.isNotBlank() }?.let(::File)) +
            info.splitSourceDirs.orEmpty().map(::File)
        return extractGameRuntimeFromApks(installedApks.distinctBy { it.absolutePath }, destination)
    }

    /** Package-private visibility keeps game-runtime extraction directly regression-testable. */
    internal fun extractGameRuntimeFromApks(sourceApks: List<File>, destination: File): List<Library> =
        extractSonames(
            sourceApks,
            destination,
            listOf(GAMERUNTIME_SONAME, LIBCXX_SONAME),
            setOf(GAMERUNTIME_SONAME),
        )

    /** Handles App Bundle installs where native libraries live in ABI split APKs. */
    internal fun extractFromApks(sourceApks: List<File>, destination: File): List<Library> =
        extractSonames(sourceApks, destination, listOf(SONAME, LIBCXX_SONAME), setOf(SONAME))

    private fun extractSonames(
        sourceApks: List<File>,
        destination: File,
        libraryNames: List<String>,
        requiredSonames: Set<String>,
    ): List<Library> {
        val availableApks = sourceApks.filter { it.isFile && it.length() > 0 }
        require(availableApks.isNotEmpty()) { "installed app APK is missing" }
        require(destination.isDirectory || destination.mkdirs()) { "cannot create compatibility runtime staging directory" }
        val output = ArrayList<Library>()
        for (soname in libraryNames) {
            val entryName = "lib/$ABI/$soname"
            val archive = availableApks.firstNotNullOfOrNull { apk ->
                ZipFile(apk).use { zip ->
                    zip.getEntry(entryName)?.takeIf { !it.isDirectory }?.let { apk }
                }
            }
            if (archive == null) {
                if (soname in requiredSonames) {
                    error("the app package is missing required native library $soname")
                }
                continue
            }
            ZipFile(archive).use { zip ->
                val entry = zip.getEntry(entryName)?.takeIf { !it.isDirectory }
                    ?: error("$entryName disappeared while staging the compatibility runtime")
                require(entry.size in 64..MAX_LIBRARY_BYTES) {
                    "$entryName is empty, truncated, or exceeds the compatibility-runtime size limit"
                }
                val file = File(destination, soname)
                try {
                    zip.getInputStream(entry).use { input ->
                        FileOutputStream(file).use { stream ->
                            val buffer = ByteArray(64 * 1024)
                            var copied = 0L
                            while (true) {
                                val count = input.read(buffer)
                                if (count < 0) break
                                copied += count
                                require(copied <= MAX_LIBRARY_BYTES) { "$entryName exceeds the compatibility-runtime size limit" }
                                stream.write(buffer, 0, count)
                            }
                            require(copied == entry.size) { "$entryName was truncated while extracting" }
                        }
                    }
                    validateArm64SharedObject(file, soname)
                    output += Library(
                        soname = soname,
                        apkPath = "lib/$ABI/$soname",
                        file = file,
                        sha256 = sha256(file),
                    )
                } catch (error: Exception) {
                    file.delete()
                    throw error
                }
            }
        }
        return output
    }

    private fun validateArm64SharedObject(file: File, name: String) {
        RandomAccessFile(file, "r").use { input ->
            require(input.length() >= ELF_HEADER_BYTES) { "$name is too small to be an ELF shared object" }
            val ident = ByteArray(20)
            input.readFully(ident)
            require(ident.copyOfRange(0, 4).contentEquals(byteArrayOf(0x7f, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte())) &&
                ident[4].toInt() == 2 && ident[5].toInt() == 1) {
                "$name is not a little-endian ELF64 library"
            }
            val elfType = (ident[16].toInt() and 0xff) or ((ident[17].toInt() and 0xff) shl 8)
            val machine = (ident[18].toInt() and 0xff) or ((ident[19].toInt() and 0xff) shl 8)
            require(elfType == 3 && machine == EM_AARCH64) { "$name is not an AArch64 ELF shared library" }
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
