// On-device bounded conversion prover.
//
// The Android importer converts an IPA by itself only when the executable is
// statically proven to be exactly one closed-integer ARM64 routine with no
// imports, dependencies, fixups or runtime metadata. Everything else stays
// UNSUPPORTED. Nothing here executes guest instructions: the decoder only
// verifies membership in the proven subset and copies the source bytes.

#include "trivial.hpp"
#include "macho.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>

namespace radek {
namespace {

std::string base64Encode(const uint8_t *data, size_t size) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const size_t remaining = size - i;
        uint32_t word = uint32_t(data[i]) << 16;
        if (remaining > 1)
            word |= uint32_t(data[i + 1]) << 8;
        if (remaining > 2)
            word |= uint32_t(data[i + 2]);
        out.push_back(alphabet[(word >> 18) & 63]);
        out.push_back(alphabet[(word >> 12) & 63]);
        out.push_back(remaining > 1 ? alphabet[(word >> 6) & 63] : '=');
        out.push_back(remaining > 2 ? alphabet[word & 63] : '=');
    }
    return out;
}

uint64_t number(const Json &json, uint64_t fallback = 0) {
    if (json.kind != Json::Number || json.value.empty())
        return fallback;
    try {
        return std::stoull(json.value);
    } catch (const std::exception &) {
        return fallback;
    }
}

bool isTrue(const Json &json) { return json.kind == Json::Bool && json.value == "true"; }
bool isFalse(const Json &json) { return json.kind == Json::Bool && json.value == "false"; }
const Json *field(const Json &json, const char *name) {
    auto it = json.fields.find(name);
    return it == json.fields.end() ? nullptr : &it->second;
}
const Json *fieldOr(const Json &json, const char *name) {
    static const Json missing;
    auto it = json.fields.find(name);
    return it == json.fields.end() ? &missing : &it->second;
}

// Load commands the bounded converter accepts; identical to the host prover.
const std::set<uint64_t> &allowedLoadCommands() {
    static const std::set<uint64_t> commands = {
        1,     0x19,      2,          0xB,         0x1B,        0x24,       0x25,
        0x2F,  0x30,      0x32,       0x80000028,  0x1D,        0x26,       0x29,
        0x21,  0x2C,      0xC,        0xD,         0x18,        0x20,       0x80000018,
        0x8000001C, 0x8000001F, 0x80000023, 0x22,  0x80000022,  0x80000033,
    };
    return commands;
}

struct TextSection {
    uint64_t offset = 0;
    uint64_t size = 0;
    bool found = false;
};

TextSection findEntryTextSection(const Json &slice, uint64_t entry) {
    TextSection result;
    const Json *segments = field(slice, "segments");
    if (!segments)
        return result;
    for (const auto &segment : segments->items) {
        if (!(number(*fieldOr(segment, "initialProtection")) & 4))
            continue;
        const Json *sections = field(segment, "sections");
        if (!sections)
            continue;
        for (const auto &section : sections->items) {
            const Json *name = field(section, "name");
            if (!name || name->value != "__text")
                continue;
            const uint64_t offset = number(*fieldOr(section, "offset"));
            const uint64_t size = number(*fieldOr(section, "size"));
            if (offset <= entry && entry < offset + size) {
                result.offset = offset;
                result.size = size;
                result.found = true;
                return result;
            }
        }
    }
    return result;
}

std::vector<std::string> cstrings(const Json &slice, const std::vector<uint8_t> &data, uint64_t sliceOffset) {
    std::vector<std::string> results;
    const Json *segments = field(slice, "segments");
    if (!segments)
        return results;
    for (const auto &segment : segments->items) {
        const Json *sections = field(segment, "sections");
        if (!sections)
            continue;
        for (const auto &section : sections->items) {
            const Json *name = field(section, "name");
            if (!name || name->value != "__cstring")
                continue;
            const uint64_t offset = number(*fieldOr(section, "offset"));
            const uint64_t size = number(*fieldOr(section, "size"));
            if (size == 0 || size > 1024 * 1024 || offset > data.size() || size > data.size() - offset)
                continue;
            size_t start = size_t(sliceOffset + offset);
            size_t end = start + size_t(size);
            size_t cursor = start;
            while (cursor < end && results.size() < 64) {
                size_t stop = cursor;
                bool printable = true;
                while (stop < end && data[stop] != 0) {
                    if (data[stop] < 32 || data[stop] > 126)
                        printable = false;
                    ++stop;
                }
                const size_t length = stop - cursor;
                if (printable && length >= 3 && length <= 512)
                    results.emplace_back(reinterpret_cast<const char *>(&data[cursor]), length);
                cursor = stop + 1;
            }
        }
    }
    return results;
}

} // namespace

