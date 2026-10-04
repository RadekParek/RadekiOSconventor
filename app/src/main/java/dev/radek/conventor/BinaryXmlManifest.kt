package dev.radek.conventor

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

/** Bounds-checked Android binary-XML string-pool patcher for the placeholder APK template. */
internal object BinaryXmlManifest {
    private const val RES_XML_TYPE = 0x0003
    private const val RES_STRING_POOL_TYPE = 0x0001
    private const val UTF8_FLAG = 0x00000100
    private const val SORTED_FLAG = 0x00000001
    private const val TEMPLATE_PACKAGE = "dev.radek.placeholder"
    private const val TEMPLATE_LABEL = "__RADEK_PLACEHOLDER_LABEL__"
    private const val MAX_STRING_COUNT = 10_000
    private const val MAX_STRING_CODE_UNITS = 1_000_000

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
            val sizeLong = u32(template, offset + 4)
            require(sizeLong >= 8 && sizeLong <= (template.size - offset).toLong()) {
                "invalid binary XML chunk size"
            }
            val size = sizeLong.toInt()
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
        require(headerSize == 28 && u32(chunk, 4) == chunk.size.toLong()) {
            "unsupported Android string-pool header"
        }
        val countLong = u32(chunk, 8)
        val styleCount = u32(chunk, 12)
        val flags = u32(chunk, 16).toInt()
        val stringsStartLong = u32(chunk, 20)
        val stylesStart = u32(chunk, 24)
        require(countLong in 1L..MAX_STRING_COUNT.toLong() && styleCount == 0L && stylesStart == 0L) {
            "unsupported styled or oversized template string pool"
        }
        val count = countLong.toInt()
        val indexTableEnd = headerSize.toLong() + countLong * 4L
        require(stringsStartLong >= indexTableEnd && stringsStartLong <= chunk.size.toLong()) {
            "invalid string-pool string offset"
        }

