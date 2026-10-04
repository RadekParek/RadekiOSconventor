package dev.radek.conventor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class PlaceholderTemplateAssetTest {
    @Test fun patchesTheRealAaptCompiledManifestAndResourceTableBundledByGradle() {
        val assets = RuntimeEnvironment.getApplication().assets
        val manifest = assets.open("placeholder-template/AndroidManifest.xml").use { it.readBytes() }
        val resources = assets.open("placeholder-template/resources.arsc").use { it.readBytes() }
        val packageName = "dev.radek.placeholder.p0123456789abcdef01234567"

        val patchedManifest = BinaryXmlManifest.customize(manifest, packageName, "IPA name — 🎮")
        val patchedResources = ResourceTablePackagePatcher.customize(resources, packageName)

        assertEquals(patchedManifest.size.toLong(), u32(patchedManifest, 4))
        assertEquals(resources.size, patchedResources.size)
        assertTrue(patchedManifest.isNotEmpty())
    }

    private fun u32(data: ByteArray, offset: Int): Long =
        (data[offset].toLong() and 0xff) or
            ((data[offset + 1].toLong() and 0xff) shl 8) or
            ((data[offset + 2].toLong() and 0xff) shl 16) or
            ((data[offset + 3].toLong() and 0xff) shl 24)
}
