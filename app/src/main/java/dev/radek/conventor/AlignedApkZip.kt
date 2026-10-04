package dev.radek.conventor

import java.io.BufferedOutputStream
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FilterOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.nio.charset.StandardCharsets
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/** ZIP writer/checks used by the runtime-built APK; stored resources are 4-byte
 *  aligned and native libraries are page-aligned so the linker can mmap them
 *  straight out of the APK on 4 KB and 16 KB page devices. */
internal object AlignedApkZip {
    private const val LOCAL_FILE_HEADER = 0x04034b50L
    private const val DATA_DESCRIPTOR = 0x08074b50L
    private const val LOCAL_HEADER_BYTES = 30L
    const val ALIGNMENT = 4
    /**
     * 16 KiB matches what the host toolchain produces (`zipalign -P 16 4`).
     * It satisfies both 4 KB-page and 16 KB-page devices while keeping the
     * alignment padding comfortably inside the ZIP extra-field limit.
     */
    const val NATIVE_LIBRARY_ALIGNMENT = 16 * 1024
    private const val APK_ALIGNMENT_EXTRA_ID = 0xd935

    /** A ZIP extra field is one or more TLV records with a 16-bit header. */
    private const val EXTRA_RECORD_HEADER = 4
    private const val MAX_EXTRA_FIELD = 65535
    private const val MAX_RECORD_PAYLOAD = MAX_EXTRA_FIELD - EXTRA_RECORD_HEADER

    /**
     * One APK entry. Either [bytes] is written directly, or [source] is set and
     * the payload is streamed from that file instead of being held in memory —
     * a game bundle is far too large to buffer whole.
     */
    data class Entry(
        val name: String,
        val bytes: ByteArray,
        val compressed: Boolean = false,
        val alignment: Int = ALIGNMENT,
        val source: File? = null,
    ) {
        fun payloadSize(): Long = source?.length() ?: bytes.size.toLong()

        fun openStream(): InputStream =
            source?.inputStream()?.buffered() ?: ByteArrayInputStream(bytes)

        companion object {
            /** Stream a large payload straight from disk instead of buffering it. */
            fun stream(name: String, file: File, compressed: Boolean = false, alignment: Int = ALIGNMENT): Entry =
                Entry(name, ByteArray(0), compressed, alignment, file)
        }
    }

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
                        val size = item.payloadSize()
                        require(size > 0) { "stored APK entry is empty: ${item.name}" }
                        setSize(size)
                        compressedSize = size
                        crc = crc32(item)
                        // Padding must be decided before the local header is
                        // written, because it is stored in that header's extra
                        // field and shifts the payload that follows it.
                        val extra = alignmentExtra(counted.bytesWritten + LOCAL_HEADER_BYTES + nameBytes.size, item.alignment)
                        if (extra.isNotEmpty()) setExtra(extra)
                    }
                }
                zip.putNextEntry(entry)
                item.openStream().use { input -> input.copyTo(zip) }
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

    /**
     * Re-checks required stored-entry alignments after signing. V1 signing adds
     * META-INF files, so this intentionally validates the required payloads
     * without requiring the signed archive to have the unsigned entry set.
     */
    fun verifyAlignedEntries(file: File, alignedStoredNames: Map<String, Int>) {
        require(file.isFile && file.length() > 0) { "APK is missing or empty" }
        ZipFile(file).use { zip ->
            alignedStoredNames.forEach { (name, alignment) ->
                require(alignment > 1) { "invalid APK entry alignment for $name" }
                val entry = zip.getEntry(name) ?: error("APK is missing $name")
                require(entry.method == ZipEntry.STORED) { "$name must be stored uncompressed in the APK" }
                require(entry.size > 0 && entry.compressedSize == entry.size) { "$name has an invalid stored size" }
            }
        }
        verifyLocalAlignment(file, alignedStoredNames)
    }

    /**
     * Builds the ZIP extra field that pushes a stored payload onto [alignment].
     *
     * The extra field length is a 16-bit field, so a single record can only ever
     * express 65535 bytes. Rather than emitting a truncated length (which
     * corrupts the local file header and makes Android refuse to install the
     * APK), the padding is split across as many TLV records as it needs.
     */
    internal fun alignmentExtra(dataOffsetWithoutExtra: Long, alignment: Int): ByteArray {
        require(alignment > 1) { "invalid APK entry alignment" }
        val misaligned = (dataOffsetWithoutExtra % alignment).toInt()
        if (misaligned == 0) return ByteArray(0)
        var total = alignment - misaligned
        // Fewer than a record header's worth of padding cannot be expressed, so
        // skip to the next boundary.
        if (total < EXTRA_RECORD_HEADER) total += alignment
        var records = 1
        while (total > MAX_EXTRA_FIELD * records) {
            records++
            require(records <= 8) { "APK entry alignment padding exceeds the ZIP extra field limit" }
        }
        var payload = total - EXTRA_RECORD_HEADER * records
        require(payload >= 0) { "APK entry alignment padding exceeds the ZIP extra field limit" }
        val extra = ByteArray(total)
        var offset = 0
        for (index in 0 until records) {
            val chunk = minOf(payload, MAX_RECORD_PAYLOAD)
            extra[offset] = (APK_ALIGNMENT_EXTRA_ID and 0xff).toByte()
            extra[offset + 1] = (APK_ALIGNMENT_EXTRA_ID ushr 8).toByte()
            extra[offset + 2] = (chunk and 0xff).toByte()
            extra[offset + 3] = ((chunk ushr 8) and 0xff).toByte()
            offset += EXTRA_RECORD_HEADER + chunk
            payload -= chunk
        }
        return extra
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
                    require(extraLength in 0..MAX_EXTRA_FIELD) { "APK local file extra field is too large" }
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

    private fun crc32(entry: Entry): Long {
        val crc = CRC32()
        entry.openStream().use { input ->
            val buffer = ByteArray(65536)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                crc.update(buffer, 0, count)
            }
        }
        return crc.value
    }

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
