package dev.radek.conventor

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

/** Bounds-checked Android binary-XML string-pool patcher for the generated APK templates. */
internal object BinaryXmlManifest {
    private const val RES_XML_TYPE = 0x0003
    private const val RES_STRING_POOL_TYPE = 0x0001
    private const val RES_XML_START_ELEMENT_TYPE = 0x0102
    private const val RES_XML_END_ELEMENT_TYPE = 0x0103
    private const val UTF8_FLAG = 0x00000100
    private const val SORTED_FLAG = 0x00000001
    private const val TEMPLATE_PACKAGE = "dev.radek.placeholder"
    private const val TEMPLATE_LABEL = "__RADEK_PLACEHOLDER_LABEL__"
    private const val MAX_STRING_COUNT = 10_000
    private const val MAX_STRING_CODE_UNITS = 1_000_000

    private const val NO_STRING = 0xffffffffL
    /** Res_value::TYPE_INT_DEC: the attribute holds a plain decimal integer. */
    private const val TYPE_INT_DEC = 0x10
    private const val NODE_BYTES = 16
    private const val ATTR_EXT_BYTES = 20
    private const val ATTRIBUTE_BYTES = 20
    private const val END_ELEMENT_BYTES = NODE_BYTES + 8
    private const val ANDROID_NAMESPACE = "http://schemas.android.com/apk/res/android"

    /**
     * Android refuses to install an APK whose targetSdkVersion is below 23, so
     * every generated manifest is pinned to at least these levels. Both match
     * the app and both templates.
     */
    const val MIN_SDK_VERSION = 26
    const val TARGET_SDK_VERSION = 35

