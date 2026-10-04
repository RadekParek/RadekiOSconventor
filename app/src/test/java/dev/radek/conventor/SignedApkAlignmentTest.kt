package dev.radek.conventor

import com.android.apksig.ApkSigner
import com.android.apksig.ApkVerifier
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.util.zip.ZipFile

/** Integration regression for alignment on the final, signed APK (not just the input ZIP). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class SignedApkAlignmentTest {
    private val symbol = "Java_dev_radek_generated_MainActivity_runNative"
    private val machineCode = byteArrayOf(0x40, 0x02, 0x80.toByte(), 0x52, 0x21, 0x04, 0x00, 0x11, 0xC0.toByte(), 0x03, 0x5F, 0xD6.toByte())

    @Test
    fun `v1 v2 v3 signing preserves 16KiB alignment and shim DT_NEEDED`() {
        val directory = Files.createTempDirectory("signed-native-alignment").toFile()
        try {
            val unsigned = directory.resolve("unsigned.apk")
            val signed = directory.resolve("signed.apk")
            val converted = ConvertedElfWriter.buildSharedObject(
                machineCode,
                symbol,
                neededLibraries = listOf(CompatibilityRuntime.SONAME),
            )
            val manifest = RuntimeEnvironment.getApplication().assets
                .open("converted-template/AndroidManifest.xml").use { it.readBytes() }
            val shim = minimalArm64Elf()
            val entries = listOf(
                AlignedApkZip.Entry("AndroidManifest.xml", manifest),
                AlignedApkZip.Entry("classes.dex", ByteArray(32) { (it * 3).toByte() }),
                AlignedApkZip.Entry("lib/arm64-v8a/libconverted.so", converted,
                    alignment = AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT),
                AlignedApkZip.Entry("lib/arm64-v8a/${CompatibilityRuntime.SONAME}", shim,
                    alignment = AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT),
            )
            val nativeAlignments = mapOf(
                "lib/arm64-v8a/libconverted.so" to AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT,
                "lib/arm64-v8a/${CompatibilityRuntime.SONAME}" to AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT,
            )
            AlignedApkZip.write(unsigned, entries)
            AlignedApkZip.verify(unsigned, entries.map { it.name }.toSet(), nativeAlignments)

            val identity = PlaceholderSigningIdentity.loadOrCreate(directory.resolve("identity.bin"))
            val config = ApkSigner.SignerConfig.Builder(
                "alignment integration test",
                identity.privateKey,
                listOf(identity.certificate),
            ).build()
            ApkSigner.Builder(listOf(config))
                .setInputApk(unsigned)
                .setOutputApk(signed)
                .setMinSdkVersion(26)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                .setAlignmentPreserved(true)
                .setLibraryPageAlignmentBytes(AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT)
                .build()
                .sign()

            val verification = ApkVerifier.Builder(signed).build().verify()
            assertTrue("signed test APK failed verification: ${verification.errors}", verification.isVerified)
            AlignedApkZip.verifyAlignedEntries(signed, nativeAlignments)
            ZipFile(signed).use { zip ->
                val library = zip.getInputStream(zip.getEntry("lib/arm64-v8a/libconverted.so")).use { it.readBytes() }
                assertEquals(listOf(CompatibilityRuntime.SONAME), dynamicNeeded(library))
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    private fun dynamicNeeded(elf: ByteArray): List<String> {
        val header = le(elf, 16)
        header.short // e_type
        header.short // e_machine
        header.int // e_version
        header.long // e_entry
        header.long // e_phoff
        val sectionOffset = header.long.toInt()
        header.int // e_flags
        header.short // e_ehsize
        header.short // e_phentsize
        header.short // e_phnum
        header.short // e_shentsize
        val sectionCount = header.short.toInt()
        header.short // e_shstrndx
        var stringsOffset = -1
        var dynamicOffset = -1
        var dynamicSize = -1
        for (index in 0 until sectionCount) {
            val section = le(elf, sectionOffset + index * 64)
            val type = section.getInt(4)
            val offset = section.getLong(24).toInt()
            val size = section.getLong(32).toInt()
            if (type == 3 && stringsOffset < 0) stringsOffset = offset // .dynstr is first SHT_STRTAB
            if (type == 6) {
                dynamicOffset = offset
                dynamicSize = size
            }
        }
        require(stringsOffset >= 0 && dynamicOffset >= 0 && dynamicSize >= 16)
        // The dynamic string table size comes from DT_STRSZ.
        val entries = ArrayList<Pair<Long, Long>>()
        val dynamic = le(elf, dynamicOffset)
        var cursor = 0
        while (cursor + 16 <= dynamicSize) {
            val tag = dynamic.getLong(cursor)
            val value = dynamic.getLong(cursor + 8)
            entries += tag to value
            cursor += 16
            if (tag == 0L) break
        }
        val stringTableSize = entries.first { it.first == 10L }.second.toInt()
        val table = elf.copyOfRange(stringsOffset, stringsOffset + stringTableSize)
        return entries.filter { it.first == 1L }.map { (_, nameOffset) ->
            val start = nameOffset.toInt()
            var end = start
            while (end < table.size && table[end] != 0.toByte()) end++
            require(end < table.size)
            String(table, start, end - start, Charsets.US_ASCII)
        }
    }

    private fun minimalArm64Elf(): ByteArray = ByteArray(64).also { elf ->
        elf[0] = 0x7f; elf[1] = 'E'.code.toByte(); elf[2] = 'L'.code.toByte(); elf[3] = 'F'.code.toByte()
        elf[4] = 2; elf[5] = 1; elf[6] = 1
        elf[16] = 3; elf[18] = 183.toByte()
    }

    private fun le(bytes: ByteArray, offset: Int): ByteBuffer =
        ByteBuffer.wrap(bytes, offset, bytes.size - offset).slice().order(ByteOrder.LITTLE_ENDIAN)
}
