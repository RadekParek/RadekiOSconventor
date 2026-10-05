package dev.radek.compat.runtime

import android.content.Context
import android.net.Uri
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.util.zip.ZipInputStream

/** Runtime import entry point. IPA bytes are read from the user's selected source, never bundled. */
object RuntimeBridge {
    private const val MAX_MAIN_EXECUTABLE_BYTES = 256L * 1024L * 1024L
    private const val MAX_EXPANDED_IPA_BYTES = 1024L * 1024L * 1024L

    init {
        System.loadLibrary("compat_runtime_v1")
    }

    /** Native report for a single main executable extracted from the user's IPA at runtime. */
    external fun runAuthorizedMainBinary(mainBinary: ByteArray, authorizationConfirmed: Boolean): String

    /** Constructs a fail-closed report for an IPA import error without starting guest code. */
    external fun reportImportBlocked(message: String, authorizationConfirmed: Boolean): String

    /**
     * Reads an app-private or picker-provided URI at runtime and extracts only the caller-selected
     * main executable. The caller must obtain the path from the selected app's bundle metadata.
     */
    fun runAuthorizedIpa(
        context: Context,
        ipaUri: Uri,
        mainExecutableEntry: String,
        authorizationConfirmed: Boolean,
    ): String {
        if (!authorizationConfirmed) {
            return reportImportBlocked("User authorization was not confirmed.", false)
        }
        return try {
            val input = context.contentResolver.openInputStream(ipaUri)
                ?: return reportImportBlocked("The selected IPA could not be opened.", true)
            input.use { stream -> runAuthorizedIpa(stream, mainExecutableEntry, true) }
        } catch (error: Exception) {
            reportImportBlocked("IPA import failed closed: ${error.message ?: error.javaClass.simpleName}", true)
        }
    }

    /** Stream-oriented import path for clients that already own an authorized input stream. */
    fun runAuthorizedIpa(
        ipaInput: InputStream,
        mainExecutableEntry: String,
        authorizationConfirmed: Boolean,
    ): String {
        if (!authorizationConfirmed) {
            return reportImportBlocked("User authorization was not confirmed.", false)
        }
        if (!isAppMainExecutableEntry(mainExecutableEntry)) {
            return reportImportBlocked(
                "The requested archive entry is not a Payload app main executable.", true,
            )
        }
        return try {
            val executable = extractMainExecutable(ipaInput, mainExecutableEntry)
            runAuthorizedMainBinary(executable, true)
        } catch (error: Exception) {
            reportImportBlocked("IPA main executable import failed closed: ${error.message ?: error.javaClass.simpleName}", true)
        }
    }

    private fun isAppMainExecutableEntry(entry: String): Boolean {
        if (entry.length > 1024 || !entry.startsWith("Payload/")) return false
        if (entry.split('/').any { it.isEmpty() || it == "." || it == ".." }) return false
        val components = entry.split('/')
        return components.size == 3 && components[1].endsWith(".app")
    }

    private fun extractMainExecutable(ipaInput: InputStream, requestedEntry: String): ByteArray {
        ZipInputStream(BufferedInputStream(ipaInput)).use { archive ->
            var expandedBytes = 0L
            val buffer = ByteArray(32 * 1024)
            while (true) {
                val entry = archive.nextEntry ?: break
                if (entry.isDirectory) {
                    archive.closeEntry()
                    continue
                }
                val isMainExecutable = entry.name == requestedEntry
                val output = if (isMainExecutable) {
                    if (entry.size > MAX_MAIN_EXECUTABLE_BYTES) {
                        throw IllegalArgumentException("main executable exceeds the 256 MiB runtime input limit")
                    }
                    ByteArrayOutputStream(
                        if (entry.size in 1..MAX_MAIN_EXECUTABLE_BYTES) entry.size.toInt() else 32 * 1024,
                    )
                } else {
                    null
                }
                var entryBytes = 0L
                while (true) {
                    val read = archive.read(buffer)
                    if (read < 0) break
                    entryBytes += read.toLong()
                    expandedBytes += read.toLong()
                    if (expandedBytes > MAX_EXPANDED_IPA_BYTES) {
                        throw IllegalArgumentException("expanded IPA exceeds the 1 GiB import safety limit")
                    }
                    if (isMainExecutable && entryBytes > MAX_MAIN_EXECUTABLE_BYTES) {
                        throw IllegalArgumentException("main executable exceeds the 256 MiB runtime input limit")
                    }
                    output?.write(buffer, 0, read)
                }
                archive.closeEntry()
                if (isMainExecutable) {
                    val executable = output!!.toByteArray()
                    if (executable.isEmpty()) {
                        throw IllegalArgumentException("IPA main executable is empty")
                    }
                    return executable
                }
            }
        }
        throw IllegalArgumentException("the selected main executable entry was not found in the IPA")
    }
}