        val utf8 = flags and UTF8_FLAG != 0
        val strings = ArrayList<String>(count)
        for (index in 0 until count) {
            val relative = u32(chunk, headerSize + index * 4)
            val absolute = stringsStartLong + relative
            require(relative < chunk.size.toLong() && absolute in stringsStartLong until chunk.size.toLong()) {
                "string-pool entry outside chunk"
            }
            val start = absolute.toInt()
            strings += if (utf8) readUtf8String(chunk, start) else readUtf16String(chunk, start)
        }
        for (source in replacements.keys) {
            require(strings.count { it == source } == 1) { "template string sentinel is missing or ambiguous: $source" }
        }
        val changed = strings.map { replacements[it] ?: it }
        return buildStringPool(changed, flags and SORTED_FLAG.inv())
    }

    /** Android UTF-8 pools have two length8 fields followed by UTF-8 and a one-byte NUL. */
    private fun readUtf8String(data: ByteArray, start: Int): String {
        val (utf16Length, afterUtf16Length) = readLength8(data, start)
        val (byteCount, stringStart) = readLength8(data, afterUtf16Length)
        require(utf16Length <= MAX_STRING_CODE_UNITS && byteCount <= MAX_STRING_CODE_UNITS * 4 &&
            stringStart >= 0 && stringStart <= data.size && byteCount <= data.size - stringStart - 1) {
            "invalid UTF-8 string-pool length"
        }
        require(data[stringStart + byteCount].toInt() == 0) { "unterminated UTF-8 string-pool entry" }
        val value = decode(data, stringStart, byteCount, StandardCharsets.UTF_8)
        require(value.length == utf16Length) { "inconsistent UTF-8 string-pool length" }
        return value
    }

    /** Android UTF-16 pools use length16 fields and a two-byte NUL terminator. */
    private fun readUtf16String(data: ByteArray, start: Int): String {
        val (utf16Length, stringStart) = readLength16(data, start)
        require(utf16Length <= MAX_STRING_CODE_UNITS) { "UTF-16 string-pool entry is too long" }
        val byteCount = utf16Length.toLong() * 2L
        require(stringStart >= 0 && stringStart <= data.size &&
            byteCount <= (data.size - stringStart - 2).toLong()) {
            "invalid UTF-16 string-pool length"
        }
        val terminator = stringStart + byteCount.toInt()
        require(data[terminator].toInt() == 0 && data[terminator + 1].toInt() == 0) {
            "unterminated UTF-16 string-pool entry"
        }
        val value = decode(data, stringStart, byteCount.toInt(), StandardCharsets.UTF_16LE)
        require(value.length == utf16Length) { "inconsistent UTF-16 string-pool length" }
        return value
    }

    private fun decode(data: ByteArray, offset: Int, length: Int, charset: java.nio.charset.Charset): String =
        charset.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(data, offset, length))
            .toString()

    private fun readLength8(data: ByteArray, offset: Int): Pair<Int, Int> {
        require(offset >= 0 && offset < data.size) { "truncated UTF-8 string-pool length" }
        val first = data[offset].toInt() and 0xff
        return if (first and 0x80 == 0) {
            first to offset + 1
        } else {
            require(offset + 1 < data.size) { "truncated two-byte UTF-8 string-pool length" }
            (((first and 0x7f) shl 8) or (data[offset + 1].toInt() and 0xff)) to offset + 2
        }
    }

    private fun readLength16(data: ByteArray, offset: Int): Pair<Int, Int> {
        val first = u16(data, offset)
        return if (first and 0x8000 == 0) {
            first to offset + 2
        } else {
            val second = u16(data, offset + 2)
            (((first and 0x7fff) shl 16) or second) to offset + 4
        }
    }

    private fun buildStringPool(strings: List<String>, originalFlags: Int): ByteArray {
        val encodedUtf8 = strings.map { it.toByteArray(StandardCharsets.UTF_8) }
        val useUtf8 = strings.indices.all { index ->
            strings[index].length <= 0x7fff && encodedUtf8[index].size <= 0x7fff
        }
        val data = ByteArrayOutputStream()
        val offsets = IntArray(strings.size)
        strings.forEachIndexed { index, value ->
            offsets[index] = data.size()
            if (useUtf8) {
                val utf8 = encodedUtf8[index]
                writeLength8(data, value.length) // UTF-16 code units
                writeLength8(data, utf8.size)
                data.write(utf8)
                data.write(0)
            } else {
                writeLength16(data, value.length)
                data.write(value.toByteArray(StandardCharsets.UTF_16LE))
                data.write(0)
                data.write(0)
            }
        }
        while (data.size() % 4 != 0) data.write(0)
        val stringsStart = 28 + strings.size * 4
        val chunkSize = stringsStart + data.size()
        val output = ByteArrayOutputStream(chunkSize)
        val outputFlags = (originalFlags and SORTED_FLAG.inv() and UTF8_FLAG.inv()) or (if (useUtf8) UTF8_FLAG else 0)
        putU16(output, RES_STRING_POOL_TYPE)
        putU16(output, 28)
        putU32(output, chunkSize.toLong())
        putU32(output, strings.size.toLong())
        putU32(output, 0)
        putU32(output, outputFlags.toLong())
        putU32(output, stringsStart.toLong())
        putU32(output, 0)
        offsets.forEach { putU32(output, it.toLong()) }
        output.write(data.toByteArray())
        return output.toByteArray()
    }

    private fun writeLength8(output: ByteArrayOutputStream, value: Int) {
        require(value in 0..0x7fff) { "string is too long for an Android UTF-8 string pool" }
        if (value < 0x80) {
            output.write(value)
        } else {
            output.write((value ushr 8) or 0x80)
            output.write(value and 0xff)
        }
    }

    private fun writeLength16(output: ByteArrayOutputStream, value: Int) {
        require(value >= 0) { "negative length in Android string pool" }
        if (value < 0x8000) {
            putU16(output, value)
        } else {
            putU16(output, (value ushr 16) or 0x8000)
            putU16(output, value and 0xffff)
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
