package dev.radek.conventor

import java.io.BufferedOutputStream
import java.io.File
import java.io.FilterOutputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.nio.charset.StandardCharsets
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/** ZIP writer/checks used by the runtime-built APK; stored resources are 4-byte
 *  aligned and native libraries are page-aligned (16 pages) so the linker can
 *  mmap them straight out of the APK on 4 KB and 16 KB page devices. */
internal object AlignedApkZip {
    private const val LOCAL_FILE_HEADER = 0x04034b50L
    private const val DATA_DESCRIPTOR = 0x08074b50L
    private const val LOCAL_HEADER_BYTES = 30L
    const val ALIGNMENT = 4
    const val NATIVE_LIBRARY_ALIGNMENT = 16 * 4096
    private const val APK_ALIGNMENT_EXTRA_ID = 0xd935

    data class Entry(val name: String, val bytes: ByteArray, val compressed: Boolean = false, val alignment: Int = ALIGNMENT)

    fun write(file: File, entries: List<Entry>) {
        require(entries.isNotEmpty()) { "APK archive has no entries" }
        file.parentFile?.let { require(it.isDirectory || it.mkdirs()) { "cannot create APK output directory" } }
        BufferedOutputStream(file.outputStream()).use { write(it, entries) }
    }

    fun write(output: OutputStream, entries: List<Entry>) {
        require(entries.isNotEmpty()) { "APK archive has no entries" }
        val names = HashSet<String>()
        entries.forEach { entry ->
            require(isSafeEntryName(entry.name)) { "invalid APK entry path: ${entry.name}" }
            require(names.add(entry.name)) { "duplicate APK entry: ${entry.name}" }
        }

        val counted = CountingOutputStream(output)
        ZipOutputStream(counted).use { zip ->
            zip.setLevel(java.util.zip.Deflater.BEST_COMPRESSION)
            for (item in entries) {
                val nameBytes = item.name.toByteArray(StandardCharsets.UTF_8)
                val entry = ZipEntry(item.name).apply {
                    if (item.compressed) {
                        method = ZipEntry.DEFLATED
                    } else {
                        method = ZipEntry.STORED
                        size = item.bytes.size.toLong()
                        compressedSize = item.bytes.size.toLong()
                        crc = crc32(item.bytes)
                        val extra = alignmentExtra(counted.bytesWritten + LOCAL_HEADER_BYTES + nameBytes.size, item.alignment)
                        if (extra.isNotEmpty()) setExtra(extra)
                    }
                }
                zip.putNextEntry(entry)
                zip.write(item.bytes)
                zip.closeEntry()
            }
        }
    }

    /** Ensures the built archive contains exactly the expected files and aligned
     *  stored APK payloads; the map values are each entry's required alignment. */
    fun verify(file: File, expectedNames: Set<String>, alignedStoredNames: Map<String, Int>) {
        require(alignedStoredNames.keys.all { it in expectedNames }) { "alignment check names are not in the expected APK set" }
        ZipFile(file).use { zip ->
            val entries = zip.entries().asSequence().toList()
            val names = entries.map { it.name }
            require(names.size == names.toSet().size) { "APK contains duplicate ZIP entries" }
            require(names.toSet() == expectedNames) {
                "APK entry set mismatch (expected ${expectedNames.sorted()}, found ${names.sorted()})"
            }
            alignedStoredNames.keys.forEach { name ->
                val entry = zip.getEntry(name) ?: error("APK is missing $name")
                require(entry.method == ZipEntry.STORED) { "$name must be stored uncompressed in the APK" }
                require(entry.size > 0 && entry.compressedSize == entry.size) { "$name has an invalid stored size" }
            }
        }
        verifyLocalAlignment(file, alignedStoredNames)
    }

