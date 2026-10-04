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

    @Test fun convertedTemplateBundlesAPatchableManifestResourceTableAndLauncherDex() {
        val assets = RuntimeEnvironment.getApplication().assets
        val manifest = assets.open("converted-template/AndroidManifest.xml").use { it.readBytes() }
        val resources = assets.open("converted-template/resources.arsc").use { it.readBytes() }
        val dex = assets.open("converted-template/classes.dex").use { it.readBytes() }
        val iconPath = assets.open("converted-template/icon-entry-path.txt").use { it.readBytes() }
            .toString(Charsets.UTF_8)

        val packageName = "dev.radek.converted.p0123456789abcdef0123"
        val patchedManifest = BinaryXmlManifest.customize(manifest, packageName, "Hello Test")
        val patchedResources = ResourceTablePackagePatcher.customize(resources, packageName)
        assertEquals(patchedManifest.size.toLong(), u32(patchedManifest, 4))
        assertEquals(resources.size, patchedResources.size)

        // The launcher DEX must define the contract entry class, and the icon
        // entry path must point at the generated converted icon resource.
        assertTrue(
            "classes.dex has a bad header: size=${dex.size} first=${dex.take(8).joinToString { "%02x".format(it) }}",
            dex.size > 0x70 && dex[0] == 'd'.code.toByte() && dex[1] == 'e'.code.toByte(),
        )
        val dexText = String(dex, Charsets.ISO_8859_1)
        val strings = Regex("[ -~]{12,}").findAll(dexText).map { it.value }.distinct().take(40).toList()
        assertTrue(
            "classes.dex does not contain the launcher class descriptor; strings found: $strings",
            dexText.contains("Ldev/radek/generated/MainActivity;"),
        )
        assertTrue(
            "icon entry path is invalid: '$iconPath'",
            iconPath.startsWith("res/") && iconPath.endsWith("generated_converted_icon.png"),
        )
    }

    private fun u32(data: ByteArray, offset: Int): Long =
        (data[offset].toLong() and 0xff) or
            ((data[offset + 1].toLong() and 0xff) shl 8) or
            ((data[offset + 2].toLong() and 0xff) shl 16) or
            ((data[offset + 3].toLong() and 0xff) shl 24)
}
