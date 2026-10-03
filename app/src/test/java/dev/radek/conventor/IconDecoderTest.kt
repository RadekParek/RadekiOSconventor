package dev.radek.conventor

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.util.zip.CRC32
import java.util.zip.Deflater
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class IconDecoderTest {
    private fun chunk(name: String, payload: ByteArray): ByteArray {
        val type = name.toByteArray(Charsets.US_ASCII)
        val output = ByteArrayOutputStream()
        output.write(ByteBuffer.allocate(4).putInt(payload.size).array())
        output.write(type)
        output.write(payload)
        val crc = CRC32().apply { update(type); update(payload) }.value.toInt()
        output.write(ByteBuffer.allocate(4).putInt(crc).array())
        return output.toByteArray()
    }

    private fun cgbiPng(): ByteArray {
        val signature = byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
        val header = ByteBuffer.allocate(13).putInt(1).putInt(1).put(8.toByte()).put(6.toByte())
            .put(0.toByte()).put(0.toByte()).put(0.toByte()).array()
        // CgBI stores premultiplied BGRA: (B=16, G=32, R=64, A=128)
        // decodes to straight-alpha RGBA (R=128, G=64, B=32, A=128).
        val deflater = Deflater(9, true)
        val compressed = try {
            deflater.setInput(byteArrayOf(0.toByte(), 16.toByte(), 32.toByte(), 64.toByte(), 128.toByte()))
            deflater.finish()
            val bytes = ByteArray(64)
            val count = deflater.deflate(bytes)
            bytes.copyOf(count)
        } finally {
            deflater.end()
        }
        return signature + chunk("CgBI", byteArrayOf(2, 0, 0, 0)) + chunk("IHDR", header) +
            chunk("IDAT", compressed) + chunk("IEND", byteArrayOf())
    }

    private fun platformPng(color: Int): ByteArray {
        val bitmap = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(color)
        return try {
            ByteArrayOutputStream().use { output ->
                assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output))
                output.toByteArray()
            }
        } finally {
            bitmap.recycle()
        }
    }

    @Test fun decodesCgbiRawDeflateAndRestoresStraightAlpha() {
        val bitmap = IconDecoder.decodeCgbi(cgbiPng(), 512)
        try {
            assertEquals(1, bitmap.width)
            assertEquals(Color.argb(128, 128, 64, 32), bitmap.getPixel(0, 0))
        } finally {
            bitmap.recycle()
        }
    }

    @Test fun declaredBrokenIconFallsBackToAnotherBundleImage() {
        val root = createTempDir(prefix = "radek-icon-test")
        try {
            val app = File(root, "Fixture.app").apply { mkdirs() }
            File(app, "DeclaredIcon.png").writeBytes(byteArrayOf(1, 2, 3, 4))
            File(app, "GameLogo.png").writeBytes(platformPng(Color.MAGENTA))
            val output = File(root, "result").apply { mkdirs() }

            val result = extractIcon(app, listOf("DeclaredIcon"), output)

            assertEquals("SUPPORTED", result.getString("status"))
            assertTrue("unexpected icon source: ${result.getString("source")}", result.getString("source").endsWith("GameLogo.png"))
            assertTrue(result.getJSONArray("attempts").length() >= 2)
            val saved = BitmapFactory.decodeFile(File(output, "icon.png").path)
            assertNotNull(saved)
            assertEquals(Color.MAGENTA, saved!!.getPixel(0, 0))
            saved.recycle()
        } finally {
            root.deleteRecursively()
        }
    }
}
