package dev.radek.conventor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets

class PlaceholderPackagingTest {
    @Test fun binaryXmlManifestKeepsIndicesAndPatchesUnicodeLabelAndPackage() {
        val original = binaryXml(
            listOf(
                "dev.radek.placeholder",
                "__RADEK_PLACEHOLDER_LABEL__",
                "manifest",
                "package",
            ),
        )
        // More than 127 UTF-8 bytes exercises Android's two-byte length8 encoding.
        val expectedLabel = "Žluťoučký drak 🎮 ${"界".repeat(60)}"
        val patched = BinaryXmlManifest.customize(original, "dev.radek.placeholder.p0123456789abcdef01234567", expectedLabel)

        assertEquals(patched.size, u32(patched, 4).toInt())
        val pool = patched.copyOfRange(8, 8 + u32(patched, 12).toInt())
        val strings = readPool(pool)
        assertEquals("dev.radek.placeholder.p0123456789abcdef01234567", strings[0])
        assertEquals(expectedLabel, strings[1])
        assertFalse((u32(pool, 16).toInt() and 1) != 0)
    }

    @Test fun binaryXmlManifestHandlesAaptUtf16StringPools() {
        val original = binaryXml(
            listOf("dev.radek.placeholder", "__RADEK_PLACEHOLDER_LABEL__", "manifest", "package"),
            utf8 = false,
        )
        val expectedLabel = "Žluťoučký drak 🎮"
        val patched = BinaryXmlManifest.customize(original, "dev.radek.placeholder.p0123456789abcdef01234567", expectedLabel)
        val pool = patched.copyOfRange(8, 8 + u32(patched, 12).toInt())

        // The patcher may safely emit UTF-8 after reading a UTF-16 source pool.
        assertTrue((u32(pool, 16).toInt() and 0x100) != 0)
        val strings = readPool(pool)
        assertEquals("dev.radek.placeholder.p0123456789abcdef01234567", strings[0])
        assertEquals(expectedLabel, strings[1])
    }

    @Test fun binaryXmlManifestKeepsLargeStringsInUtf16WhenUtf8LengthFieldsWouldOverflow() {
        val large = "界".repeat(40_000)
        val original = binaryXml(
            listOf("dev.radek.placeholder", "__RADEK_PLACEHOLDER_LABEL__", large),
            utf8 = false,
        )
        val patched = BinaryXmlManifest.customize(original, "dev.radek.placeholder.p0123456789abcdef01234567", "Game")
        val pool = patched.copyOfRange(8, 8 + u32(patched, 12).toInt())

        assertEquals(0, u32(pool, 16).toInt() and 0x100)
        assertEquals(large, readPool(pool)[2])
    }

    @Test fun binaryXmlManifestRejectsUnsafePackageNames() {
        val original = binaryXml(listOf("dev.radek.placeholder", "__RADEK_PLACEHOLDER_LABEL__"))
        org.junit.Assert.assertThrows(IllegalArgumentException::class.java) {
            BinaryXmlManifest.customize(original, "../outside", "Game")
        }
    }

    @Test fun resourcesTablePackageNameIsRewrittenWithoutChangingItsSize() {
        val original = resourcesTable("dev.radek.placeholder")
        val patched = ResourceTablePackagePatcher.customize(original, "dev.radek.placeholder.p0123456789abcdef01234567")
        assertEquals(original.size, patched.size)
        assertEquals("dev.radek.placeholder.p0123456789abcdef01234567", readResourcePackageName(patched, 24))
        assertEquals(0x0002, u16(patched, 0))
        assertEquals(1, u32(patched, 8).toInt())
    }

    @Test fun templatePackageMustBeUniqueAndExpected() {
        org.junit.Assert.assertThrows(IllegalArgumentException::class.java) {
            ResourceTablePackagePatcher.customize(resourcesTable("dev.radek.other"), "dev.radek.placeholder.p0123456789abcdef01234567")
        }
    }

    private fun binaryXml(strings: List<String>, utf8: Boolean = true): ByteArray {
        val data = ByteArrayOutputStream()
        val offsets = ArrayList<Int>()
        strings.forEach { value ->
            offsets += data.size()
            if (utf8) {
                val bytes = value.toByteArray(StandardCharsets.UTF_8)
                writeLength8(data, value.length)
                writeLength8(data, bytes.size)
                data.write(bytes)
                data.write(0)
            } else {
                writeLength16(data, value.length)
                data.write(value.toByteArray(StandardCharsets.UTF_16LE))
                data.write(0)
                data.write(0)
            }
        }
        while (data.size() % 4 != 0) data.write(0)
        val stringStart = 28 + offsets.size * 4
        val pool = ByteArrayOutputStream()
        putU16(pool, 1)
        putU16(pool, 28)
        putU32(pool, stringStart + data.size())
        putU32(pool, strings.size)
        putU32(pool, 0)
        putU32(pool, if (utf8) 0x100 else 0)
        putU32(pool, stringStart)
        putU32(pool, 0)
        offsets.forEach { putU32(pool, it) }
        pool.write(data.toByteArray())
        val dummyNode = ByteArrayOutputStream().apply {
            putU16(this, 0x0100)
            putU16(this, 8)
            putU32(this, 8)
        }.toByteArray()
        val totalSize = 8 + pool.size() + dummyNode.size
        return ByteArrayOutputStream().apply {
            putU16(this, 0x0003)
            putU16(this, 8)
            putU32(this, totalSize)
            write(pool.toByteArray())
            write(dummyNode)
        }.toByteArray()
    }

