package dev.radek.conventor

import java.io.File
import java.io.RandomAccessFile
import java.util.zip.ZipFile

/** Rejects symlinks, ZIP64, encrypted members, duplicate paths and ZIP bombs. */
object SafeZip {
    /**
     * There is no fixed archive size limit. 512 MiB was an arbitrary number that
     * stopped real games from ever being analyzed, so an archive is now bounded
     * only by the device's own free storage plus the per-member guards below
     * (expansion ratio, member count, encrypted and ZIP64 members), which exist
     * to stop ZIP bombs rather than to police how large a game may be.
     */
    private const val MAX_FILE = 1024L * 1024 * 1024
    private const val MIN_FREE_HEADROOM = 64L * 1024 * 1024
    /** Strict form: used for names that must already be canonical. */
    fun validateName(name: String): String {
        require(name.isNotEmpty() && !name.startsWith('/') && '\\' !in name && ':' !in name && '\u0000' !in name) { "unsafe ZIP path" }
        val parts = name.trimEnd('/').split('/')
        require(parts.size <= 32 && name.toByteArray().size <= 1024 && parts.none { it == ".." || it == "." || it.isEmpty() }) { "unsafe ZIP path" }
        return parts.joinToString("/")
    }

    /**
     * Lenient form for real archives. Shipped IPAs contain names that are odd but
     * harmless (backslash separators, Windows drive prefixes, `//`, `./`), and
     * failing the whole import over one of them is worse than normalising it.
     *
     * Traversal, NUL bytes, absolute paths that survive normalisation, over-deep
     * and over-long names still throw, and every result is written under the
     * destination directory only.
     */
    fun memberName(name: String): String {
        require(name.isNotEmpty() && '\u0000' !in name) { "unsafe ZIP path" }
        var candidate = name.replace('\\', '/').replace(':', '_')
        while (candidate.startsWith("/")) candidate = candidate.substring(1)
        val parts = candidate.split('/').map { it.trim() }.filter { it.isNotEmpty() && it != "." }
        require(parts.isNotEmpty() && parts.none { it == ".." }) { "unsafe ZIP path" }
        require(parts.size <= 32) { "unsafe ZIP path" }
        val joined = parts.joinToString("/")
        require(joined.toByteArray().size <= 1024) { "unsafe ZIP path" }
        return joined
    }

    /** True when the two names differ only by case (ZIP is case-insensitive on iOS). */
    fun collides(existing: Set<String>, name: String): Boolean =
        existing.contains(name.lowercase(java.util.Locale.ROOT))
    private fun centralDirectory(file: File) {
        RandomAccessFile(file, "r").use { f ->
            val length = f.length()
            require(length >= 22) { "invalid IPA size" }
            val tail = ByteArray(minOf(length, 65557).toInt())
            f.seek(length - tail.size); f.readFully(tail)
            fun u16(b: ByteArray, p: Int): Int = (b[p].toInt() and 255) or ((b[p+1].toInt() and 255) shl 8)
            fun u32(b: ByteArray, p: Int): Long = u16(b, p).toLong() or (u16(b, p+2).toLong() shl 16)
            val end = (tail.size - 22 downTo 0).firstOrNull { u32(tail, it) == 0x06054b50L && it + 22 + u16(tail, it + 20) == tail.size }
                ?: error("ZIP end record missing")
            require(u16(tail, end+4) == 0 && u16(tail, end+6) == 0) { "multi-disk ZIP unsupported" }
            val entries = u16(tail, end+10)
            require(entries <= 20000 && entries == u16(tail, end+8)) { "ZIP entry limit / ZIP64 unsupported" }
            val size = u32(tail, end+12); val offset = u32(tail, end+16)
            require(size <= 16 * 1024 * 1024 && offset + size == length - tail.size + end) { "invalid central directory / ZIP64 unsupported" }
            f.seek(offset)
            val header = ByteArray(46)
            repeat(entries) {
                require(f.filePointer + 46 <= offset + size)
                f.readFully(header)
                require(u32(header, 0) == 0x02014b50L) { "invalid central directory signature" }
                require((u16(header, 8) and 1) == 0) { "encrypted ZIP prohibited" }
                require(u16(header, 10) in listOf(0, 8)) { "unsupported ZIP compression" }
                val kind = (u32(header, 38) shr 16).toInt() and 0xf000
                require(kind in listOf(0, 0x8000, 0x4000)) { "ZIP links / special files prohibited" }
                require(u32(header, 20) != 0xffffffffL && u32(header, 24) != 0xffffffffL && u32(header, 42) != 0xffffffffL) { "ZIP64 unsupported on device" }
                val remaining = u16(header, 28) + u16(header, 30) + u16(header, 32)
                require(f.filePointer + remaining <= offset + size)
                f.seek(f.filePointer + remaining)
            }
            require(f.filePointer == offset + size)
        }
    }
    /**
     * The only archive-level bound that remains: the device must have room for
     * the archive plus everything it expands to.
     */
    fun requireStorage(destination: File, requiredBytes: Long) {
        var directory: File? = destination
        while (directory != null && !directory.isDirectory) directory = directory.parentFile
        val usable = try { directory?.usableSpace ?: Long.MAX_VALUE } catch (_: Exception) { Long.MAX_VALUE }
        require(usable >= requiredBytes + MIN_FREE_HEADROOM) {
            "not enough free storage: ${formatMib(requiredBytes)} MiB needed, ${formatMib(maxOf(0L, usable))} MiB free"
        }
    }