    private fun verifyLocalAlignment(file: File, requiredNames: Map<String, Int>) {
        if (requiredNames.isEmpty()) return
        val found = HashSet<String>()
        ZipFile(file).use { zip ->
            RandomAccessFile(file, "r").use { input ->
                var offset = 0L
                while (offset + 4 <= input.length()) {
                    input.seek(offset)
                    if (readLe32(input) != LOCAL_FILE_HEADER) break
                    require(offset + LOCAL_HEADER_BYTES <= input.length()) { "truncated APK local file header" }
                    input.seek(offset + 6)
                    val flags = readLe16(input)
                    val method = readLe16(input)
                    input.seek(offset + 26)
                    val nameLength = readLe16(input)
                    val extraLength = readLe16(input)
                    val headerEnd = offset + LOCAL_HEADER_BYTES + nameLength + extraLength
                    require(headerEnd <= input.length()) { "truncated APK local file name or extra data" }
                    input.seek(offset + LOCAL_HEADER_BYTES)
                    val nameBytes = ByteArray(nameLength)
                    input.readFully(nameBytes)
                    val name = String(nameBytes, StandardCharsets.UTF_8)
                    val entry = zip.getEntry(name) ?: error("APK local entry is absent from its central directory: $name")
                    val dataOffset = headerEnd
                    val requiredAlignment = requiredNames[name]
                    if (requiredAlignment != null) {
                        require(method == ZipEntry.STORED && entry.method == ZipEntry.STORED) {
                            "$name is not an uncompressed APK entry"
                        }
                        require(dataOffset % requiredAlignment == 0L) { "$name is not ${requiredAlignment}-byte aligned" }
                        found += name
                    }
                    offset = dataOffset + entry.compressedSize
                    if (flags and 0x0008 != 0) {
                        require(offset + 4 <= input.length()) { "truncated APK data descriptor" }
                        input.seek(offset)
                        offset += if (readLe32(input) == DATA_DESCRIPTOR) 16L else 12L
                    }
                }
            }
        }
        require(found == requiredNames.keys) { "APK is missing aligned stored entries: ${(requiredNames.keys - found).sorted()}" }
    }

    private fun alignmentExtra(dataOffsetWithoutExtra: Long, alignment: Int): ByteArray {
        val misaligned = (dataOffsetWithoutExtra % alignment).toInt()
        if (misaligned == 0) return ByteArray(0)
        // The extra field itself costs a 4-byte TLV header; reserve room for it so
        // the payload that follows the extra field lands on the required boundary.
        var extraTotal = alignment - misaligned
        if (extraTotal < 4) extraTotal += alignment
        val paddingBytes = extraTotal - 4
        require(paddingBytes <= 65535) { "stored entry alignment padding exceeds the ZIP extra field limit" }
        // ZIP extra fields are TLV records. Unknown IDs are ignored by Android; this
        // record adds only enough bytes to align the following stored payload.
        return ByteArray(extraTotal).also { extra ->
            extra[0] = (APK_ALIGNMENT_EXTRA_ID and 0xff).toByte()
            extra[1] = (APK_ALIGNMENT_EXTRA_ID ushr 8).toByte()
            extra[2] = (paddingBytes and 0xff).toByte()
            extra[3] = ((paddingBytes ushr 8) and 0xff).toByte()
        }
    }

    private fun crc32(bytes: ByteArray): Long = CRC32().apply { update(bytes, 0, bytes.size) }.value

    private fun isSafeEntryName(name: String): Boolean =
        name.isNotBlank() && !name.startsWith('/') && '\\' !in name &&
            name.split('/').all { it.isNotEmpty() && it != "." && it != ".." }

    private fun readLe16(input: RandomAccessFile): Int {
        val low = input.readUnsignedByte()
        val high = input.readUnsignedByte()
        return low or (high shl 8)
    }

    private fun readLe32(input: RandomAccessFile): Long =
        readLe16(input).toLong() or (readLe16(input).toLong() shl 16)

    private class CountingOutputStream(output: OutputStream) : FilterOutputStream(output) {
        var bytesWritten: Long = 0
            private set

        override fun write(value: Int) {
            out.write(value)
            bytesWritten++
        }

        override fun write(bytes: ByteArray, offset: Int, length: Int) {
            out.write(bytes, offset, length)
            bytesWritten += length
        }
    }
}
