package dev.radek.conventor

import android.graphics.BitmapFactory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.charset.StandardCharsets

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class SplashExtractorTest {
    @Test fun parsesSplashSheetDescriptorBinaryFormat() {
        val raw = ByteArrayOutputStream().apply {
            writeU16String("SPLASHES.png")
            writeU16Be(3)
            writeSprite("SPLASH_ROVIO", 1, 321, 121, 190, 60, 95)
            writeSprite("SPLASH_ANGRY_BIRDS", 1, 511, 480, 320, 240, 160)
            writeSprite("SPLASH_CLICKGAMER", 1, 1, 480, 320, 240, 160)
        }.toByteArray()

        val parsed = SplashExtractor.parseSplashSheetDescriptor(raw)
        assertNotNull(parsed)
        assertEquals("SPLASHES.png", parsed!!.sheetName)
        assertEquals(3, parsed.entries.size)
        assertEquals("SPLASH_ANGRY_BIRDS", parsed.entries[1].name)
        assertEquals(480, parsed.entries[1].width)
        assertEquals(320, parsed.entries[1].height)
    }

    @Test fun extractsNormalizedSplashScreenPngFromAngryBirdsIpa() {
        val ipaCandidates = listOf(
            File("tests/data/AngryBirds_v1.0_os30.ipa"),
            File("../tests/data/AngryBirds_v1.0_os30.ipa"),
        )
        val ipa = ipaCandidates.firstOrNull { it.isFile } ?: return
        val splashFrames = SplashExtractor.extractSplashPngs(ipa)
        assertEquals(3, splashFrames.size)
        assertTrue("Expected splash PNG to be extracted from AngryBirds_v1.0_os30.ipa", splashFrames[0].size > 64)
        splashFrames.forEach { frame ->
            val decoded = BitmapFactory.decodeByteArray(frame, 0, frame.size)
            assertNotNull(decoded)
            assertEquals(480, decoded!!.width)
            assertEquals(320, decoded.height)
        }
        assertNotNull(SplashExtractor.extractSplashPng(ipa))
    }

    private fun ByteArrayOutputStream.writeU16Be(value: Int) {
        write((value ushr 8) and 0xff)
        write(value and 0xff)
    }

    private fun ByteArrayOutputStream.writeU16String(value: String) {
        val bytes = value.toByteArray(StandardCharsets.US_ASCII)
        writeU16Be(bytes.size)
        write(bytes)
    }

    private fun ByteArrayOutputStream.writeSprite(
        name: String,
        x: Int,
        y: Int,
        width: Int,
        height: Int,
        pivotX: Int,
        pivotY: Int,
    ) {
        writeU16String(name)
        writeU16Be(x)
        writeU16Be(y)
        writeU16Be(width)
        writeU16Be(height)
        writeU16Be(pivotX)
        writeU16Be(pivotY)
    }
}
