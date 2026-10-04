package dev.radek.conventor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class ConvertedElfWriterTest {
    private val symbol = "Java_dev_radek_generated_MainActivity_runNative"
    private val code = byteArrayOf(0x40, 0x02, 0x80.toByte(), 0x52, 0x21, 0x04, 0x00, 0x11, 0xC0.toByte(), 0x03, 0x5F, 0xD6.toByte())

    // slice() makes absolute get(index) relative to the wrapped offset; without
    // it ByteBuffer.wrap keeps array-based indices and reads the wrong bytes.
    private fun le(buffer: ByteArray, offset: Int): ByteBuffer =
        ByteBuffer.wrap(buffer, offset, buffer.size - offset).slice().order(ByteOrder.LITTLE_ENDIAN)

    @Test
    fun `builds a valid aarch64 ET_DYN with the JNI export`() {
        val elf = ConvertedElfWriter.buildSharedObject(code, symbol)

        // ELF identification.
        assertEquals(0x7f, elf[0].toInt())
        assertEquals('E'.code.toByte(), elf[1])
        assertEquals('L'.code.toByte(), elf[2])
        assertEquals('F'.code.toByte(), elf[3])
        assertEquals(2, elf[4].toInt()) // ELFCLASS64
        assertEquals(1, elf[5].toInt()) // little endian

        val header = le(elf, 16)
        assertEquals(3, header.short.toInt())      // ET_DYN
        assertEquals(183, header.short.toInt())    // EM_AARCH64
        assertEquals(1, header.int)                // EV_CURRENT
        header.long                                // e_entry
        val phoff = header.long.toInt()
        val shoff = header.long
        header.int                                 // e_flags
        assertEquals(64, header.short.toInt())     // e_ehsize
        assertEquals(56, header.short.toInt())     // e_phentsize
        val phnum = header.short.toInt()
        assertEquals(4, phnum)
        assertEquals(64, header.short.toInt())     // e_shentsize
        val shnum = header.short.toInt()
        assertEquals(7, shnum)
        assertEquals(6, header.short.toInt())      // e_shstrndx
        for (index in 0 until phnum) {
            val programHeader = le(elf, phoff + index * 56)
            if (programHeader.getInt(0) == 1) { // PT_LOAD
                assertEquals(0L, programHeader.getLong(8) % AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT.toLong())
                assertEquals(0L, programHeader.getLong(16) % AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT.toLong())
                assertEquals(AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT.toLong(), programHeader.getLong(48))
            }
        }

        // The machine code is present verbatim; find .text through the section headers.
        var textSize = -1
        var textOffset = -1
        var dynstrOffset = -1
        var dynstrSize = -1
        var dynsymOffset = -1
        for (index in 0 until shnum) {
            val section = le(elf, shoff.toInt() + index * 64)
            val type = section.getInt(4)
            if (type == 1) { // SHT_PROGBITS == .text
                textOffset = section.getLong(24).toInt()
                textSize = section.getLong(32).toInt()
            } else if (type == 3 && dynstrOffset < 0) { // first SHT_STRTAB == .dynstr
                dynstrOffset = section.getLong(24).toInt()
                dynstrSize = section.getLong(32).toInt()
            } else if (type == 11) { // SHT_DYNSYM
                dynsymOffset = section.getLong(24).toInt()
            }
        }
        assertEquals(code.size, textSize)
        assertTrue(textOffset > 0)
        for (i in code.indices) assertEquals(code[i], elf[textOffset + i])

        // The JNI symbol name lives in .dynstr.
        val dynstr = String(elf, dynstrOffset, dynstrSize, Charsets.US_ASCII)
        assertTrue(dynstr.contains(symbol))

        // The exported symbol is STT_FUNC with the code size, pointing into .text.
        val symbolEntry = le(elf, dynsymOffset + 24) // skip the null symbol
        assertEquals(1, symbolEntry.int)             // st_name -> symbol
        assertEquals(0x12, symbolEntry.get().toInt()) // STB_GLOBAL | STT_FUNC
        symbolEntry.get()                            // st_other
        assertEquals(1, symbolEntry.short.toInt())   // st_shndx -> .text
        assertEquals(textOffset.toLong(), symbolEntry.long)
        assertEquals(code.size.toLong(), symbolEntry.long)
    }

    @Test
    fun `rejects empty code and bad symbol names`() {
        var failed = runCatching { ConvertedElfWriter.buildSharedObject(ByteArray(0), symbol) }.isFailure
        assertTrue(failed)
        failed = runCatching { ConvertedElfWriter.buildSharedObject(code, "../evil") }.isFailure
        assertTrue(failed)
        failed = runCatching {
            ConvertedElfWriter.buildSharedObject(code, symbol, neededLibraries = listOf("../libioscompat.so"))
        }.isFailure
        assertTrue(failed)
        failed = runCatching {
            ConvertedElfWriter.buildSharedObject(
                code,
                symbol,
                neededLibraries = listOf("libioscompat.so", "libioscompat.so"),
            )
        }.isFailure
        assertTrue(failed)
    }

    @Test
    fun `output is deterministic`() {
        val first = ConvertedElfWriter.buildSharedObject(code, symbol)
        val second = ConvertedElfWriter.buildSharedObject(code, symbol)
        assertTrue(first.contentEquals(second))
    }
}
