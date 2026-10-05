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
        assertEquals(0, mapping.getInt("generatedApiImplementationCount"))
        assertEquals(3, mapping.getInt("classifiedImportSymbols"))
        assertEquals(100, mapping.getInt("classificationCoveragePercent"))
        assertEquals("COMPLETE", mapping.getString("classificationStatus"))
        assertEquals(1, mapping.getInt("unmappedSymbolCount"))
        assertEquals(0, mapping.getInt("linkedImplementationCoveragePercent"))
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
        assertEquals(0, mapping.getInt("linkedImplementationCoveragePercent"))
        val resolved = mapping.getJSONArray("symbols").getJSONObject(0)
        assertEquals("liblog.so", resolved.getString("targetLibrary"))
        assertTrue(resolved.getBoolean("verifiedOnDevice"))
        assertEquals("RUNTIME_DLSYM", resolved.getString("resolutionEvidence"))
        assertFalse(resolved.getBoolean("linkedOrRewritten"))
    }

    @Test fun runtimeExportCoverageUsesCandidateAndAllImportDenominatorsSeparately() {
        assertEquals("167/264 must round to 63%, not 65%", 63, AndroidApiMapper.coveragePercent(167, 264))
        assertEquals("172/264 name candidates round to 65%", 65, AndroidApiMapper.coveragePercent(172, 264))
        assertEquals(0, AndroidApiMapper.coveragePercent(167, 0))
        val imports = JSONArray()
        // Five catalog-only candidates model symbols that look like direct NDK
        // names but are not exported by this device/API level.
        listOf("_malloc", "_free", "_memcpy", "_sin", "_glDrawArrays")
            .forEach { imports.put(JSONObject().put("name", it)) }
        // 167 exact public-library hits plus 92 Apple-only names produce the
        // reported-size case: 172 name candidates among 264 imports, with 167
        // current-device exports actually verified.
        repeat(167) { imports.put(JSONObject().put("name", "_runtime_export_$it")) }
        repeat(92) { imports.put(JSONObject().put("name", "_apple_only_$it")) }
        assertEquals(264, imports.length())
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))

        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveNdkLibrary = { symbol ->
                if (symbol.startsWith("runtime_export_")) "libnativewindow.so" else null
            },
            runtimeApiLevel = 36,
        )

        assertEquals(172, mapping.getInt("mappedNameCandidates"))
        assertEquals(65, mapping.getInt("candidateCoveragePercent"))
        assertEquals(167, mapping.getInt("runtimeVerifiedNdkCandidates"))
        assertEquals(172, mapping.getInt("runtimeVerifiedCandidateCount"))
        assertEquals(97, mapping.getInt("runtimeVerifiedCandidateCoveragePercent"))
        assertEquals(63, mapping.getInt("runtimeVerifiedImportCoveragePercent"))
        // The legacy field is explicitly retained as the all-import denominator.
        assertEquals(63, mapping.getInt("runtimeVerifiedCoveragePercent"))
        assertEquals(36, mapping.getInt("runtimeVerifiedAndroidApiLevel"))
    }

    @Test fun resolverAcceptsAdditionalPublicNdkLibraries() {
        val imports = JSONArray()
            .put(JSONObject().put("name", "_ANativeWindow_lock"))
            .put(JSONObject().put("name", "_sync_wait"))
            .put(JSONObject().put("name", "_ANeuralNetworksCompilation_createForDevices"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))

        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveNdkLibrary = { symbol -> when (symbol) {
                "ANativeWindow_lock" -> "libnativewindow.so"
                "sync_wait" -> "libsync.so"
                "ANeuralNetworksCompilation_createForDevices" -> "libneuralnetworks.so"
                else -> null
            } },
            runtimeApiLevel = 36,
        )

        assertEquals(3, mapping.getInt("mappedNameCandidates"))
        assertEquals(3, mapping.getInt("runtimeVerifiedNdkCandidates"))
        assertEquals(100, mapping.getInt("runtimeVerifiedCandidateCoveragePercent"))
        assertEquals(100, mapping.getInt("runtimeVerifiedImportCoveragePercent"))
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
        assertEquals(0, mapping.getInt("generatedApiImplementationCount"))
        assertEquals(0, mapping.getInt("linkedImplementationCount"))
        val items = mapping.getJSONArray("symbols")
        val time = items.getJSONObject(0)
        assertEquals("IMPLEMENTED_API_REPLACEMENT_AVAILABLE", time.getString("classification"))
        assertEquals("libioscompat.so", time.getString("targetLibrary"))
        assertTrue(time.getBoolean("implementationCodePresent"))
        assertTrue(time.getBoolean("runtimeVerified"))
        assertFalse(time.getBoolean("linkedOrRewritten"))
        assertFalse(time.getBoolean("codeGenerated"))
    }

    @Test fun providersExposeConcreteCompatibilityExportsWithoutClaimingLinkage() {
        assertEquals("libioscompat.so:radek_compat_CFRunLoopGetMain", Providers.forSymbol("_CFRunLoopGetMain"))
        assertEquals("libioscompat.so:radek_compat_malloc", Providers.forSymbol("_malloc"))
        assertTrue(Providers.forSymbol("___divdi3")!!.contains("not linked"))
    }

    @Test fun coreFoundationRunLoopExportsAreConcreteBodiesButNotGeneratedOrLinked() {
        val imports = JSONArray()
            .put(JSONObject().put("name", "_CFRunLoopGetCurrent"))
            .put(JSONObject().put("name", "_CFRunLoopRunInMode"))
            .put(JSONObject().put("name", "_CFRunLoopStop"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))

        val mapping = AndroidApiMapper.analyze(nodes)
        val items = mapping.getJSONArray("symbols")

        assertEquals(3, mapping.getInt("implementedApiReplacementCount"))
        for (index in 0 until items.length()) {
            val item = items.getJSONObject(index)
            assertEquals("IMPLEMENTED_API_REPLACEMENT_AVAILABLE", item.getString("classification"))
            assertEquals("libioscompat.so", item.getString("targetLibrary"))
            assertFalse(item.getBoolean("linkedOrRewritten"))
            assertFalse(item.getBoolean("codeGenerated"))
        }
    }

    @Test fun compilerRuntimeImportsAreToolchainCandidatesNotDirectSharedLibraryLinks() {
        val imports = JSONArray()
            .put(JSONObject().put("name", "__Unwind_Resume"))
            .put(JSONObject().put("name", "___aeabi_uidiv"))
            .put(JSONObject().put("name", "___divti3"))
            .put(JSONObject().put("name", "___divdi3"))
            .put(JSONObject().put("name", "__aeabi_unwind_cpp_pr0"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))

        val mapping = AndroidApiMapper.analyze(nodes)
        val items = mapping.getJSONArray("symbols")
        val decoded = (0 until items.length()).map { items.getJSONObject(it) }

        assertEquals(0, mapping.getInt("mappedNameCandidates"))
        assertEquals(5, mapping.getInt("compilerRuntimeCandidateCount"))
        assertEquals(0, mapping.getInt("unmappedSymbolCount"))
        assertEquals(100, mapping.getInt("classificationCoveragePercent"))
        assertTrue(decoded.all { it.getString("classification") == "COMPILER_RUNTIME_CANDIDATE" })
        assertTrue(decoded.all { !it.getBoolean("linkedOrRewritten") && !it.getBoolean("codeGenerated") })
        assertTrue(decoded.any { it.getString("reason").contains("does not provide a drop-in libgcc_s.so") })
    }

    @Test fun libgccInstallNameAndSymbolHintsStayExplicitlyUnlinked() {
        val mapping = Providers.classify("/usr/lib/libgcc_s.1.dylib")
        assertEquals(Providers.STATUS_COMPATIBILITY, mapping.getString("status"))
        assertEquals("libgcc_s", mapping.getString("framework"))
        assertTrue(mapping.getString("provider").contains("compiler-rt"))
        assertTrue(mapping.getString("reason").contains("not a loadable-library alias"))
        assertTrue(Providers.forSymbol("___divti3")!!.contains("not linked"))
    }

    @Test fun displayLinkSemanticHintStatesTheObjectiveCAbiIsNotImplemented() {
        val symbol = "_OBJC_CLASS_" + '$' + "_CADisplayLink"
        val imports = JSONArray().put(JSONObject().put("name", symbol))
        val slice = JSONObject().put("imports", imports)
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(slice))))

        val item = AndroidApiMapper.analyze(nodes).getJSONArray("symbols").getJSONObject(0)

        assertEquals("SEMANTIC_REWRITE_CANDIDATE", item.getString("classification"))
        assertTrue(item.getString("targetApi").contains("not the Objective-C CADisplayLink ABI"))
        assertFalse(item.getBoolean("codeGenerated"))
    }

    @Test fun emptyImportSetDoesNotClaimPerfectCoverage() {
        val mapping = AndroidApiMapper.analyze(JSONArray())
        assertEquals(0, mapping.getInt("candidateCoveragePercent"))
        assertEquals(0, mapping.getInt("distinctImportSymbols"))
        assertEquals(0, mapping.getInt("classificationCoveragePercent"))
        assertEquals(0, mapping.getInt("generatedApiImplementationCount"))
        assertEquals(0, mapping.getInt("linkedImplementationCoveragePercent"))
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
        assertEquals(0, mapping.getInt("linkedImplementationCount"))
        assertEquals(0, mapping.getInt("linkedImplementationCoveragePercent"))
        assertEquals(0, mapping.getInt("generatedApiImplementationCount"))
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
