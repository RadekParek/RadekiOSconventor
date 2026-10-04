package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class ArtifactNamesTest {
    @Test fun outputNameUsesSanitizedPickedIpaBasename() {
        val report = JSONObject().put("source", JSONObject().put("originalName", "../My Game.ipa"))
        assertEquals("My Game.apk", ArtifactNames.apkFileName(report))
    }

    @Test fun fallsBackSafelyWhenOriginalNameIsMissing() {
        val report = JSONObject().put("application", JSONObject().put("name", "bad:game"))
        assertEquals("bad_game.apk", ArtifactNames.apkFileName(report))
    }

    @Test fun previewArtifactHasAnExplicitlySeparateFilenameWithoutPlaceholderWording() {
        val report = JSONObject().put("source", JSONObject().put("originalName", "../My Game.ipa"))
        assertEquals("My Game-preview.apk", ArtifactNames.placeholderApkFileName(report))
        assertNotEquals(ArtifactNames.apkFileName(report), ArtifactNames.placeholderApkFileName(report))
        for (name in listOf(ArtifactNames.apkFileName(report), ArtifactNames.placeholderApkFileName(report))) {
            org.junit.Assert.assertFalse(
                "user-visible artifact names must not say placeholder: $name",
                name.lowercase().contains("placeholder"),
            )
        }
    }
}
