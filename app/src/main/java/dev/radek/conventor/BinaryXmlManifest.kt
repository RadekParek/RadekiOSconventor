package dev.radek.conventor

import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets

/** Narrow, bounds-checked Android binary-XML string-pool patcher for the APK template. */
internal object BinaryXmlManifest {
    private const val RES_XML_TYPE = 0x0003
    private const val RES_STRING_POOL_TYPE = 0x0001
    private const val UTF8_FLAG = 0x00000100
    private const val SORTED_FLAG = 0x00000001
    private const val TEMPLATE_PACKAGE = "dev.radek.placeholder"
    private const val TEMPLATE_LABEL = "__RADEK_PLACEHOLDER_LABEL__"

    fun customize(template: ByteArray, packageName: String, label: String): ByteArray {
        require(packageName.matches(Regex("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+"))) {
            "invalid Android package name"
        }
        require(label.isNotBlank() && label.length <= 160 && label.none { Character.isISOControl(it) }) {
            "invalid placeholder application name"
        }
        require(u16(template, 0) == RES_XML_TYPE && u16(template, 2) == 8) {
            "template manifest is not Android binary XML"
        }
        require(u32(template, 4) == template.size.toLong()) { "invalid binary XML size" }

        val output = ByteArrayOutputStream(template.size + label.length * 4)
        output.write(template, 0, 8)
        var offset = 8
        var poolCount = 0
        while (offset < template.size) {
            require(offset + 8 <= template.size) { "truncated binary XML chunk" }
            val type = u16(template, offset)
            val size = u32(template, offset + 4).toInt()
            require(size >= 8 && offset + size <= template.size) { "invalid binary XML chunk size" }
            if (type == RES_STRING_POOL_TYPE) {
                poolCount++
                val chunk = template.copyOfRange(offset, offset + size)
                output.write(
                    replacePoolStrings(
                        chunk,
                        mapOf(TEMPLATE_PACKAGE to packageName, TEMPLATE_LABEL to label),
                    ),
                )
            } else {
                output.write(template, offset, size)
            }
            offset += size
        }
        require(offset == template.size && poolCount == 1) { "template must contain exactly one string pool" }
        val result = output.toByteArray()
        putU32(result, 4, result.size.toLong())
        return result
    }

    private fun replacePoolStrings(chunk: ByteArray, replacements: Map<String, String>): ByteArray {
        require(u16(chunk, 0) == RES_STRING_POOL_TYPE) { "invalid string-pool chunk" }
        val headerSize = u16(chunk, 2)
        val chunkSize = u32(chunk, 4).toInt()
        require(headerSize == 28 && chunkSize == chunk.size) { "unsupported Android string-pool header" }
        val count = u32(chunk, 8).toInt()
        val styleCount = u32(chunk, 12).toInt()
        val flags = u32(chunk, 16).toInt()
        val stringsStart = u32(chunk, 20).toInt()
        val stylesStart = u32(chunk, 24).toInt()
        require(count in 1..10000 && styleCount == 0 && stylesStart == 0) {
            "unsupported styled or oversized template string pool"
        }
        require(stringsStart >= headerSize + count * 4 && stringsStart <= chunk.size) {
            "invalid string-pool string offset"
        }
        val strings = ArrayList<String>(count)
        for (index in 0 until count) {
            val relative = u32(chunk, headerSize + index * 4).toInt()
            require(stringsStart + relative in stringsStart until chunk.size) { "string-pool entry outside chunk" }
            strings += readUtf8String(chunk, stringsStart + relative)
        }
        for (source in replacements.keys) {
            require(strings.count { it == source } == 1) { "template string sentinel is missing or ambiguous: $source" }
        }
        val changed = strings.map { replacements[it] ?: it }
        return buildStringPool(changed, flags and SORTED_FLAG.inv())
    }

