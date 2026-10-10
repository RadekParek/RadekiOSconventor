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
            .put(JSONObject().put("name", "_CustomUnmappedSymbol"))
            .put(JSONObject().put("name", "_OBJC_CLASS_" + '$' + "_CADisplayLink"))
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
        val objc = decoded.single { it.getString("sourceSymbol") == "_CustomUnmappedSymbol" }
        val view = decoded.single { it.getString("sourceSymbol").contains("CADisplayLink") }
        assertEquals("libc.so", malloc.getString("targetLibrary"))
        assertFalse(malloc.getBoolean("linkedOrRewritten"))
        assertEquals("UNMAPPED", objc.getString("classification"))
        assertEquals("SEMANTIC_REWRITE_CANDIDATE", view.getString("classification"))
        assertTrue(view.getString("targetApi").contains("Choreographer"))
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
        // Both shim exports were resolved from the shipped libioscompat.so on
        // this device, so the aggregate counts them as real linked
        // implementations. That is an export-level fact: the per-symbol rows
        // below still report linkedOrRewritten=false because no IPA callsite
        // is rewritten.
        assertEquals(2, mapping.getInt("linkedImplementationCount"))
        assertEquals(100, mapping.getInt("linkedImplementationCoveragePercent"))
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
        // ___divdi3 has a separate host-tested compatibility body and its
        // compat-runtime-v1 adapter carries verified semantics (exact 64-bit
        // division covered by the host test suite), so it is promoted to the
        // verified-handler classification; neither record is a direct NDK link.
        assertEquals(4, decoded.count { it.getString("classification") == "COMPILER_RUNTIME_CANDIDATE" })
        assertEquals(0, decoded.count { it.getString("classification") == "IMPLEMENTED_API_REPLACEMENT_AVAILABLE" })
        assertEquals(1, mapping.getInt("implementedApiReplacementCount"))
        val divdi3 = decoded.single { it.getString("sourceSymbol") == "___divdi3" }
        assertEquals("COMPAT_VERIFIED_HANDLER_RESOLVED", divdi3.getString("classification"))
        assertEquals("compiler-runtime.divdi3", divdi3.getString("targetSymbol"))
        assertEquals("GUEST_RUNTIME_PROVIDER_VERIFIED_SEMANTICS", divdi3.getString("resolutionEvidence"))
        assertTrue(divdi3.getJSONObject("evidence").getBoolean("hostTestedImplementation"))
        assertTrue(decoded.all { !it.getBoolean("linkedOrRewritten") && !it.getBoolean("codeGenerated") })
        assertTrue(decoded.any { it.getString("reason").contains("does not provide a drop-in libgcc_s.so") })
    }

    @Test fun legacyCppRuntimeNamesStayCandidatesAndFrameworkSubsetsStayPartial() {
        val gcc = Providers.classify("/usr/lib/libgcc_s.1.dylib")
        assertEquals(Providers.STATUS_CANDIDATE, gcc.getString("status"))
        assertEquals("libgcc_s", gcc.getString("framework"))
        assertTrue(gcc.getString("provider").contains("compiler-rt"))
        assertTrue(gcc.getString("reason").contains("drop-in libgcc_s.so"))

        val stdcxx = Providers.classify("/usr/lib/libstdc++.6.dylib")
        assertEquals(Providers.STATUS_CANDIDATE, stdcxx.getString("status"))
        assertTrue(stdcxx.getString("reason").contains("different C++ ABIs and mangling"))
        assertFalse(stdcxx.getString("provider").contains("GNU C++ runtime served by libc++"))

        for (framework in listOf("CoreFoundation", "QuartzCore")) {
            val mapping = Providers.classify("/System/Library/Frameworks/$framework.framework/$framework")
            assertEquals(Providers.STATUS_COMPATIBILITY, mapping.getString("status"))
            assertTrue(mapping.getString("reason").contains("Partial only"))
            assertFalse(Providers.describe("/System/Library/Frameworks/$framework.framework/$framework").startsWith("PROVIDED"))
        }
        for (framework in listOf("Foundation", "UIKit", "CoreGraphics", "OpenAL", "AudioToolbox")) {
            val mapping = Providers.classify("/System/Library/Frameworks/$framework.framework/$framework")
            assertEquals(Providers.STATUS_CANDIDATE, mapping.getString("status"))
            assertTrue(mapping.getString("reason").contains("No ") || mapping.getString("reason").contains("not adapted"))
        }
        assertTrue(Providers.forSymbol("___divti3")!!.contains("not linked"))
    }

    @Test fun providerEvidenceListsExportsHostTestsStubsAndNoneWithoutLinkClaims() {
        val symbolEvidence = mapOf(
            "_verified" to JSONObject().put("evidence", JSONObject()
                .put("exportsVerifiedOnThisDevice", true).put("hostTestedImplementation", false).put("stubOnly", false)),
            "_hostTested" to JSONObject().put("evidence", JSONObject()
                .put("exportsVerifiedOnThisDevice", false).put("hostTestedImplementation", true).put("stubOnly", false)),
            "_stub" to JSONObject().put("evidence", JSONObject()
                .put("exportsVerifiedOnThisDevice", false).put("hostTestedImplementation", false).put("stubOnly", true)),
        )
        val evidence = Providers.evidenceForImports(
            listOf("_verified", "_hostTested", "_stub", "_unknown"),
            symbolEvidence,
        )
        assertEquals(1, evidence.getInt("exportsVerifiedOnThisDevice"))
        assertEquals(1, evidence.getInt("hostTestedImplementations"))
        assertEquals(1, evidence.getInt("stubOnlyCount"))
        assertEquals(1, evidence.getInt("noneCount"))
        assertFalse(evidence.getBoolean("none"))
        assertFalse(evidence.getBoolean("runtimeBackingClaimed"))
        assertEquals(0, evidence.getInt("linkedGameCallCount"))
        assertEquals(0, evidence.getInt("recompiledBytesLinked"))

        val noEvidence = Providers.classify("/usr/lib/libUnknown.dylib").getJSONObject("evidence")
        assertEquals(Providers.STATUS_NO_EXECUTION_PATH_YET, Providers.classify("/usr/lib/libUnknown.dylib").getString("status"))
        assertTrue(noEvidence.getBoolean("none"))
        assertEquals("none", noEvidence.getJSONArray("evidenceKinds").getString(0))
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

    @Test fun guestRuntimeAdapterCatalogIsSeparateFromStrictNdkAndCompilerRuntimeLinking() {
        val imports = JSONArray()
            .put(JSONObject().put("name", "_OBJC_CLASS_" + '$' + "_NSObject"))
            .put(JSONObject().put("name", "___divdi3"))
            .put(JSONObject().put("name", "__Unwind_SjLj_Register"))
            .put(JSONObject().put("name", "_malloc"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))

        val mapping = AndroidApiMapper.analyze(nodes)
        val symbols = mapping.getJSONArray("symbols")
        val guestRows = (0 until symbols.length())
            .map { symbols.getJSONObject(it) }
            .filter { it.optBoolean("guestRuntimeProviderCatalogued") }

        assertEquals(4, mapping.getInt("distinctImportSymbols"))
        assertEquals(1, mapping.getInt("sameNameNdkProviderCount"))
        assertEquals(25, mapping.getInt("sameNameNdkProviderCoveragePercent"))
        assertEquals(3, mapping.getInt("guestRuntimeProviderCount"))
        assertEquals(3, mapping.getInt("guestRuntimeAdapterCataloguedCount"))
        assertEquals(75, mapping.getInt("guestRuntimeAdapterCatalogInventoryCount"))
        assertEquals("libcompat_runtime_v1.so", mapping.getString("guestRuntimeProviderLibrary"))
        assertEquals("CATALOG_ONLY_NOT_RUNTIME_LINKED", mapping.getString("guestRuntimeProviderCatalogStatus"))
        assertEquals(2, mapping.getInt("compilerRuntimeCandidateCount"))
        assertEquals(2, mapping.getInt("compilerRuntimeGuestProviderCount"))
        assertEquals(4, mapping.getInt("runtimeProviderCount"))
        assertEquals(3, guestRows.size)
        // ___divdi3, the NSObject class object and the SjLj register adapter all
        // carry verified semantics now, so every catalogued guest row resolves.
        assertEquals(3, guestRows.count { it.getString("classification") == "COMPAT_VERIFIED_HANDLER_RESOLVED" })
        assertEquals(0, guestRows.count { it.getString("classification") == "GUEST_RUNTIME_ADAPTER_CATALOGUED" })
        assertTrue(guestRows.all { it.getString("targetLibrary").contains("libcompat_runtime_v1.so") })
        assertTrue(guestRows.all { it.getString("staticRecompilationStrategy").contains("no static Android code-callsite rewrite") })
        assertEquals(0, mapping.getInt("runtimeVerifiedNdkCandidates"))
        // runtimeVerifiedCandidateCount counts direct same-name candidates
        // (here only _malloc); the guest-provider promotions do not change it.
        assertEquals(1, mapping.getInt("runtimeVerifiedCandidateCount"))
        assertEquals(0, mapping.getInt("linkedImplementationCount"))
        val breakdown = mapping.getJSONObject("reviewedMapping").getJSONObject("breakdown")
        assertEquals(0, breakdown.getInt("guestRuntimeAdapterCatalogued"))
        assertEquals(3, breakdown.getInt("concreteCompatImplementation"))
        assertEquals(1, breakdown.getInt("sameNameNdkOrSystemExport"))
        assertEquals(0, breakdown.getInt("compilerRuntimeToolchain"))
        assertEquals(4, breakdown.getInt("kindCountsSum"))
        assertTrue(mapping.getString("measure").contains("actual bind/relocation results"))
    }

    @Test fun reviewedAndroidMappingCoverageCountsEveryMappingKindButNeverImplementation() {
        // One direct same-name export, one compiled compatibility implementation,
        // one reviewed semantic target and one explicit unimplemented stub handler.
        val imports = JSONArray()
            .put(JSONObject().put("name", "_malloc"))
            .put(JSONObject().put("name", "_mach_absolute_time"))
            .put(JSONObject().put("name", "_OBJC_CLASS_" + '$' + "_CADisplayLink"))
            .put(JSONObject().put("name", "_CustomStubOnlySymbol"))
        val slice = JSONObject().put("imports", imports)
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(slice))))

        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveCompatHandler = { symbol ->
                if (symbol == "_CustomStubOnlySymbol") "stubbed:CustomStubOnlySymbol" else null
            },
        )

        assertEquals(4, mapping.getInt("distinctImportSymbols"))
        // Three of four imports have a reviewed mapping of some kind; the explicit
        // stub handler is a resolution target, not an Android mapping.
        assertEquals(3, mapping.getInt("reviewedMappingCount"))
        assertEquals(75, mapping.getInt("reviewedMappingCoveragePercent"))
        val reviewed = mapping.getJSONObject("reviewedMapping")
        assertEquals(3, reviewed.getInt("count"))
        assertEquals(75, reviewed.getInt("percent"))
        assertEquals(1, reviewed.getInt("strictSameNameNdkSubsetCount"))
        assertEquals(25, reviewed.getInt("strictSameNameNdkSubsetPercent"))
        assertTrue(reviewed.getBoolean("kindCountsAreNotInterchangeable"))
        val breakdown = reviewed.getJSONObject("breakdown")
        // Exactly one kind per import, and the kinds sum to the mapping count.
        assertEquals(1, breakdown.getInt("sameNameNdkOrSystemExport"))
        assertEquals(0, breakdown.getInt("compilerRuntimeToolchain"))
        assertEquals(1, breakdown.getInt("concreteCompatImplementation"))
        assertEquals(1, breakdown.getInt("reviewedSemanticApiTarget"))
        assertEquals(3, breakdown.getInt("kindCountsSum"))
        assertEquals(1, breakdown.getInt("explicitStubHandlerOnly"))
        assertEquals(0, breakdown.getInt("unmapped"))
        // No mapping count may be read as rewritten or linked code.
        assertEquals(0, mapping.getInt("linkedImplementationCoveragePercent"))
        assertEquals(0, mapping.getInt("generatedApiImplementationCount"))
        assertTrue(mapping.getString("measure").contains("strict same-name NDK candidate subset"))
    }

    @Test fun emptyImportSetDoesNotClaimPerfectCoverage() {
        val mapping = AndroidApiMapper.analyze(JSONArray())
        assertEquals(0, mapping.getInt("candidateCoveragePercent"))
        assertEquals(0, mapping.getInt("distinctImportSymbols"))
        assertEquals(0, mapping.getInt("classificationCoveragePercent"))
        assertEquals(0, mapping.getInt("generatedApiImplementationCount"))
        assertEquals(0, mapping.getInt("linkedImplementationCoveragePercent"))
        val evidence = mapping.getJSONObject("evidence")
        assertTrue(evidence.getBoolean("none"))
        assertEquals(0, evidence.getInt("observedImportCount"))
        assertEquals(0, evidence.getInt("recompiledBytesLinked"))
        assertEquals(0, evidence.getInt("linkedGameCallCount"))
    }

    @Test fun compatStubHandlersAreRegisteredAndNeverCountedAsVerifiedImplementations() {
        // glDrawArrays and malloc are in the reviewed bionic catalog, so they must
        // classify as direct candidates and never reach the compat resolver; the two
        // OpenAL symbols have no other mapping and are the only resolver inputs.
        val imports = JSONArray()
            .put(JSONObject().put("name", "_glDrawArrays"))
            .put(JSONObject().put("name", "_CustomStubSymbolA"))
            .put(JSONObject().put("name", "_CustomStubSymbolB"))
            .put(JSONObject().put("name", "_malloc"))
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))
        val registered = mutableListOf<String>()

        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveCompatHandler = { symbol ->
                registered += symbol
                when (symbol) {
                    "_CustomStubSymbolA" -> "stubbed:radek_compat_stub_0"
                    "_CustomStubSymbolB" -> "stubbed:radek_compat_stub_1"
                    else -> null
                }
            },
        )

        // The resolver is only consulted for symbols with no other mapping.
        assertEquals(
            "resolver must see exactly the otherwise-unmapped symbols (got $registered)",
            listOf("_CustomStubSymbolA", "_CustomStubSymbolB"),
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
        val stub = items.single { it.getString("sourceSymbol") == "_CustomStubSymbolB" }
        assertEquals("stub classification", "COMPAT_STUB_HANDLER_REGISTERED", stub.getString("classification"))
        assertEquals("stub target library", "libioscompat.so", stub.getString("targetLibrary"))
        assertEquals("stub target symbol", "radek_compat_stub_1", stub.getString("targetSymbol"))
        assertFalse("stub must not claim implementation", stub.getBoolean("implementationCodePresent"))
        assertFalse("stub must not claim linking", stub.getBoolean("linkedOrRewritten"))
        assertTrue("stub reason states non-implementation", stub.getString("reason").contains("does not implement"))
        val direct = items.single { it.getString("sourceSymbol") == "_glDrawArrays" }
        assertEquals("catalog symbol keeps direct classification", "BIONIC_SYMBOL_CANDIDATE", direct.getString("classification"))
        assertEquals("catalog symbol target library", "libGLESv2.so", direct.getString("targetLibrary"))
        assertTrue(stub.getJSONObject("evidence").getBoolean("stubOnly"))
        assertFalse(stub.getJSONObject("evidence").getBoolean("none"))
        assertTrue(direct.getJSONObject("evidence").getBoolean("hostTestedImplementation"))
        assertFalse(direct.getJSONObject("evidence").getBoolean("none"))
        val evidence = mapping.getJSONObject("evidence")
        assertEquals(4, evidence.getInt("observedImportCount"))
        assertEquals(2, evidence.getInt("hostTestedImplementations"))
        assertEquals(2, evidence.getInt("stubOnlyCount"))
        assertEquals(0, evidence.getInt("noneCount"))
        assertFalse(evidence.getBoolean("runtimeBackingClaimed"))
        assertEquals(0, evidence.getInt("recompiledBytesLinked"))
    }

    @Test fun angryBirdsAndGeneralGameImportsReachOneHundredPercentCompatibilityCoverage() {
        val representativeGameImports = listOf(
            "_malloc", "_gettimeofday", "_pthread_mutex_init", "_sinf", "_deflate",
            "_glDrawArrays", "_glBindFramebufferOES", "_glOrthof", "_glTexImage2D",
            "_alSourcePlay", "_alGenBuffers", "_alcOpenDevice",
            "_AudioSessionInitialize", "_AudioQueueNewOutput", "_ExtAudioFileRead",
            "_CGBitmapContextCreate", "_CGContextDrawImage", "_UIApplicationMain",
            "_CFAbsoluteTimeGetCurrent", "_NSLog", "_SCNetworkReachabilityGetFlags",
            "_objc_msgSend", "_OBJC_CLASS_\$_UIView", "_OBJC_CLASS_\$_CAEAGLLayer",
            "_OBJC_CLASS_\$_SKPaymentQueue", "_OBJC_CLASS_\$_GKLocalPlayer",
            "___divdi3", "__Unwind_SjLj_Register", "__ZSt9terminatev",
        )
        val imports = JSONArray()
        representativeGameImports.forEach { imports.put(JSONObject().put("name", it)) }
        val nodes = JSONArray().put(JSONObject().put("analysis", JSONObject()
            .put("slices", JSONArray().put(JSONObject().put("imports", imports)))))

        val mapping = AndroidApiMapper.analyze(
            nodes,
            resolveNdkLibrary = { symbol -> AndroidApiMapper.findBionicLibrary(symbol) },
            resolveApiReplacement = { source -> AndroidApiMapper.compiledCompatibilityProvider(source) },
            runtimeApiLevel = 35,
        )

        val total = representativeGameImports.size
        assertEquals(total, mapping.getInt("distinctImportSymbols"))
        assertEquals(100, mapping.getInt("classificationCoveragePercent"))
        assertEquals(0, mapping.getInt("unmappedSymbolCount"))
        assertEquals(0, mapping.getInt("compatStubHandlerCount"))
        assertEquals(100, mapping.getInt("runtimeVerifiedCandidateCoveragePercent"))
        assertEquals(total, mapping.getInt("implementedApiReplacementCount"))
        assertEquals(total, mapping.getInt("runtimeVerifiedApiReplacementCount"))
        val evidence = mapping.getJSONObject("evidence")
        assertEquals(total, evidence.getInt("exportsVerifiedOnThisDevice"))
        assertEquals(total, evidence.getInt("hostTestedImplementations"))
        assertEquals(0, evidence.getInt("stubOnlyCount"))
        assertEquals(0, evidence.getInt("noneCount"))
    }

    @Test fun compatibilityNeedsListSeparatesNdkCandidatesCatalogEntriesAndUnimplementedStubs() {
        val mapping = JSONObject()
            .put("distinctImportSymbols", 4)
            .put("mappedNameCandidates", 1)
            .put("runtimeVerifiedNdkCandidates", 0)
            .put("guestRuntimeProviderCount", 1)
            .put("compatStubHandlerCount", 1)
            .put("unmappedSymbolCount", 1)
            .put("symbols", JSONArray()
                .put(JSONObject()
                    .put("sourceSymbol", "_malloc")
                    .put("classification", "BIONIC_SYMBOL_CANDIDATE")
                    .put("targetLibrary", "libc.so")
                    .put("targetSymbol", "malloc")
                    .put("reason", "Candidate only; not linked."))
                .put(JSONObject()
                    .put("sourceSymbol", "_OBJC_CLASS_" + '$' + "_UIView")
                    .put("classification", "GUEST_RUNTIME_ADAPTER_CATALOGUED")
                    .put("targetLibrary", "libcompat_runtime_v1.so")
                    .put("targetSymbol", "objc.class.UIView")
                    .put("reason", "Catalog presence is not API completeness."))
                .put(JSONObject()
                    .put("sourceSymbol", "_UnknownApi")
                    .put("classification", "COMPAT_STUB_HANDLER_REGISTERED")
                    .put("targetLibrary", "libioscompat.so")
                    .put("targetSymbol", "radek_compat_stub")
                    .put("reason", "The stub does not implement the API."))
                .put(JSONObject()
                    .put("sourceSymbol", "_ResolvedApi")
                    .put("classification", "IMPLEMENTED_API_REPLACEMENT_AVAILABLE")
                    .put("targetLibrary", "libioscompat.so")
                    .put("targetSymbol", "radek_impl_resolved_api")))

        val output = ApiNeedReport.format(mapping)

        // The filtered report counts needs against all imports and lists each
        // unresolved symbol in its own actionable category.
        assertTrue(output.contains("Unimplemented / unresolved NDK needs: 3 of 4 imports"))
        assertTrue(output.contains("_malloc"))
        assertTrue(output.contains("NDK CANDIDATES"))
        assertTrue(output.contains("_OBJC_CLASS_\$_UIView"))
        assertTrue(output.contains("GUEST ADAPTERS"))
        assertTrue(output.contains("_UnknownApi"))
        assertTrue(output.contains("STUBS"))
        // Fully resolved imports are hidden: no entry, no triage prose.
        assertFalse(output.contains("_ResolvedApi"))
        assertFalse(output.contains("device export verification"))
        assertFalse(output.contains("not a linked-game or complete-API count"))
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
