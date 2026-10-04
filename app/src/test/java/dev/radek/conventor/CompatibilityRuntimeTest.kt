package dev.radek.conventor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class CompatibilityRuntimeTest {
    @Test
    fun `extracts validated arm64 runtime dependencies from base and ABI splits`() {
        val directory = Files.createTempDirectory("compat-runtime").toFile()
        try {
            val baseApk = directory.resolve("base.apk")
            val abiSplit = directory.resolve("split_config.arm64_v8a.apk")
            val runtimeBytes = minimalElf(machine = 183)
            val libcxxBytes = minimalElf(machine = 183)
            ZipOutputStream(baseApk.outputStream()).use { zip ->
                zip.putNextEntry(ZipEntry("lib/arm64-v8a/libc++_shared.so"))
                zip.write(libcxxBytes)
                zip.closeEntry()
                zip.putNextEntry(ZipEntry("lib/armeabi-v7a/${CompatibilityRuntime.SONAME}"))
                zip.write(minimalElf(machine = 40))
                zip.closeEntry()
            }
            ZipOutputStream(abiSplit.outputStream()).use { zip ->
                zip.putNextEntry(ZipEntry("lib/arm64-v8a/${CompatibilityRuntime.SONAME}"))
                zip.write(runtimeBytes)
                zip.closeEntry()
            }

            val libraries = CompatibilityRuntime.extractFromApks(listOf(baseApk, abiSplit), directory.resolve("staged"))

            assertEquals(listOf(CompatibilityRuntime.SONAME, "libc++_shared.so"), libraries.map { it.soname })
            assertEquals(listOf("lib/arm64-v8a/${CompatibilityRuntime.SONAME}", "lib/arm64-v8a/libc++_shared.so"),
                libraries.map { it.apkPath })
            assertArrayEquals(runtimeBytes, libraries.first().file.readBytes())
            assertEquals(64, libraries.first().sha256.length)
            assertTrue(libraries.all { it.file.isFile })
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test
    fun `fails closed when required runtime is missing or not arm64`() {
        val directory = Files.createTempDirectory("compat-runtime-invalid").toFile()
        try {
            val missing = directory.resolve("missing.apk")
            ZipOutputStream(missing.outputStream()).use { zip ->
                zip.putNextEntry(ZipEntry("assets/not-a-runtime.so"))
                zip.write(minimalElf(machine = 183))
                zip.closeEntry()
            }
            assertTrue(runCatching {
                CompatibilityRuntime.extractFromApk(missing, directory.resolve("missing-out"))
            }.isFailure)

            val wrongMachine = directory.resolve("wrong-arch.apk")
            ZipOutputStream(wrongMachine.outputStream()).use { zip ->
                zip.putNextEntry(ZipEntry("lib/arm64-v8a/${CompatibilityRuntime.SONAME}"))
                zip.write(minimalElf(machine = 40))
                zip.closeEntry()
            }
            assertTrue(runCatching {
                CompatibilityRuntime.extractFromApk(wrongMachine, directory.resolve("wrong-out"))
            }.isFailure)
            assertTrue(!directory.resolve("wrong-out/${CompatibilityRuntime.SONAME}").exists())
        } finally {
            directory.deleteRecursively()
        }
    }

    private fun minimalElf(machine: Int): ByteArray = ByteArray(64).also { elf ->
        elf[0] = 0x7f; elf[1] = 'E'.code.toByte(); elf[2] = 'L'.code.toByte(); elf[3] = 'F'.code.toByte()
        elf[4] = 2; elf[5] = 1; elf[6] = 1
        elf[16] = 3
        elf[18] = (machine and 0xff).toByte()
        elf[19] = ((machine ushr 8) and 0xff).toByte()
    }
}
