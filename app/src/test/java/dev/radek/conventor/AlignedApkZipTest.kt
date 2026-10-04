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
            AlignedApkZip.verify(apk, entries.map { it.name }.toSet(), stored)
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