    fun customize(
        template: ByteArray,
        packageName: String,
        label: String,
        minSdkVersion: Int = MIN_SDK_VERSION,
        targetSdkVersion: Int = TARGET_SDK_VERSION,
    ): ByteArray {
        require(packageName.matches(Regex("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+"))) {
            "invalid Android package name"
        }
        require(label.isNotBlank() && label.length <= 160 && label.none { Character.isISOControl(it) }) {
            "invalid placeholder application name"
        }
        require(minSdkVersion in 1..10_000 && targetSdkVersion in 1..10_000 && minSdkVersion <= targetSdkVersion) {
            "invalid Android SDK levels"
        }
        require(u16(template, 0) == RES_XML_TYPE && u16(template, 2) == 8) {
            "template manifest is not Android binary XML"
        }
        require(u32(template, 4) == template.size.toLong()) { "invalid binary XML size" }

        val chunks = ArrayList<Pair<Int, ByteArray>>()
        var poolChunk: ByteArray? = null
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
                poolChunk = template.copyOfRange(offset, offset + size)
            } else {
                chunks += type to template.copyOfRange(offset, offset + size)
            }
            offset += size
        }
        require(offset == template.size && poolCount == 1) { "template must contain exactly one string pool" }
        val pool = poolChunk!!

        val (decoded, flags) = decodePoolStrings(pool)
        val replacements = mapOf(TEMPLATE_PACKAGE to packageName, TEMPLATE_LABEL to label)
        for (source in replacements.keys) {
            require(decoded.count { it == source } == 1) { "template string sentinel is missing or ambiguous: $source" }
        }
        val strings = ArrayList<String>(decoded.map { replacements[it] ?: it })

        // Android 14+ rejects an APK whose targetSdkVersion is below 23, and a
        // manifest without <uses-sdk> defaults both levels to 1. Rather than
        // shipping an APK the installer silently refuses (the generic "app not
        // installed" message), patch or insert the element here.
        val hasUsesSdk = chunks.any { (type, bytes) ->
            type == RES_XML_START_ELEMENT_TYPE && elementName(strings, bytes) == "uses-sdk"
        }
        val extraIndices = LinkedHashMap<String, Int>()
        if (!hasUsesSdk) {
            for (name in listOf("uses-sdk", "minSdkVersion", "targetSdkVersion")) {
                var index = strings.indexOf(name)
                if (index < 0) {
                    index = strings.size
                    strings += name
                }
                extraIndices[name] = index
            }
        }

        val output = ByteArrayOutputStream(template.size + label.length * 4 + 256)
        output.write(template, 0, 8)
        output.write(buildStringPool(strings, flags and SORTED_FLAG.inv()))

        // <uses-sdk> must be a child of the root element. Real aapt output
        // always has one, so the fallback only exists to stay total: a fixture
        // with no elements at all is left alone rather than rejected.
        val insertionIndex = if (hasUsesSdk) -1 else {
            val manifestIndex = chunks.indexOfFirst { (type, bytes) ->
                type == RES_XML_START_ELEMENT_TYPE && elementName(strings, bytes) == "manifest"
            }
            if (manifestIndex >= 0) manifestIndex else chunks.indexOfFirst { (type, _) -> type == RES_XML_START_ELEMENT_TYPE }
        }
        chunks.forEachIndexed { index, (type, bytes) ->
            if (type == RES_XML_START_ELEMENT_TYPE) {
                output.write(patchSdkAttributes(bytes, strings, minSdkVersion, targetSdkVersion))
                if (index == insertionIndex) {
                    val namespaceIndex = strings.indexOf(ANDROID_NAMESPACE)
                    output.write(
                        buildUsesSdkStartElement(
                            namespaceIndex,
                            extraIndices.getValue("uses-sdk"),
                            extraIndices.getValue("minSdkVersion"),
                            extraIndices.getValue("targetSdkVersion"),
                            minSdkVersion,
                            targetSdkVersion,
                        ),
                    )
                    output.write(buildEndElement(extraIndices.getValue("uses-sdk")))
                }
            } else {
                output.write(bytes)
            }
        }
        val result = output.toByteArray()
        putU32(result, 4, result.size.toLong())
        return result
    }

    // --- <uses-sdk> handling ------------------------------------------------

    private fun elementName(strings: List<String>, chunk: ByteArray): String {
        require(chunk.size >= NODE_BYTES + 8) { "truncated binary XML element" }
        val index = u32(chunk, NODE_BYTES + 4)
        if (index == NO_STRING) return ""
        require(index < strings.size.toLong()) { "binary XML element name outside the string pool" }
        return strings[index.toInt()]
    }

    private fun patchSdkAttributes(chunk: ByteArray, strings: List<String>, minSdk: Int, targetSdk: Int): ByteArray {
        if (elementName(strings, chunk) != "uses-sdk") return chunk
        require(chunk.size >= NODE_BYTES + ATTR_EXT_BYTES) { "truncated binary XML start element" }
        val attributeStart = NODE_BYTES + u16(chunk, NODE_BYTES + 8)
        val attributeSize = u16(chunk, NODE_BYTES + 10)
        val count = u16(chunk, NODE_BYTES + 12)
        require(attributeSize >= ATTRIBUTE_BYTES && attributeStart >= NODE_BYTES + ATTR_EXT_BYTES) {
            "invalid binary XML attribute layout"
        }
        require(attributeStart + count.toLong() * attributeSize <= chunk.size.toLong()) {
            "binary XML attributes outside the element chunk"
        }
        val result = chunk.copyOf()
        for (index in 0 until count) {
            val base = attributeStart + index * attributeSize
            val nameIndex = u32(result, base + 4)
            if (nameIndex == NO_STRING) continue
            require(nameIndex < strings.size.toLong()) { "binary XML attribute name outside the string pool" }
            val dataType = result[base + 15].toInt() and 0xff
            if (dataType != TYPE_INT_DEC) continue
            val current = u32(result, base + 16)
            val floor = when (strings[nameIndex.toInt()]) {
                "minSdkVersion" -> minSdk.toLong()
                "targetSdkVersion" -> targetSdk.toLong()
                else -> continue
            }
            // Only ever raise a level; a template is allowed to declare higher.
            if (current < floor) putU32(result, base + 16, floor)
        }
        return result
    }

    private fun buildUsesSdkStartElement(
        namespaceIndex: Int,
        nameIndex: Int,
        minSdkNameIndex: Int,
        targetSdkNameIndex: Int,
        minSdk: Int,
        targetSdk: Int,
    ): ByteArray {
        val size = NODE_BYTES + ATTR_EXT_BYTES + 2 * ATTRIBUTE_BYTES
        val chunk = ByteArray(size)
        putU16(chunk, 0, RES_XML_START_ELEMENT_TYPE)
        putU16(chunk, 2, NODE_BYTES + ATTR_EXT_BYTES)
        putU32(chunk, 4, size.toLong())
        putU32(chunk, 8, 0) // lineNumber
        putU32(chunk, 12, NO_STRING) // comment
        putU32(chunk, 16, if (namespaceIndex >= 0) namespaceIndex.toLong() else NO_STRING)
        putU32(chunk, 20, nameIndex.toLong())
        putU16(chunk, 24, ATTR_EXT_BYTES) // attributeStart, relative to the attrExt struct
        putU16(chunk, 26, ATTRIBUTE_BYTES)
        putU16(chunk, 28, 2) // attributeCount
        putU16(chunk, 30, 0) // idIndex
        putU16(chunk, 32, 0) // classIndex
        putU16(chunk, 34, 0) // styleIndex
        val namespace = if (namespaceIndex >= 0) namespaceIndex.toLong() else NO_STRING
        writeIntegerAttribute(chunk, NODE_BYTES + ATTR_EXT_BYTES, namespace, minSdkNameIndex, minSdk)
        writeIntegerAttribute(chunk, NODE_BYTES + ATTR_EXT_BYTES + ATTRIBUTE_BYTES, namespace, targetSdkNameIndex, targetSdk)
        return chunk
    }

    private fun writeIntegerAttribute(chunk: ByteArray, base: Int, namespace: Long, nameIndex: Int, value: Int) {
        putU32(chunk, base, namespace)
        putU32(chunk, base + 4, nameIndex.toLong())
        putU32(chunk, base + 8, NO_STRING) // rawValue
        putU16(chunk, base + 12, 8) // typedValue size
        chunk[base + 14] = 0 // res0
        chunk[base + 15] = TYPE_INT_DEC.toByte()
        putU32(chunk, base + 16, value.toLong())
    }

    private fun buildEndElement(nameIndex: Int): ByteArray {
        val chunk = ByteArray(END_ELEMENT_BYTES)
        putU16(chunk, 0, RES_XML_END_ELEMENT_TYPE)
        putU16(chunk, 2, END_ELEMENT_BYTES)
        putU32(chunk, 4, END_ELEMENT_BYTES.toLong())
        putU32(chunk, 8, 0) // lineNumber
        putU32(chunk, 12, NO_STRING) // comment
        putU32(chunk, 16, NO_STRING) // ns
        putU32(chunk, 20, nameIndex.toLong())
        return chunk
    }

    // --- string pool --------------------------------------------------------

    private fun decodePoolStrings(chunk: ByteArray): Pair<List<String>, Int> {
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
        return strings to flags
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

    private fun putU16(data: ByteArray, offset: Int, value: Int) {
        require(offset >= 0 && offset + 2 <= data.size) { "binary XML output field outside buffer" }
        data[offset] = (value and 0xff).toByte()
        data[offset + 1] = ((value ushr 8) and 0xff).toByte()
    }

    private fun putU32(data: ByteArray, offset: Int, value: Long) {
        require(offset >= 0 && offset + 4 <= data.size) { "binary XML output field outside buffer" }
        data[offset] = (value and 0xff).toByte()
        data[offset + 1] = ((value ushr 8) and 0xff).toByte()
        data[offset + 2] = ((value ushr 16) and 0xff).toByte()
        data[offset + 3] = ((value ushr 24) and 0xff).toByte()
    }
}
