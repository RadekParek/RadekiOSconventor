package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/** Formats the imported symbols that still need a real Android binding or behavior. */
internal object ApiNeedReport {
    fun format(mapping: JSONObject): String {
        val symbols = mapping.optJSONArray("symbols") ?: JSONArray()
        val total = mapping.optInt("distinctImportSymbols", symbols.length())
        val directCandidates = mapping.optInt("mappedNameCandidates", 0)
        val verifiedDirect = mapping.optInt("runtimeVerifiedNdkCandidates", 0)
        val guestCatalog = mapping.optInt("guestRuntimeProviderCount", 0)
        val stubs = mapping.optInt("compatStubHandlerCount", 0)
        val unmapped = mapping.optInt("unmappedSymbolCount", 0)
        val lines = mutableListOf<String>()
        lines += "Per-IPA imported API needs"
        lines += "Observed imports: $total"
        lines += "Strict same-name NDK/system candidates: $directCandidates; current-device exports verified: $verifiedDirect"
        lines += "Guest-runtime catalog matches: $guestCatalog; explicit compatibility stubs: $stubs; unmapped: $unmapped"
        lines += ""
        lines += "These are triage results, not a linked-game or complete-API count. An export candidate, guest adapter catalog entry, or stub does not rewrite the IPA callsite."
        lines += ""

        val ordered = (0 until symbols.length())
            .mapNotNull { index -> symbols.optJSONObject(index) }
            .sortedBy { it.optString("sourceSymbol") }
        for (item in ordered) {
            val symbol = item.optString("sourceSymbol", "<unnamed import>")
            val classification = item.optString("classification", "UNMAPPED")
            val target = item.optString("targetLibrary")
                .takeIf { it.isNotBlank() }
                ?.let { library ->
                    val targetSymbol = item.optString("targetSymbol")
                    if (targetSymbol.isBlank()) library else "$library:$targetSymbol"
                }
            val state = when (classification) {
                "BIONIC_SYMBOL_CANDIDATE" -> if (item.optBoolean("verifiedOnDevice")) {
                    "Same-name NDK/system export resolved on this device; IPA callsite is still not linked."
                } else {
                    "Same-name NDK/system candidate only; device export verification and binary relinking are not established."
                }
                "COMPILER_RUNTIME_CANDIDATE" ->
                    "Toolchain/compiler-runtime candidate only; no compatible Android link is verified."
                "GUEST_RUNTIME_ADAPTER_CATALOGUED" ->
                    "Guest-runtime catalog entry only; not a same-name NDK export or proof of complete API semantics."
                "IMPLEMENTED_API_REPLACEMENT_AVAILABLE", "COMPAT_VERIFIED_HANDLER_RESOLVED" ->
                    "A concrete compatibility implementation is available, but this IPA callsite is not rewritten or linked."
                "SEMANTIC_REWRITE_CANDIDATE" ->
                    "Android semantic target only; the iOS object/lifecycle behavior still needs a real rewrite."
                "COMPAT_STUB_HANDLER_REGISTERED" ->
                    "UNIMPLEMENTED: a stub records the call and returns a safe default; it is not an API body."
                else -> "UNRESOLVED: no reviewed provider or concrete API implementation is available."
            }
            val reason = item.optString("reason").takeIf { it.isNotBlank() }
            lines += "$symbol"
            lines += "  Classification: $classification"
            if (target != null) lines += "  Candidate/provider: $target"
            lines += "  Need: $state"
            if (reason != null) lines += "  Detail: $reason"
        }
        if (ordered.isEmpty()) lines += "No import rows were returned by the mapper."
        return lines.joinToString("\n")
    }
}
