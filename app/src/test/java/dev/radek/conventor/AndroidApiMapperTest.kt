package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class AndroidApiMapperTest {
    @Test fun reportsDirectAndSemanticCandidatesWithoutClaimingGeneratedCode() {
        val imports = JSONArray()
            .put(JSONObject().put("name", "_malloc"))
            .put(JSONObject().put("name", "_objc_msgSend"))
            .put(JSONObject().put("name", "_OBJC_CLASS_" + '$' + "_UIView"))
            .put(JSONObject().put("name", "_malloc"))
        val slice = JSONObject().put("imports", imports)
        val analysis = JSONObject().put("slices", JSONArray().put(slice))
        val nodes = JSONArray().put(JSONObject().put("analysis", analysis))

        val mapping = AndroidApiMapper.analyze(nodes)

        assertEquals(3, mapping.getInt("distinctImportSymbols"))
        assertEquals(1, mapping.getInt("mappedNameCandidates"))
        assertEquals(33, mapping.getInt("candidateCoveragePercent"))
        assertEquals(1, mapping.getInt("semanticRewriteCandidates"))
        assertEquals(0, mapping.getInt("generatedTranslationCount"))
        assertEquals(3, mapping.getInt("classifiedImportSymbols"))
        assertEquals(100, mapping.getInt("classificationCoveragePercent"))
        assertEquals("COMPLETE", mapping.getString("classificationStatus"))
        assertEquals(1, mapping.getInt("unmappedSymbolCount"))
        assertEquals(0, mapping.getInt("implementedTranslationCoveragePercent"))
        val symbols = mapping.getJSONArray("symbols")
        val decoded = (0 until symbols.length()).map { symbols.getJSONObject(it) }
        val malloc = decoded.single { it.getString("sourceSymbol") == "_malloc" }
        val objc = decoded.single { it.getString("sourceSymbol") == "_objc_msgSend" }
        val view = decoded.single { it.getString("sourceSymbol").contains("UIView") }
        assertEquals("libc.so", malloc.getString("targetLibrary"))
        assertFalse(malloc.getBoolean("linkedOrRewritten"))
        assertEquals("UNMAPPED", objc.getString("classification"))
        assertEquals("SEMANTIC_REWRITE_CANDIDATE", view.getString("classification"))
        assertEquals("android.view.View", view.getString("targetApi"))
        assertFalse(view.getBoolean("codeGenerated"))
        assertTrue(mapping.getString("measure").contains("triage, not implementation coverage"))
    }

    @Test fun runtimeResolverAddsOnlyAConfirmedPublicNdkExportAndKeepsImplementationAtZero() {
        val imports = JSONArray()
            .put(JSONObject().put("name", "_android_log_write"))
            .put(JSONObject().put("name", "_unavailable_apple_api"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))
        val seen = mutableListOf<String>()

        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveNdkLibrary = { symbol ->
                seen += symbol
                when (symbol) {
                    "android_log_write" -> "liblog.so"
                    "unavailable_apple_api" -> "libprivate.so" // not an allowed NDK library
                    else -> null
                }
            },
            runtimeApiLevel = 35,
        )

        assertEquals(listOf("android_log_write", "unavailable_apple_api"), seen)
        assertEquals(1, mapping.getInt("mappedNameCandidates"))
        assertEquals(50, mapping.getInt("candidateCoveragePercent"))
        assertEquals(1, mapping.getInt("runtimeVerifiedNdkCandidates"))
        assertEquals(50, mapping.getInt("runtimeVerifiedCoveragePercent"))
        assertEquals("CURRENT_DEVICE_DLSYM", mapping.getString("runtimeNdkResolverStatus"))
        assertEquals(35, mapping.getInt("runtimeVerifiedAndroidApiLevel"))
        assertEquals(100, mapping.getInt("classificationCoveragePercent"))
        assertEquals(0, mapping.getInt("implementedTranslationCoveragePercent"))
        val resolved = mapping.getJSONArray("symbols").getJSONObject(0)
        assertEquals("liblog.so", resolved.getString("targetLibrary"))
        assertTrue(resolved.getBoolean("verifiedOnDevice"))
        assertEquals("RUNTIME_DLSYM", resolved.getString("resolutionEvidence"))
        assertFalse(resolved.getBoolean("linkedOrRewritten"))
    }

    @Test fun realTimeApiShimExportsAreReportedSeparatelyFromNdkNameCandidates() {
        val imports = JSONArray()
            .put(JSONObject().put("name", "_CFAbsoluteTimeGetCurrent"))
            .put(JSONObject().put("name", "_mach_timebase_info"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))
        val seen = mutableListOf<String>()

        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveApiReplacement = { source ->
                seen += source
                when (source) {
                    "_CFAbsoluteTimeGetCurrent" -> "libioscompat.so:CFAbsoluteTimeGetCurrent"
                    "_mach_timebase_info" -> "libioscompat.so:mach_timebase_info"
                    else -> null
                }
            },
        )

        assertEquals(listOf("_CFAbsoluteTimeGetCurrent", "_mach_timebase_info"), seen)
        assertEquals(0, mapping.getInt("mappedNameCandidates"))
        assertEquals(2, mapping.getInt("implementedApiReplacementCount"))
        assertEquals(2, mapping.getInt("runtimeVerifiedApiReplacementCount"))
        assertEquals("CURRENT_DEVICE_COMPAT_DLSYM", mapping.getString("runtimeApiReplacementResolverStatus"))
        assertEquals(0, mapping.getInt("generatedTranslationCount"))
        assertEquals(0, mapping.getInt("implementedTranslationCount"))
        val items = mapping.getJSONArray("symbols")
        val time = items.getJSONObject(0)
        assertEquals("IMPLEMENTED_API_REPLACEMENT_AVAILABLE", time.getString("classification"))
        assertEquals("libioscompat.so", time.getString("targetLibrary"))
        assertTrue(time.getBoolean("implementationCodePresent"))
        assertTrue(time.getBoolean("runtimeVerified"))
        assertFalse(time.getBoolean("linkedOrRewritten"))
        assertFalse(time.getBoolean("codeGenerated"))
    }

    @Test fun emptyImportSetDoesNotClaimPerfectCoverage() {
        val mapping = AndroidApiMapper.analyze(JSONArray())
        assertEquals(0, mapping.getInt("candidateCoveragePercent"))
        assertEquals(0, mapping.getInt("distinctImportSymbols"))
        assertEquals(0, mapping.getInt("classificationCoveragePercent"))
        assertEquals(0, mapping.getInt("generatedTranslationCount"))
        assertEquals(0, mapping.getInt("implementedTranslationCoveragePercent"))
    }
}