    private fun formatMib(bytes: Long): String = (bytes / (1024 * 1024)).toString()

    fun extract(
        source: File,
        destination: File,
        onBytes: (Long, Long, String) -> Unit = { _, _, _ -> },
        onFile: (Int, Int) -> Unit = { _, _ -> },
    ) {
        require(!destination.exists()) { "workspace already exists" }
        centralDirectory(source)
        requireStorage(destination, source.length())
        require(destination.mkdirs())
        try {
            ZipFile(source).use { zip ->
                val entries = zip.entries().toList()
                require(entries.size <= 20000)
                val uncompressedTotal = entries.sumOf { it.size }
                requireStorage(destination, uncompressedTotal + source.length())
                val names = mutableSetOf<String>()
                var extractedBytes = 0L
                entries.forEachIndexed { index, entry ->
                    val name = memberName(entry.name)
                    require(names.add(name.lowercase(java.util.Locale.ROOT))) { "duplicate/case-colliding ZIP path" }
                    require(entry.size in 0..MAX_FILE && entry.compressedSize >= 0 && entry.size <= maxOf(1L, entry.compressedSize) * 250) { "ZIP expansion limit" }
                    val target = File(destination, name)
                    require(target.canonicalPath.startsWith(destination.canonicalPath + File.separator))
                    if (entry.isDirectory) {
                        require(target.isDirectory || target.mkdirs())
                    } else {
                        require(target.parentFile!!.isDirectory || target.parentFile!!.mkdirs())
                        require(target.createNewFile()) { "ZIP path collision" }
                        zip.getInputStream(entry).use { input ->
                            target.outputStream().use { output ->
                                val buffer = ByteArray(65536); var written = 0L
                                val crc = java.util.zip.CRC32()
                                while (true) {
                                    val count = input.read(buffer); if (count < 0) break
                                    written += count; require(written <= entry.size && written <= MAX_FILE)
                                    crc.update(buffer, 0, count); output.write(buffer, 0, count)
                                    extractedBytes += count
                                    onBytes(extractedBytes, uncompressedTotal, name)
                                }
                                require(written == entry.size && crc.value == entry.crc) { "ZIP size/CRC mismatch" }
                            }
                        }
                    }
                    onFile(index + 1, entries.size)
                }
            }
        } catch (error: Throwable) {
            destination.deleteRecursively()
            throw error
        }
    }
}