    private fun readPool(pool: ByteArray): List<String> {
        val count = u32(pool, 8).toInt()
        val stringsStart = u32(pool, 20).toInt()
        val headerSize = u16(pool, 2)
        val utf8 = (u32(pool, 16).toInt() and 0x100) != 0
        return (0 until count).map { index ->
            var offset = stringsStart + u32(pool, headerSize + index * 4).toInt()
            if (utf8) {
                val (_, afterUtf16) = readLength8(pool, offset)
                offset = afterUtf16
                val (length, stringStart) = readLength8(pool, offset)
                String(pool, stringStart, length, StandardCharsets.UTF_8)
            } else {
                val (length, stringStart) = readLength16(pool, offset)
                String(pool, stringStart, length * 2, StandardCharsets.UTF_16LE)
            }
        }
    }

    private fun resourcesTable(packageName: String): ByteArray {
        val packageChunk = ByteArray(288)
        putU16(packageChunk, 0, 0x0200)
        putU16(packageChunk, 2, 288)
        putU32(packageChunk, 4, 288)
        putU32(packageChunk, 8, 0x7f)
        val encodedName = (packageName + "\u0000").toByteArray(StandardCharsets.UTF_16LE)
        encodedName.copyInto(packageChunk, 12)
        val root = ByteArray(12 + packageChunk.size)
        putU16(root, 0, 0x0002)
        putU16(root, 2, 12)
        putU32(root, 4, root.size)
        putU32(root, 8, 1)
        packageChunk.copyInto(root, 12)
        return root
    }

    private fun readResourcePackageName(data: ByteArray, offset: Int): String {
        val chars = CharArray(128)
        for (index in chars.indices) {
            val code = (data[offset + 2 * index].toInt() and 0xff) or ((data[offset + 2 * index + 1].toInt() and 0xff) shl 8)
            if (code == 0) return String(chars, 0, index)
            chars[index] = code.toChar()
        }
        return String(chars)
    }

    private fun writeLength8(output: ByteArrayOutputStream, value: Int) {
        if (value < 0x80) output.write(value) else {
            output.write((value ushr 8) or 0x80)
            output.write(value and 0xff)
        }
    }

    private fun writeLength16(output: ByteArrayOutputStream, value: Int) {
        if (value < 0x8000) putU16(output, value) else {
            putU16(output, (value ushr 16) or 0x8000)
            putU16(output, value and 0xffff)
        }
    }

    private fun readLength16(data: ByteArray, offset: Int): Pair<Int, Int> {
        val first = u16(data, offset)
        return if (first and 0x8000 == 0) first to offset + 2 else {
            val second = u16(data, offset + 2)
            (((first and 0x7fff) shl 16) or second) to offset + 4
        }
    }

    private fun readLength8(data: ByteArray, offset: Int): Pair<Int, Int> {
        val first = data[offset].toInt() and 0xff
        return if (first and 0x80 == 0) first to offset + 1 else
            (((first and 0x7f) shl 8) or (data[offset + 1].toInt() and 0xff)) to offset + 2
    }

    private fun putU16(output: ByteArrayOutputStream, value: Int) {
        output.write(value and 0xff)
        output.write((value ushr 8) and 0xff)
    }

    private fun putU32(output: ByteArrayOutputStream, value: Int) {
        output.write(value and 0xff)
        output.write((value ushr 8) and 0xff)
        output.write((value ushr 16) and 0xff)
        output.write((value ushr 24) and 0xff)
    }

    private fun putU16(data: ByteArray, offset: Int, value: Int) {
        data[offset] = value.toByte()
        data[offset + 1] = (value ushr 8).toByte()
    }

    private fun putU32(data: ByteArray, offset: Int, value: Int) {
        for (index in 0..3) data[offset + index] = (value ushr (8 * index)).toByte()
    }

    private fun u16(data: ByteArray, offset: Int): Int =
        (data[offset].toInt() and 0xff) or ((data[offset + 1].toInt() and 0xff) shl 8)

    private fun u32(data: ByteArray, offset: Int): Long =
        (data[offset].toLong() and 0xff) or
            ((data[offset + 1].toLong() and 0xff) shl 8) or
            ((data[offset + 2].toLong() and 0xff) shl 16) or
            ((data[offset + 3].toLong() and 0xff) shl 24)
}