    private fun buildStringPool(strings: List<String>, flags: Int): ByteArray {
        val data = ByteArrayOutputStream()
        val offsets = IntArray(strings.size)
        strings.forEachIndexed { index, value ->
            val utf8 = value.toByteArray(StandardCharsets.UTF_8)
            offsets[index] = data.size()
            writeLength8(data, value.length) // UTF-16 code units
            writeLength8(data, utf8.size)
            data.write(utf8)
            data.write(0)
        }
        while (data.size() % 4 != 0) data.write(0)
        val stringsStart = 28 + strings.size * 4
        val chunkSize = stringsStart + data.size()
        val output = ByteArrayOutputStream(chunkSize)
        putU16(output, RES_STRING_POOL_TYPE)
        putU16(output, 28)
        putU32(output, chunkSize.toLong())
        putU32(output, strings.size.toLong())
        putU32(output, 0)
        putU32(output, (flags or UTF8_FLAG).toLong())
        putU32(output, stringsStart.toLong())
        putU32(output, 0)
        offsets.forEach { putU32(output, it.toLong()) }
        output.write(data.toByteArray())
        return output.toByteArray()
    }

    private fun readUtf8String(data: ByteArray, start: Int): String {
        val (utf16Length, afterUtf16Length) = readLength8(data, start)
        val (byteCount, stringStart) = readLength8(data, afterUtf16Length)
        require(utf16Length <= 32767 && byteCount in 0..65536 && stringStart + byteCount < data.size) {
            "invalid UTF-8 string-pool length"
        }
        require(data[stringStart + byteCount].toInt() == 0) { "unterminated UTF-8 string-pool entry" }
        val value = String(data, stringStart, byteCount, StandardCharsets.UTF_8)
        require(value.length == utf16Length) { "inconsistent UTF-8 string-pool length" }
        return value
    }

    private fun readLength8(data: ByteArray, offset: Int): Pair<Int, Int> {
        require(offset < data.size) { "truncated UTF-8 string-pool length" }
        val first = data[offset].toInt() and 0xff
        return if (first and 0x80 == 0) {
            first to offset + 1
        } else {
            require(offset + 1 < data.size) { "truncated two-byte string-pool length" }
            (((first and 0x7f) shl 8) or (data[offset + 1].toInt() and 0xff)) to offset + 2
        }
    }

    private fun writeLength8(output: ByteArrayOutputStream, value: Int) {
        require(value in 0..0x7fff) { "string is too long for an Android string pool" }
        if (value < 0x80) {
            output.write(value)
        } else {
            output.write((value ushr 8) or 0x80)
            output.write(value and 0xff)
        }
    }

    private fun u16(data: ByteArray, offset: Int): Int {
        require(offset >= 0 && offset + 2 <= data.size) { "truncated binary XML field" }
        return (data[offset].toInt() and 0xff) or ((data[offset + 1].toInt() and 0xff) shl 8)
    }

    private fun u32(data: ByteArray, offset: Int): Long {
        require(offset >= 0 && offset + 4 <= data.size) { "truncated binary XML field" }
        return (data[offset].toLong() and 0xff) or
            ((data[offset + 1].toLong() and 0xff) shl 8) or
            ((data[offset + 2].toLong() and 0xff) shl 16) or
            ((data[offset + 3].toLong() and 0xff) shl 24)
    }

    private fun putU16(output: ByteArrayOutputStream, value: Int) {
        output.write(value and 0xff)
        output.write((value ushr 8) and 0xff)
    }

    private fun putU32(output: ByteArrayOutputStream, value: Long) {
        output.write((value and 0xff).toInt())
        output.write(((value ushr 8) and 0xff).toInt())
        output.write(((value ushr 16) and 0xff).toInt())
        output.write(((value ushr 24) and 0xff).toInt())
    }

    private fun putU32(data: ByteArray, offset: Int, value: Long) {
        require(offset >= 0 && offset + 4 <= data.size) { "binary XML output field outside buffer" }
        data[offset] = (value and 0xff).toByte()
        data[offset + 1] = ((value ushr 8) and 0xff).toByte()
        data[offset + 2] = ((value ushr 16) and 0xff).toByte()
        data[offset + 3] = ((value ushr 24) and 0xff).toByte()
    }
}