size_t proveArm64IntegerLeaf(const uint8_t *code, size_t size, std::string *reason) {
    auto fail = [&](const std::string &message) -> size_t {
        if (reason)
            *reason = message;
        return 0;
    };
    if (!code || size == 0)
        return fail("empty entry code");
    bool initialized[16] = {false};
    size_t p = 0;
    size_t count = 0;
    while (p < size) {
        if (++count > 4096)
            return fail("entry point does not terminate within 4096 verified instructions");
        if (p + 4 > size)
            return fail("truncated ARM64 instruction");
        const uint32_t w = uint32_t(code[p]) | (uint32_t(code[p + 1]) << 8) |
                           (uint32_t(code[p + 2]) << 16) | (uint32_t(code[p + 3]) << 24);
        const size_t start = p;
        p += 4;
        int dst = -1;
        int src = -1;
        bool insert = false;
        if (w == 0xD65F03C0u) {
            if (!initialized[0])
                return fail("return value is not initialized");
            if (reason)
                reason->clear();
            return p;
        }
        if ((w & 0xFF800000u) == 0x52800000u || (w & 0xFF800000u) == 0x72800000u) {
            const unsigned shift = ((w >> 21) & 3) * 16;
            if (shift > 16)
                return fail("invalid 32-bit MOV encoding");
            insert = (w & 0xFF800000u) == 0x72800000u;
            dst = int(w & 31);
        } else if ((w & 0xFFC00000u) == 0x11000000u || (w & 0xFFC00000u) == 0x51000000u) {
            dst = int(w & 31);
            src = int((w >> 5) & 31);
        } else if ((w & 0x7FE00000u) == 0x2A000000u && (w & 0xFC00u) == 0 && (w & 0x3E0u) == 0x3E0u) {
            // ORR <W|X>d, WZR/XZR, <W|X>m with no shift: the canonical MOV-register.
            dst = int(w & 31);
            const unsigned source = (w >> 16) & 31;
            if (source != 31)
                src = int(source);
        } else {
            char message[96];
            std::snprintf(message, sizeof(message),
                          "ARM64 instruction 0x%08x at +0x%zx is not in the proven subset", w, start);
            return fail(message);
        }
        if (dst < 0 || dst > 15)
            return fail("special/platform/callee-saved register write");
        if (insert && !initialized[dst])
            return fail("read of uninitialized register");
        if (src >= 0 && !initialized[src])
            return fail("input/stack register dependency is not a closed leaf function");
        initialized[dst] = true;
    }
    return fail("entry point does not terminate within 4096 verified instructions");
}

