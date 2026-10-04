package dev.radek.conventor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipFile

class AlignedApkZipTest {
    @Test fun writesOnlyExpectedEntriesAndAlignsStoredPackagePayloads() {
        val stored = setOf("AndroidManifest.xml", "classes.dex", "resources.arsc", "res/drawable/icon.png", "assets/ipa-icon.png")
        val entries = listOf(
            AlignedApkZip.Entry("AndroidManifest.xml", ByteArray(17) { it.toByte() }),
            AlignedApkZip.Entry("classes.dex", ByteArray(113) { (it * 3).toByte() }),
            AlignedApkZip.Entry("resources.arsc", ByteArray(259) { (it * 7).toByte() }),
            AlignedApkZip.Entry("res/drawable/icon.png", ByteArray(31) { (it * 11).toByte() }),
            AlignedApkZip.Entry("assets/ipa-icon.png", ByteArray(43) { (it * 13).toByte() }),
            AlignedApkZip.Entry("assets/placeholder-info.json", "{\"placeholderOnly\":true}".toByteArray(), compressed = true),
        )
        val output = ByteArrayOutputStream()
        AlignedApkZip.write(output, entries)

        val directory = Files.createTempDirectory("aligned-apk-zip").toFile()
        try {
            val apk = directory.resolve("placeholder.apk").apply { writeBytes(output.toByteArray()) }
            AlignedApkZip.verify(apk, entries.map { it.name }.toSet(), stored.associateWith { AlignedApkZip.ALIGNMENT })
            ZipFile(apk).use { zip ->
                entries.forEach { expected ->
                    val actual = zip.getEntry(expected.name)
                    assertEquals(expected.name, if (expected.name in stored) ZipEntry.STORED else ZipEntry.DEFLATED, actual.method)
                    assertArrayEquals(expected.bytes, zip.getInputStream(actual).use { it.readBytes() })
                }
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test fun pageAlignsNativeLibrariesForDirectLoaderMmap() {
        val entries = listOf(
            AlignedApkZip.Entry("AndroidManifest.xml", ByteArray(17) { it.toByte() }),
            AlignedApkZip.Entry("classes.dex", ByteArray(113) { (it * 3).toByte() }),
            AlignedApkZip.Entry("classes2.dex", ByteArray(501) { (it * 5).toByte() }),
            AlignedApkZip.Entry("lib/arm64-v8a/libconverted.so", ByteArray(333) { (it * 13).toByte() },
                alignment = AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT),
        )
        val directory = Files.createTempDirectory("aligned-apk-zip-so").toFile()
        try {
            val apk = directory.resolve("converted.apk")
            AlignedApkZip.write(apk, entries)
            AlignedApkZip.verify(apk, entries.map { it.name }.toSet(), mapOf(
                "AndroidManifest.xml" to AlignedApkZip.ALIGNMENT,
                "classes.dex" to AlignedApkZip.ALIGNMENT,
                "classes2.dex" to AlignedApkZip.ALIGNMENT,
                "lib/arm64-v8a/libconverted.so" to AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT,
            ))
            // The .so payload must start at a multiple of the native-library
            // alignment inside the archive, so the linker can mmap it in place.
            val bytes = apk.readBytes()
            var offset = 0
            while (offset + 30 <= bytes.size) {
                require(bytes[offset] == 0x50.toByte() && bytes[offset + 1] == 0x4b.toByte())
                val nameLength = (bytes[offset + 26].toInt() and 0xff) or ((bytes[offset + 27].toInt() and 0xff) shl 8)
                val extraLength = (bytes[offset + 28].toInt() and 0xff) or ((bytes[offset + 29].toInt() and 0xff) shl 8)
                val name = String(bytes, offset + 30, nameLength, Charsets.UTF_8)
                val dataOffset = offset + 30 + nameLength + extraLength
                if (name == "lib/arm64-v8a/libconverted.so") {
                    assertEquals(0L, dataOffset.toLong() % AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT)
                    break
                }
                val size = (bytes[offset + 18].toInt() and 0xff).toLong() or
                    ((bytes[offset + 19].toInt() and 0xff).toLong() shl 8) or
                    ((bytes[offset + 20].toInt() and 0xff).toLong() shl 16) or
                    ((bytes[offset + 21].toInt() and 0xff).toLong() shl 24)
                offset = dataOffset + size.toInt()
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test fun rejectsDuplicateAndTraversalEntryNames() {
        assertThrows(IllegalArgumentException::class.java) {
            AlignedApkZip.write(ByteArrayOutputStream(), listOf(
                AlignedApkZip.Entry("classes.dex", byteArrayOf(1)),
                AlignedApkZip.Entry("classes.dex", byteArrayOf(2)),
            ))
        }
        assertThrows(IllegalArgumentException::class.java) {
            AlignedApkZip.write(ByteArrayOutputStream(), listOf(AlignedApkZip.Entry("../outside", byteArrayOf(1))))
        }
    }
}
