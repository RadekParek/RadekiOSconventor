package dev.radek.conventor

import java.nio.charset.StandardCharsets

/** Rewrites the fixed-width package name in the template resources.arsc table. */
internal object ResourceTablePackagePatcher {
    private const val RES_TABLE_TYPE = 0x0002
    private const val RES_TABLE_PACKAGE_TYPE = 0x0200
    private const val TEMPLATE_PACKAGE = "dev.radek.placeholder"

    fun customize(template: ByteArray, packageName: String): ByteArray {
        require(packageName.matches(Regex("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+"))) {
            "invalid Android package name"
        }
        require(packageName.length <= 127) { "Android package name is too long for resources.arsc" }
        require(u16(template, 0) == RES_TABLE_TYPE && u16(template, 2) >= 12) {
            "template resources.arsc has an invalid root chunk"
        }
        require(u32(template, 4) == template.size.toLong()) { "invalid resources.arsc size" }
        val packageCount = u32(template, 8).toInt()
        val matches = ArrayList<Int>()
        var offset = u16(template, 2)
        while (offset < template.size) {
            require(offset + 8 <= template.size) { "truncated resources.arsc chunk" }
            val type = u16(template, offset)
            val headerSize = u16(template, offset + 2)
            val size = u32(template, offset + 4).toInt()
            require(headerSize >= 8 && size >= headerSize && offset + size <= template.size) {
                "invalid resources.arsc chunk size"
            }
            if (type == RES_TABLE_PACKAGE_TYPE) {
                require(headerSize >= 268) { "resource package header is too small" }
                val name = readPackageName(template, offset + 12)
                if (name == TEMPLATE_PACKAGE) matches += offset + 12
            }
            offset += size
        }
        require(offset == template.size && packageCount == 1 && matches.size == 1) {
            "expected exactly one placeholder package in resources.arsc"
        }
        val result = template.copyOf()
        val encoded = (packageName + "\u0000").toByteArray(StandardCharsets.UTF_16LE)
        require(encoded.size <= 256) { "Android package name is too long for resources.arsc" }
        result.fill(0, matches.single(), matches.single() + 256)
        encoded.copyInto(result, matches.single())
        return result
    }

    private fun readPackageName(data: ByteArray, offset: Int): String {
        require(offset >= 0 && offset + 256 <= data.size) { "truncated resource package name" }
        val chars = CharArray(128)
        for (index in chars.indices) {
            val low = data[offset + index * 2].toInt() and 0xff
            val high = data[offset + index * 2 + 1].toInt() and 0xff
            val char = (low or (high shl 8)).toChar()
            if (char == '\u0000') return String(chars, 0, index)
            chars[index] = char
        }
        return String(chars)
    }

    private fun u16(data: ByteArray, offset: Int): Int {
        require(offset >= 0 && offset + 2 <= data.size) { "truncated resources.arsc field" }
        return (data[offset].toInt() and 0xff) or ((data[offset + 1].toInt() and 0xff) shl 8)
    }

    private fun u32(data: ByteArray, offset: Int): Long {
        require(offset >= 0 && offset + 4 <= data.size) { "truncated resources.arsc field" }
        return (data[offset].toLong() and 0xff) or
            ((data[offset + 1].toLong() and 0xff) shl 8) or
            ((data[offset + 2].toLong() and 0xff) shl 16) or
            ((data[offset + 3].toLong() and 0xff) shl 24)
    }
}