Json translateTrivial(const std::vector<uint8_t> &data) {
    Json result = Json::object();
    result["status"] = std::string("UNSUPPORTED");
    result["reason"] = std::string("no ARM64 slice was found (on-device conversion targets arm64-v8a)");
    result["architecture"] = std::string("arm64");
    result["targetAbi"] = std::string("arm64-v8a");
    result["machineCode"] = std::string("");
    result["sourceBytes"] = uint64_t(0);
    result["textBytes"] = uint64_t(0);
    result["coveragePercent"] = uint64_t(0);
    result["functionCount"] = uint64_t(0);
    result["strings"] = Json::array();

    Json analysis;
    try {
        analysis = analyze(data);
    } catch (const std::exception &e) {
        result["reason"] = std::string("Mach-O analysis failed: ") + e.what();
        return result;
    }
    const Json *slices = field(analysis, "slices");
    if (!slices)
        return result;

    for (const auto &slice : slices->items) {
        const Json *arch = field(slice, "architecture");
        if (!arch || arch->value != "arm64")
            continue;
        std::string why;
        auto reject = [&]() { result["reason"] = why.empty() ? std::string("unsupported slice") : why; };

        const Json *fileType = field(slice, "fileType");
        if (!fileType || number(*fileType) != 2) {
            why = "entry must be an MH_EXECUTE program";
            reject();
            continue;
        }
        if (isTrue(*fieldOr(slice, "encrypted"))) {
            why = "encrypted/FairPlay binaries are never converted";
            reject();
            continue;
        }
        if (isTrue(*fieldOr(slice, "pacRequired"))) {
            why = "ARM64e pointer authentication is not supported";
            reject();
            continue;
        }
        if (isTrue(*fieldOr(slice, "bigEndian"))) {
            why = "big-endian slice is not supported";
            reject();
            continue;
        }
        const Json *metadata = field(slice, "metadata");
        if (metadata && !metadata->items.empty()) {
            why = "Objective-C/Swift/unwind metadata requires runtime support";
            reject();
            continue;
        }
        if (field(slice, "chainedFixups")) {
            why = "chained fixups require address reconstruction";
            reject();
            continue;
        }
        const Json *bindComplete = field(slice, "bindDecodingComplete");
        if (bindComplete && isFalse(*bindComplete)) {
            why = "dyld bind table did not decode completely";
            reject();
            continue;
        }
        const Json *dependencies = field(slice, "dependencies");
        if (dependencies && !dependencies->items.empty()) {
            why = "linked dependencies are not provided";
            reject();
            continue;
        }
        const Json *imports = field(slice, "imports");
        if (imports && !imports->items.empty()) {
            why = "imported symbols require API replacements";
            reject();
            continue;
        }
        const Json *loadCommands = field(slice, "loadCommands");
        bool commandsAllowed = loadCommands != nullptr;
        if (commandsAllowed) {
            for (const auto &command : loadCommands->items) {
                const Json *id = field(command, "command");
                if (!id || !allowedLoadCommands().count(number(*id))) {
                    commandsAllowed = false;
                    break;
                }
            }
        }
        if (!commandsAllowed) {
            why = "load command requires an unsupported loader semantic";
            reject();
            continue;
        }
        const Json *entryField = field(slice, "entryOffset");
        if (!entryField) {
            why = "only LC_MAIN entry points are converted";
            reject();
            continue;
        }
        const uint64_t entry = number(*entryField);
        if (entry % 4) {
            why = "unaligned entry point";
            reject();
            continue;
        }
        const TextSection section = findEntryTextSection(slice, entry);
        if (!section.found) {
            why = "entry point is not in an executable __text section";
            reject();
            continue;
        }
        if (entry != section.offset) {
            why = "the proven routine must start at the __text section (other functions are not converted)";
            reject();
            continue;
        }
        if (section.size == 0 || section.size > 4096 * 4) {
            why = "__text section is empty or exceeds the bounded converter limit";
            reject();
            continue;
        }
        const uint64_t sliceOffset = number(*fieldOr(slice, "offset"));
        if (sliceOffset > data.size() || entry > data.size() - sliceOffset ||
            section.size > data.size() - sliceOffset - entry) {
            why = "entry code lies outside the slice";
            reject();
            continue;
        }
        const uint8_t *code = data.data() + size_t(sliceOffset + entry);
        std::string proofReason;
        const size_t consumed = proveArm64IntegerLeaf(code, size_t(section.size), &proofReason);
        if (consumed == 0) {
            why = proofReason;
            reject();
            continue;
        }
        if (consumed != section.size) {
            why = "the proven entry routine does not cover the whole executable __text section";
            reject();
            continue;
        }
        result["status"] = std::string("PROVEN");
        result["reason"] = std::string("");
        result["machineCode"] = base64Encode(code, consumed);
        result["sourceBytes"] = uint64_t(consumed);
        result["textBytes"] = uint64_t(section.size);
        result["coveragePercent"] = uint64_t(100);
        result["functionCount"] = uint64_t(1);
        Json strings = Json::array();
        for (const auto &text : cstrings(slice, data, sliceOffset))
            strings.push(Json(text));
        result["strings"] = strings;
        return result;
    }
    return result;
}
} // namespace radek
