package dev.radek.conventor

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.CRC32
import java.util.zip.DataFormatException
import java.util.zip.Inflater

/**
 * Bounded icon decoder for ordinary Android-supported formats plus Apple's
 * non-standard CgBI PNG variant. CgBI images use raw DEFLATE and premultiplied
 * BGRA samples, which BitmapFactory does not consistently accept across Android
 * versions. Unsupported variants are rejected so the caller can try another
 * real bundle image rather than display a transparent placeholder.
 */
internal object IconDecoder {
    private const val MAX_FILE_BYTES = 16 * 1024 * 1024
    private const val MAX_DIMENSION = 8192
    private const val MAX_CGBI_INFLATED_BYTES = 16 * 1024 * 1024
    private val signature = byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
    private val cgbiType = byteArrayOf(0x43, 0x67, 0x42, 0x49)

    fun isCgbi(file: File): Boolean {
        if (!file.isFile || file.length() !in 1..MAX_FILE_BYTES.toLong()) return false
        return file.inputStream().use { input ->
            val head = ByteArray(512)
            val count = input.read(head)
            count > 0 && indexOf(head, count, cgbiType) >= 0
        }
    }

    fun decode(file: File, targetSize: Int): Bitmap? {
        if (!file.isFile || file.length() !in 1..MAX_FILE_BYTES.toLong() || targetSize < 1) return null
        if (isCgbi(file)) {
            return try {
                decodeCgbi(file.readBytes(), targetSize)
            } catch (_: Exception) {
                // An unsupported/corrupt CgBI candidate is handled by the caller's fallback chain.
                null
            }
        }
        val head = file.inputStream().use { input -> ByteArray(8).also { input.read(it) } }
        val isPng = head.contentEquals(signature)
        val isJpeg = head.size >= 3 && head[0] == 0xff.toByte() && head[1] == 0xd8.toByte() && head[2] == 0xff.toByte()
        if (!isPng && !isJpeg) return null
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.path, bounds)
        if (bounds.outWidth !in 1..MAX_DIMENSION || bounds.outHeight !in 1..MAX_DIMENSION) return null
        val sample = maxOf(1, maxOf(bounds.outWidth, bounds.outHeight) / targetSize)
        return BitmapFactory.decodeFile(file.path, BitmapFactory.Options().apply {
            inSampleSize = sample
            inPreferredConfig = Bitmap.Config.ARGB_8888
        })
    }

    /** Decode supported 8-bit, non-interlaced CgBI RGB/RGBA PNG into sampled pixels. */
    internal fun decodeCgbi(data: ByteArray, targetSize: Int): Bitmap {
        require(targetSize > 0 && data.size in 33..MAX_FILE_BYTES) { "invalid CgBI image size" }
        require(data.copyOfRange(0, signature.size).contentEquals(signature)) { "invalid PNG signature" }

        var p = signature.size
        var width = 0
        var height = 0
        var depth = 0
        var colorType = -1
        var appleChunk = false
        var sawHeader = false
        var sawEnd = false
        val compressed = ByteArrayOutputStream()
        while (p < data.size) {
            require(data.size - p >= 12) { "truncated PNG chunk" }
            val chunkLengthLong = readU32(data, p)
            require(chunkLengthLong <= data.size - p - 12L) { "PNG chunk outside image" }
            val chunkLength = chunkLengthLong.toInt()
            val typeOffset = p + 4
            val payloadOffset = p + 8
            val crcOffset = payloadOffset + chunkLength
            val type = String(data, typeOffset, 4, Charsets.US_ASCII)
            if (type != "CgBI") {
                val crc = CRC32().apply { update(data, typeOffset, 4 + chunkLength) }.value
                require(crc == readU32(data, crcOffset)) { "invalid PNG CRC" }
            }
            when (type) {
                "CgBI" -> appleChunk = true
                "IHDR" -> {
                    require(!sawHeader && chunkLength == 13) { "invalid PNG header" }
                    width = readU32(data, payloadOffset).toInt()
                    height = readU32(data, payloadOffset + 4).toInt()
                    depth = data[payloadOffset + 8].toInt() and 0xff
                    colorType = data[payloadOffset + 9].toInt() and 0xff
                    require(data[payloadOffset + 10].toInt() == 0 && data[payloadOffset + 11].toInt() == 0) {
                        "unsupported PNG compression/filter"
                    }
                    require(data[payloadOffset + 12].toInt() == 0) { "interlaced CgBI image unsupported" }
                    require(width in 1..MAX_DIMENSION && height in 1..MAX_DIMENSION) { "PNG dimensions out of range" }
                    require(depth == 8 && colorType in setOf(2, 6)) { "unsupported CgBI pixel format" }
                    sawHeader = true
                }
                "IDAT" -> {
                    require(sawHeader && !sawEnd) { "invalid PNG data order" }
                    require(compressed.size().toLong() + chunkLength <= MAX_FILE_BYTES) { "PNG data exceeds limit" }
                    compressed.write(data, payloadOffset, chunkLength)
                }
                "IEND" -> {
                    require(chunkLength == 0 && sawHeader) { "invalid PNG end" }
                    sawEnd = true
                    p = crcOffset + 4
                    break
                }
                else -> {
                    // Unknown critical chunks may alter how pixel data must be read.
                    if (type[0].isUpperCase() && type !in setOf("PLTE")) {
                        throw IllegalArgumentException("unsupported critical PNG chunk: $type")
                    }
                }
            }
            p = crcOffset + 4
        }
        require(appleChunk && sawHeader && sawEnd && p == data.size && compressed.size() > 0) {
            "incomplete CgBI PNG"
        }

        val channels = if (colorType == 6) 4 else 3
        val strideLong = width.toLong() * channels
        val expectedLong = height.toLong() * (strideLong + 1)
        require(strideLong <= Int.MAX_VALUE && expectedLong <= MAX_CGBI_INFLATED_BYTES) {
            "CgBI image exceeds decoded pixel limit"
        }
        val rows = inflate(compressed.toByteArray(), expectedLong.toInt())
        val sample = maxOf(1, maxOf(width, height) / targetSize)
        val outputWidth = (width + sample - 1) / sample
        val outputHeight = (height + sample - 1) / sample
        val colors = IntArray(outputWidth * outputHeight)
        val previous = ByteArray(strideLong.toInt())
        val row = ByteArray(strideLong.toInt())
        var sourceOffset = 0
        var outputY = 0
        for (y in 0 until height) {
            val filter = rows[sourceOffset++].toInt() and 0xff
            for (x in row.indices) {
                val raw = rows[sourceOffset++].toInt() and 0xff
                val left = if (x >= channels) row[x - channels].toInt() and 0xff else 0
                val above = previous[x].toInt() and 0xff
                val upperLeft = if (x >= channels) previous[x - channels].toInt() and 0xff else 0
                val predictor = when (filter) {
                    0 -> 0
                    1 -> left
                    2 -> above
                    3 -> (left + above) / 2
                    4 -> paeth(left, above, upperLeft)
                    else -> throw IllegalArgumentException("invalid PNG filter")
                }
                row[x] = ((raw + predictor) and 0xff).toByte()
            }
            if (y % sample == 0) {
                var outputX = 0
                for (x in 0 until width step sample) {
                    // pngcrush's CgBI output stores B,G,R,(A) and premultiplies RGB.
                    val blue = row[x * channels].toInt() and 0xff
                    val green = row[x * channels + 1].toInt() and 0xff
                    val red = row[x * channels + 2].toInt() and 0xff
                    val alpha = if (channels == 4) row[x * channels + 3].toInt() and 0xff else 255
                    val straightRed = unpremultiply(red, alpha)
                    val straightGreen = unpremultiply(green, alpha)
                    val straightBlue = unpremultiply(blue, alpha)
                    colors[outputY * outputWidth + outputX] = Color.argb(alpha, straightRed, straightGreen, straightBlue)
                    outputX++
                }
                outputY++
            }
            row.copyInto(previous)
        }
        return Bitmap.createBitmap(colors, outputWidth, outputHeight, Bitmap.Config.ARGB_8888)
    }

    private fun unpremultiply(channel: Int, alpha: Int): Int = when {
        alpha == 0 -> 0
        alpha == 255 -> channel
        else -> minOf(255, (channel * 255 + alpha / 2) / alpha)
    }

    private fun paeth(a: Int, b: Int, c: Int): Int {
        val p = a + b - c
        val pa = kotlin.math.abs(p - a)
        val pb = kotlin.math.abs(p - b)
        val pc = kotlin.math.abs(p - c)
        return when {
            pa <= pb && pa <= pc -> a
            pb <= pc -> b
            else -> c
        }
    }

    @Throws(DataFormatException::class)
    private fun inflate(payload: ByteArray, expected: Int): ByteArray {
        for (raw in listOf(true, false)) {
            val inflater = Inflater(raw)
            try {
                inflater.setInput(payload)
                val output = ByteArray(expected + 1)
                var written = 0
                while (!inflater.finished() && written < output.size) {
                    val count = inflater.inflate(output, written, output.size - written)
                    if (count == 0) {
                        if (inflater.needsDictionary() || inflater.needsInput()) break
                        throw DataFormatException("PNG inflater made no progress")
                    }
                    written += count
                }
                if (written == expected && inflater.finished() && inflater.remaining == 0) return output.copyOf(expected)
            } catch (_: DataFormatException) {
                // CgBI producers vary: accept raw DEFLATE and zlib-wrapped streams.
            } finally {
                inflater.end()
            }
        }
        throw DataFormatException("CgBI pixel data cannot be decompressed")
    }

    private fun readU32(data: ByteArray, offset: Int): Long {
        require(offset >= 0 && offset + 4 <= data.size) { "PNG field outside image" }
        return ((data[offset].toLong() and 0xff) shl 24) or
            ((data[offset + 1].toLong() and 0xff) shl 16) or
            ((data[offset + 2].toLong() and 0xff) shl 8) or
            (data[offset + 3].toLong() and 0xff)
    }

    private fun indexOf(data: ByteArray, length: Int, needle: ByteArray): Int {
        if (needle.size > length) return -1
        for (i in 0..length - needle.size) {
            if ((0 until needle.size).all { data[i + it] == needle[it] }) return i
        }
        return -1
    }
}
