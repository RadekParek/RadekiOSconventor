package dev.radek.conventor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
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

    /**
     * A ZIP extra field length is a 16-bit field. Padding large enough to
     * overflow it used to be written truncated, which leaves the local file
     * header disagreeing with the bytes that follow it; Android reads the local
     * header, so the APK stopped installing. Every padding the aligner can
     * produce must stay inside the limit and parse as whole TLV records.
     */
    @Test fun keepsEveryAlignmentPaddingInsideTheZipExtraFieldLimit() {
        val alignment = AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT
        val payload = ByteArray(64) { (it * 7).toByte() }
        val paddings = listOf(0, 1, 2, 3, 4, 5, 6, 7, 8, 31, 32, 33) +
            listOf(alignment - 8, alignment - 7, alignment - 6, alignment - 5, alignment - 4,
                alignment - 3, alignment - 2, alignment - 1) +
            (0 until alignment step 337)
        val directory = Files.createTempDirectory("aligned-apk-zip-padding").toFile()
        try {
            for (padding in paddings.distinct()) {
                val entries = listOf(
                    AlignedApkZip.Entry("pad.bin", ByteArray(padding + 1) { 0x5a.toByte() }),
                    AlignedApkZip.Entry("lib/arm64-v8a/libconverted.so", payload, alignment = alignment),
                )
                val apk = directory.resolve("padding-$padding.apk")
                AlignedApkZip.write(apk, entries)
                AlignedApkZip.verify(apk, entries.map { it.name }.toSet(),
                    mapOf("lib/arm64-v8a/libconverted.so" to alignment))
                assertExtraFieldRecordsAreWhole(apk, "lib/arm64-v8a/libconverted.so", alignment)
                ZipFile(apk).use { zip ->
                    assertArrayEquals(payload, zip.getInputStream(zip.getEntry("lib/arm64-v8a/libconverted.so")).readBytes())
                }
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    private fun assertExtraFieldRecordsAreWhole(apk: File, name: String, alignment: Int) {
        RandomAccessFile(apk, "r").use { input ->
            var offset = 0L
            while (offset + 30 <= input.length()) {
                input.seek(offset)
                val signature = ByteArray(4).also { input.readFully(it) }
                require(signature.contentEquals(byteArrayOf(0x50, 0x4b, 0x03, 0x04))) {
                    "no local file header at $offset in $name"
                }
                input.seek(offset + 26)
                val nameLength = readU16(input)
                val extraLength = readU16(input)
                input.seek(offset + 30)
                val entryName = String(ByteArray(nameLength).also { input.readFully(it) }, Charsets.UTF_8)
                if (entryName == name) {
                    require((offset + 30L + nameLength + extraLength) % alignment == 0L) {
                        "$name is not $alignment-byte aligned at padding boundary"
                    }
                    var consumed = 0
                    while (consumed < extraLength) {
                        require(consumed + 4 <= extraLength) {
                            "$name extra field has a truncated TLV record: $extraLength bytes declared"
                        }
                        readU16(input)
                        val recordLength = readU16(input)
                        require(consumed + 4 + recordLength <= extraLength) {
                            "$name extra field record overruns the declared length: $extraLength"
                        }
                        input.skipBytes(recordLength)
                        consumed += 4 + recordLength
                    }
                    require(consumed == extraLength) { "$name extra field records do not fill the declared length" }
                    return
                }
                input.seek(offset + 18)
                val compressedSize = (readU32(input))
                offset += 30L + nameLength + extraLength + compressedSize
            }
        }
        error("$name has no local file header")
    }

    private fun readU16(input: RandomAccessFile): Int = input.readUnsignedByte() or (input.readUnsignedByte() shl 8)

    private fun readU32(input: RandomAccessFile): Long =
        readU16(input).toLong() or (readU16(input).toLong() shl 16)
}
