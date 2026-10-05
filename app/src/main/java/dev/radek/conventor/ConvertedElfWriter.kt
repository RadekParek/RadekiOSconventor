package dev.radek.conventor

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Minimal self-contained Android ET_DYN writer for the statically recompiled entry code.
 *
 * Kotlin port of the host's `radek/elf_writer.py` (arm64): one exported,
 * relocation-free function, no imports, no Android lifecycle. Keeping the
 * writer tiny makes its loader contract auditable and fail-closed.
 */
internal object ConvertedElfWriter {
    // 16 KiB segment alignment: loadable on both 4 KB-page and 16 KB-page ARM64
    // devices (the APK also places the library at a 16-page-aligned offset).
    private const val PAGE_SIZE = 0x4000
    private const val SYMBOL_REGEX = "^[A-Za-z_][A-Za-z0-9_]*$"
    private const val NEEDED_LIBRARY_REGEX = "^lib[A-Za-z0-9_.+-]+\\.so$"
    private const val MAX_CODE_BYTES = 16 * 1024 * 1024
    private const val MAX_NEEDED_LIBRARIES = 64

    private fun align(value: Int, alignment: Int): Int = (value + alignment - 1) and (alignment - 1).inv()

    /** Wrap the proven ARM64 machine code in a loadable library exporting [symbol]. */
    fun buildSharedObject(
        machineCode: ByteArray,
        symbol: String,
        neededLibraries: List<String> = emptyList(),
    ): ByteArray {
        require(machineCode.isNotEmpty() && machineCode.size <= MAX_CODE_BYTES) {
            "statically recompiled ELF function is empty or exceeds the 16 MiB limit"
        }
        require(Regex(SYMBOL_REGEX).matches(symbol)) { "invalid statically recompiled ELF symbol name" }
        require(neededLibraries.size <= MAX_NEEDED_LIBRARIES &&
            neededLibraries.distinct().size == neededLibraries.size &&
            neededLibraries.all { Regex(NEEDED_LIBRARY_REGEX).matches(it) }) {
            "invalid or duplicate ELF DT_NEEDED library name"
        }

        // arm64 constants (mirrors elf_writer.py with is_64 = true).
        val elfClass = 2
        val machine = 183
        val ehdrSize = 64
        val phdrSize = 56
        val shdrSize = 64
        val dynSize = 16
        val symSize = 24
        val symbolAlignment = 8
        val phnum = 4

        val phoff = ehdrSize
        val textOffset = align(ehdrSize + phnum * phdrSize, 16)
        val textAddress = textOffset
        val textEnd = textOffset + machineCode.size
        val dataOffset = align(textEnd, PAGE_SIZE)
        val dataAddress = dataOffset

        val stringsOutput = java.io.ByteArrayOutputStream()
        stringsOutput.write(0)
        stringsOutput.write(symbol.toByteArray(Charsets.US_ASCII))
        stringsOutput.write(0)
        val neededOffsets = neededLibraries.map { library ->
            val offset = stringsOutput.size()
            stringsOutput.write(library.toByteArray(Charsets.US_ASCII))
            stringsOutput.write(0)
            offset
        }
        val strings = stringsOutput.toByteArray()
        val dynamicOffset = dataOffset
        val dynamicSize = (6 + neededOffsets.size) * dynSize
        val hashOffset = align(dynamicOffset + dynamicSize, 4)
        val hashSize = 5 * 4
        val stringsOffset = hashOffset + hashSize
        val symbolsOffset = align(stringsOffset + strings.size, symbolAlignment)

        val dynamicAddress = dataAddress + dynamicOffset - dataOffset
        val hashAddress = dataAddress + hashOffset - dataOffset
        val stringsAddress = dataAddress + stringsOffset - dataOffset
        val symbolsAddress = dataAddress + symbolsOffset - dataOffset
        val dynamicEntries = buildList {
            neededOffsets.forEach { add(1L to it.toLong()) } // DT_NEEDED
            add(4L to hashAddress.toLong())                 // DT_HASH
            add(5L to stringsAddress.toLong())              // DT_STRTAB
            add(6L to symbolsAddress.toLong())              // DT_SYMTAB
            add(10L to strings.size.toLong())               // DT_STRSZ
            add(11L to symSize.toLong())                     // DT_SYMENT
            add(0L to 0L)                                   // DT_NULL
        }
        val dynamicBlob = ByteBuffer.allocate(dynamicEntries.size * dynSize).order(ByteOrder.LITTLE_ENDIAN)
        for ((tag, value) in dynamicEntries) {
            dynamicBlob.putLong(tag).putLong(value)
        }

        // SysV ELF hash table for exactly one defined global symbol.
        val hashBlob = ByteBuffer.allocate(hashSize).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(1).putInt(2).putInt(1).putInt(0).putInt(0)

        val nullSymbol = ByteArray(symSize)
        val functionSymbol = ByteBuffer.allocate(symSize).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(1)              // st_name
            .put(0x12.toByte())     // st_info: STB_GLOBAL | STT_FUNC
            .put(0)                 // st_other
            .putShort(1)            // st_shndx: .text
            .putLong(textAddress.toLong())
            .putLong(machineCode.size.toLong())
        val symbolsBlob = nullSymbol + functionSymbol.array()

        val shstrtab = byteArrayOf(0) + ".text".toByteArray() + byteArrayOf(0) +
            ".dynstr".toByteArray() + byteArrayOf(0) + ".dynsym".toByteArray() + byteArrayOf(0) +
            ".hash".toByteArray() + byteArrayOf(0) + ".dynamic".toByteArray() + byteArrayOf(0) +
            ".shstrtab".toByteArray() + byteArrayOf(0)
        val names = LinkedHashMap<String, Int>()
        var cursor = 1
        for (sectionName in listOf(".text", ".dynstr", ".dynsym", ".hash", ".dynamic", ".shstrtab")) {
            names[sectionName] = cursor
            cursor += sectionName.length + 1
        }

        val symbolsEnd = symbolsOffset + symbolsBlob.size
        val dataEnd = maxOf(
            dynamicOffset + dynamicBlob.limit(),
            hashOffset + hashBlob.limit(),
            symbolsEnd,
        )
        require(dataEnd - dataOffset <= MAX_CODE_BYTES) { "statically recompiled ELF dynamic data exceeds the 16 MiB limit" }
        val shstrtabOffset = dataEnd
        val sectionOffset = align(shstrtabOffset + shstrtab.size, symbolAlignment)
        val sectionCount = 7
        val totalSize = sectionOffset + sectionCount * shdrSize

        val image = ByteArray(totalSize)
        machineCode.copyInto(image, textOffset)
        dynamicBlob.array().copyInto(image, dynamicOffset)
        hashBlob.array().copyInto(image, hashOffset)
        strings.copyInto(image, stringsOffset)
        symbolsBlob.copyInto(image, symbolsOffset)
        shstrtab.copyInto(image, shstrtabOffset)

        // ELF identification + header.
        byteArrayOf(0x7f, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte(),
            elfClass.toByte(), 1, 1, 0, 0).copyInto(image, 0)
        val header = ByteBuffer.wrap(image, 16, ehdrSize - 16).order(ByteOrder.LITTLE_ENDIAN)
        header.putShort(3)                    // e_type: ET_DYN
            .putShort(machine.toShort())      // e_machine: EM_AARCH64
            .putInt(1)                        // e_version
            .putLong(0)                       // e_entry
            .putLong(phoff.toLong())          // e_phoff
            .putLong(sectionOffset.toLong())  // e_shoff
            .putInt(0)                        // e_flags
            .putShort(ehdrSize.toShort())
            .putShort(phdrSize.toShort())
            .putShort(phnum.toShort())
            .putShort(shdrSize.toShort())
            .putShort(sectionCount.toShort())
            .putShort(6)                      // e_shstrndx

        val programHeaders = listOf(
            // PT_LOAD RX: file [0, textEnd) at vaddr 0
            longArrayOf(1, 5, 0, 0, 0, textEnd.toLong(), textEnd.toLong(), PAGE_SIZE.toLong()),
            // PT_LOAD RW: data segment
            longArrayOf(1, 6, dataOffset.toLong(), dataAddress.toLong(), dataAddress.toLong(),
                (dataEnd - dataOffset).toLong(), (dataEnd - dataOffset).toLong(), PAGE_SIZE.toLong()),
            // PT_DYNAMIC
            longArrayOf(2, 6, dynamicOffset.toLong(), dynamicAddress.toLong(), dynamicAddress.toLong(),
                dynamicBlob.limit().toLong(), dynamicBlob.limit().toLong(), 8),
            // PT_GNU_STACK (non-executable stack)
            longArrayOf(0x6474E551, 6, 0, 0, 0, 0, 0, 16),
        )
        programHeaders.forEachIndexed { index, values ->
            val buffer = ByteBuffer.wrap(image, phoff + index * phdrSize, phdrSize).order(ByteOrder.LITTLE_ENDIAN)
            buffer.putInt(values[0].toInt()).putInt(values[1].toInt())
                .putLong(values[2]).putLong(values[3]).putLong(values[4])
                .putLong(values[5]).putLong(values[6]).putLong(values[7])
        }

        val sectionHeaders = listOf(
            longArrayOf(0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            longArrayOf(names[".text"]!!.toLong(), 1, 6, textAddress.toLong(), textOffset.toLong(),
                machineCode.size.toLong(), 0, 0, 16, 0),
            longArrayOf(names[".dynstr"]!!.toLong(), 3, 2, stringsAddress.toLong(), stringsOffset.toLong(),
                strings.size.toLong(), 0, 0, 1, 0),
            longArrayOf(names[".dynsym"]!!.toLong(), 11, 2, symbolsAddress.toLong(), symbolsOffset.toLong(),
                symbolsBlob.size.toLong(), 2, 1, symbolAlignment.toLong(), symSize.toLong()),
            longArrayOf(names[".hash"]!!.toLong(), 5, 2, hashAddress.toLong(), hashOffset.toLong(),
                hashBlob.limit().toLong(), 3, 0, 4, 4),
            longArrayOf(names[".dynamic"]!!.toLong(), 6, 3, dynamicAddress.toLong(), dynamicOffset.toLong(),
                dynamicBlob.limit().toLong(), 2, 0, 8, dynSize.toLong()),
            longArrayOf(names[".shstrtab"]!!.toLong(), 3, 0, 0, shstrtabOffset.toLong(),
                shstrtab.size.toLong(), 0, 0, 1, 0),
        )
        sectionHeaders.forEachIndexed { index, values ->
            val buffer = ByteBuffer.wrap(image, sectionOffset + index * shdrSize, shdrSize).order(ByteOrder.LITTLE_ENDIAN)
            buffer.putInt(values[0].toInt()).putInt(values[1].toInt()).putLong(values[2])
                .putLong(values[3]).putLong(values[4]).putLong(values[5])
                .putInt(values[6].toInt()).putInt(values[7].toInt())
                .putLong(values[8]).putLong(values[9])
        }
        return image
    }
}
