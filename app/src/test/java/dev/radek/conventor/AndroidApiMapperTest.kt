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

    @Test fun mapperRetainsMoreThanTenThousandDistinctImports() {
        val imports = JSONArray()
        for (index in 0 until 10_001) {
            imports.put(JSONObject().put("name", "_large_game_import_$index"))
        }
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))

        val mapping = AndroidApiMapper.analyze(nodes)

        assertEquals(10_001, mapping.getInt("distinctImportSymbols"))
        assertEquals(10_001, mapping.getInt("classifiedImportSymbols"))
        assertEquals("COMPLETE", mapping.getString("classificationStatus"))
        assertFalse(mapping.getBoolean("truncated"))
    }

    @Test fun compactMachOImportTruncationIsNotReportedAsCompleteClassification() {
        val slice = JSONObject()
            .put("imports", JSONArray().put(JSONObject().put("name", "_observed_import")))
            .put("importsTruncated", true)
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(slice))))

        val mapping = AndroidApiMapper.analyze(nodes)

        assertEquals(1, mapping.getInt("distinctImportSymbols"))
        assertEquals("TRUNCATED", mapping.getString("classificationStatus"))
        assertTrue(mapping.getBoolean("truncated"))
        assertEquals(0, mapping.getInt("classificationCoveragePercent"))
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

    @Test fun compatStubHandlersAreRegisteredAndNeverCountedAsVerifiedImplementations() {
        // glDrawArrays and malloc are in the reviewed bionic catalog, so they must
        // classify as direct candidates and never reach the compat resolver; the two
        // OpenAL symbols have no other mapping and are the only resolver inputs.
        val imports = JSONArray()
            .put(JSONObject().put("name", "_glDrawArrays"))
            .put(JSONObject().put("name", "_alSourcePlay"))
            .put(JSONObject().put("name", "_alDeleteSources"))
            .put(JSONObject().put("name", "_malloc"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))
        val registered = mutableListOf<String>()

        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveCompatHandler = { symbol ->
                registered += symbol
                when (symbol) {
                    "_alSourcePlay" -> "stubbed:radek_compat_stub_0"
                    "_alDeleteSources" -> "stubbed:radek_compat_stub_1"
                    else -> null
                }
            },
        )

        // The resolver is only consulted for symbols with no other mapping.
        assertEquals(
            "resolver must see exactly the otherwise-unmapped symbols (got $registered)",
            listOf("_alDeleteSources", "_alSourcePlay"),
            registered,
        )
        assertEquals("direct bionic candidates", 2, mapping.getInt("mappedNameCandidates"))
        assertEquals("stub handler count", 2, mapping.getInt("compatStubHandlerCount"))
        assertEquals("verified handler count", 0, mapping.getInt("compatVerifiedHandlerCount"))
        assertEquals("handler coverage percent", 50, mapping.getInt("compatHandlerCoveragePercent"))
        assertEquals(
            "resolver status",
            "DYNAMIC_REGISTRY_REGISTRATION",
            mapping.getString("compatHandlerResolverStatus"),
        )
        assertEquals("unmapped after registration", 0, mapping.getInt("unmappedSymbolCount"))
        // Stubs are triage/resolution coverage, never implementation coverage.
        assertEquals(0, mapping.getInt("implementedTranslationCount"))
        assertEquals(0, mapping.getInt("implementedTranslationCoveragePercent"))
        assertEquals(0, mapping.getInt("generatedTranslationCount"))
        val items = (0 until mapping.getJSONArray("symbols").length())
            .map { mapping.getJSONArray("symbols").getJSONObject(it) }
        val stub = items.single { it.getString("sourceSymbol") == "_alDeleteSources" }
        assertEquals("stub classification", "COMPAT_STUB_HANDLER_REGISTERED", stub.getString("classification"))
        assertEquals("stub target library", "libioscompat.so", stub.getString("targetLibrary"))
        assertEquals("stub target symbol", "radek_compat_stub_1", stub.getString("targetSymbol"))
        assertFalse("stub must not claim implementation", stub.getBoolean("implementationCodePresent"))
        assertFalse("stub must not claim linking", stub.getBoolean("linkedOrRewritten"))
        assertTrue("stub reason states non-implementation", stub.getString("reason").contains("does not implement"))
        val direct = items.single { it.getString("sourceSymbol") == "_glDrawArrays" }
        assertEquals("catalog symbol keeps direct classification", "BIONIC_SYMBOL_CANDIDATE", direct.getString("classification"))
        assertEquals("catalog symbol target library", "libGLESv2.so", direct.getString("targetLibrary"))
    }

    @Test fun compatResolverExceptionsFallBackToUnmapped() {
        val imports = JSONArray().put(JSONObject().put("name", "_someDarwinApi"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))
        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveCompatHandler = { throw UnsatisfiedLinkError("native not loaded") },
        )
        assertEquals(1, mapping.getInt("unmappedSymbolCount"))
        assertEquals(0, mapping.getInt("compatStubHandlerCount"))
        assertEquals("NOT_RUN", mapping.getString("runtimeNdkResolverStatus"))
    }
}
