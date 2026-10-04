#include "macho.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_set>
namespace radek {
namespace {
constexpr size_t kBaseAnalysisBudget = 40000000;
constexpr size_t kMaximumAnalysisBudget = 800000000;
constexpr size_t kMaximumCompactImports = 100000;

size_t analysisBudgetFor(size_t inputBytes) {
    // Large symbol/fixup tables naturally need more work than a tiny fixture.
    // Scale the guard with the actual input size, while retaining a hard ceiling
    // so a malformed image cannot force unbounded CPU or JSON allocation.
    const size_t maximumExtra = kMaximumAnalysisBudget - kBaseAnalysisBudget;
    const size_t extra = inputBytes > maximumExtra / 4 ? maximumExtra : inputBytes * 4;
    return kBaseAnalysisBudget + extra;
}

struct Reader {
    const std::vector<uint8_t> &b;
    size_t base, size;
    bool be = false;
    std::shared_ptr<size_t> budget = std::make_shared<size_t>(kBaseAnalysisBudget);
    void consume(size_t n) const {
        if (n > *budget)
            throw std::runtime_error("Mach-O analysis complexity limit");
        *budget -= n;
    }
    void check(uint64_t p, uint64_t n) const {
        if (p > size || n > size - p)
            throw std::runtime_error("Mach-O range outside slice");
    }
    uint64_t u(size_t p, size_t n) const {
        consume(8);
        check(p, n);
        uint64_t v = 0;
        for (size_t i = 0; i < n; i++)
            v |= uint64_t(b[base + p + i]) << (8 * (be ? n - i - 1 : i));
        return v;
    }
    std::string str(size_t p, size_t end) const {
        check(p, 0);
        check(end, 0);
        if (p > end)
            throw std::runtime_error("invalid string range");
        std::string s;
        while (p < end) {
            consume(1);
            auto c = b[base + p++];
            if (!c)
                return s;
            if (s.size() >= 4096)
                throw std::runtime_error("Mach-O string length limit");
            s += char(c);
        }
        throw std::runtime_error("unterminated Mach-O string");
    }
    std::string fixed(size_t p, size_t n) const {
        check(p, n);
        std::string s;
        for (size_t i = 0; i < n && b[base + p + i]; i++)
            s += char(b[base + p + i]);
        return s;
    }
    int64_t sleb(size_t &p, size_t end) const {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (p >= end)
                throw std::runtime_error("truncated SLEB128");
            uint8_t c = uint8_t(u(p++, 1));
            auto bits = c & 127;
            if (shift == 63 && bits != 0 && bits != 127)
                throw std::runtime_error("SLEB128 overflow");
            value |= uint64_t(bits) << shift;
            if (!(c & 128)) {
                if ((c & 64) && shift < 57)
                    value |= (~uint64_t(0)) << (shift + 7);
                return int64_t(value);
            }
        }
        throw std::runtime_error("SLEB128 overflow");
    }
    uint64_t leb(size_t &p, size_t end) const {
        uint64_t n = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (p >= end)
                throw std::runtime_error("truncated ULEB128");
            uint8_t c = uint8_t(u(p++, 1));
            if (shift == 63 && (c & 0x7e))
                throw std::runtime_error("ULEB128 overflow");
            n |= uint64_t(c & 127) << shift;
            if (!(c & 128))
                return n;
        }
        throw std::runtime_error("ULEB128 overflow");
    }
};
Json object() {
    return Json::object();
}
Json array() {
    return Json::array();
}
Json signedNumber(int64_t value) {
    Json result;
    result.kind = Json::Number;
    result.value = std::to_string(value);
    return result;
}
// CPU_SUBTYPE_ARM_* from mach/machine.h. Old 32-bit iOS games are usually
// armv6 (iPhone OS 2-5) or armv7; every subtype is named so the report and the
// ARM32 -> ARM64 lowering can select a slice instead of discarding it.
std::string arch(uint64_t c, uint64_t s) {
    s &= 0xffffff;
    if (c == 0x100000c)
        return s == 2 ? "arm64e" : "arm64";
    if (c == 12) {
        switch (s) {
        case 5:
            return "armv4t";
        case 6:
            return "armv6";
        case 7:
            return "armv5tej";
        case 9:
            return "armv7";
        case 10:
            return "armv7f";
        case 11:
            return "armv7s";
        case 12:
            return "armv7k";
        case 13:
            return "armv8-32";
        case 14:
            return "armv6m";
        case 15:
            return "armv7m";
        case 16:
            return "armv7em";
        default:
            return "arm32-unknown";
        }
    }
    return "unsupported";
}
Json relocations(Reader &r, size_t off, size_t n, bool includeDetails) {
    r.check(off, uint64_t(n) * 8);
    if (!includeDetails)
        return array();
    if (n > 1000000)
        throw std::runtime_error("too many relocations for detailed analysis");
    Json a = array();
    for (size_t i = 0; i < n; i++) {
        uint64_t x = r.u(off + i * 8, 4), y = r.u(off + i * 8 + 4, 4);
        Json j = object();
        j["address"] = x;
        j["raw"] = y;
        bool scattered = (x & 0x80000000) != 0;
        j["scattered"] = scattered;
        if (scattered) {
            j["type"] = (x >> 24) & 15;
            j["pcRelative"] = bool(x & (1 << 30));
            j["length"] = (x >> 28) & 3;
            j["value"] = y;
        } else {
            j["symbolIndex"] = y & 0xffffff;
            j["pcRelative"] = bool(y & (1 << 24));
            j["length"] = (y >> 25) & 3;
            j["external"] = bool(y & (1 << 27));
            j["type"] = (y >> 28) & 15;
        }
        a.push(j);
    }
    return a;
}
Json thin(Reader r, bool includeSymbolDetails) {
    auto magic = r.u(0, 4);
    bool wide = magic == 0xfeedfacf || magic == 0xcffaedfe;
    if (magic != 0xfeedface && magic != 0xfeedfacf && magic != 0xcefaedfe && magic != 0xcffaedfe)
        throw std::runtime_error("not a Mach-O slice");
    r.be = magic == 0xcefaedfe || magic == 0xcffaedfe;
    size_t hdr = wide ? 32 : 28;
    r.check(0, hdr);
    auto cpu = r.u(4, 4), sub = r.u(8, 4), nc = r.u(16, 4), sz = r.u(20, 4);
    if ((cpu == 12 && wide) || (cpu == 0x100000c && !wide))
        throw std::runtime_error("CPU/header width mismatch");
    if (nc > 65536 || sz < nc * 8)
        throw std::runtime_error("invalid load command count");
    r.check(hdr, sz);
    Json j = object();
    j["architecture"] = arch(cpu, sub);
    j["cpuType"] = cpu;
    j["cpuSubtype"] = sub;
    j["fileType"] = r.u(12, 4);
    j["flags"] = r.u(24, 4);
    j["offset"] = uint64_t(r.base);
    j["size"] = uint64_t(r.size);
    j["bits"] = uint64_t(wide ? 64 : 32);
    j["bigEndian"] = r.be;
    j["encrypted"] = false;
    j["pacRequired"] = arch(cpu, sub) == "arm64e";
    j["segments"] = array();
    j["dependencies"] = array();
    j["symbols"] = array();
    j["symbolCount"] = uint64_t(0);
    j["imports"] = array();
    j["exports"] = array();
    j["loadCommands"] = array();
    j["metadata"] = array();
    j["linkedit"] = array();
    j["fixupAnomalies"] = array();
    j["fixupStreams"] = array();
    if (!includeSymbolDetails)
        j["importsTruncated"] = false;
    std::unordered_set<std::string> compactImportNames;
    auto appendImport = [&](Json import) {
        if (includeSymbolDetails) {
            j["imports"].push(std::move(import));
            return;
        }
        const auto found = import.fields.find("name");
        if (found == import.fields.end() || compactImportNames.count(found->second.value))
            return;
        if (compactImportNames.size() >= kMaximumCompactImports) {
            j["importsTruncated"] = true;
            return;
        }
        compactImportNames.insert(found->second.value);
        Json compact = object();
        compact["name"] = found->second;
        j["imports"].push(std::move(compact));
    };
    size_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    bool symSeen = false;
    size_t exportOff = 0, exportSize = 0;
    uint64_t textVmAddress = 0;
    bool textSeen = false;
    struct Bind {
        size_t off, size;
        std::string kind;
    };
    std::vector<Bind> binds;
    for (size_t p = hdr, k = 0; k < nc; k++) {
        auto cmd = r.u(p, 4), len = r.u(p + 4, 4);
        if (len < 8 || len % 4 || len > hdr + sz - p)
            throw std::runtime_error("invalid load command size");
        size_t end = p + len;
        auto need = [&](size_t n) {
            if (len < n)
                throw std::runtime_error("short load command");
        };
        Json lc = object();
        lc["command"] = cmd;
        lc["size"] = len;
        j["loadCommands"].push(lc);
        if (cmd == 1 || cmd == 0x19) {
            bool s64 = cmd == 0x19;
            size_t h = s64 ? 72 : 56, ss = s64 ? 80 : 68;
            need(h);
            size_t ns = r.u(p + (s64 ? 64 : 48), 4);
            if (ns > (len - h) / ss)
                throw std::runtime_error("section table exceeds segment command");
            Json s = object();
            s["name"] = r.fixed(p + 8, 16);
            s["vmAddress"] = r.u(p + 24, s64 ? 8 : 4);
            if (!textSeen && s["name"].value == "__TEXT") {
                textSeen = true;
                textVmAddress = r.u(p + 24, s64 ? 8 : 4);
            }
            s["vmSize"] = r.u(p + (s64 ? 32 : 28), s64 ? 8 : 4);
            auto fo = r.u(p + (s64 ? 40 : 32), s64 ? 8 : 4), fs = r.u(p + (s64 ? 48 : 36), s64 ? 8 : 4);
            r.check(fo, fs);
            if (fs > r.u(p + (s64 ? 32 : 28), s64 ? 8 : 4))
                throw std::runtime_error("segment file size exceeds VM range");
            s["fileOffset"] = fo;
            s["fileSize"] = fs;
            s["maxProtection"] = r.u(p + (s64 ? 56 : 40), 4);
            s["initialProtection"] = r.u(p + (s64 ? 60 : 44), 4);
            s["sections"] = array();
            for (size_t z = 0; z < ns; z++) {
                size_t q = p + h + z * ss;
                Json sec = object();
                auto name = r.fixed(q, 16);
                sec["name"] = name;
                sec["segment"] = r.fixed(q + 16, 16);
                sec["address"] = r.u(q + 32, s64 ? 8 : 4);
                auto size = r.u(q + (s64 ? 40 : 36), s64 ? 8 : 4), off = r.u(q + (s64 ? 48 : 40), 4),
                     flags = r.u(q + (s64 ? 64 : 56), 4);
                auto type = flags & 255;
                auto va = r.u(q + 32, s64 ? 8 : 4), segva = r.u(p + 24, s64 ? 8 : 4),
                     vmsize = r.u(p + (s64 ? 32 : 28), s64 ? 8 : 4);
                if (va < segva || va - segva > vmsize || size > vmsize - (va - segva))
                    throw std::runtime_error("section outside segment VM range");
                if (type != 1 && type != 12 && type != 18) {
                    r.check(off, size);
                    if (size && (off < fo || off - fo > fs || size > fs - (off - fo)))
                        throw std::runtime_error("section outside segment file range");
                }
                sec["size"] = size;
                sec["offset"] = off;
                sec["alignment"] = r.u(q + (s64 ? 52 : 44), 4);
                sec["flags"] = flags;
                sec["reserved1"] = r.u(q + (s64 ? 68 : 60), 4);
                sec["reserved2"] = r.u(q + (s64 ? 72 : 64), 4);
                const auto relocationOffset = r.u(q + (s64 ? 56 : 48), 4);
                const auto relocationCount = r.u(q + (s64 ? 60 : 52), 4);
                sec["relocationCount"] = relocationCount;
                sec["relocations"] = relocations(r, relocationOffset, relocationCount, includeSymbolDetails);
                s["sections"].push(sec);
                if (name.find("objc") != std::string::npos || name.find("swift") != std::string::npos ||
                    name == "__unwind_info" || name == "__eh_frame" || type == 9 || type == 10 ||
                    type == 21) {
                    Json m = object();
                    m["section"] = name;
                    m["offset"] = off;
                    m["size"] = size;
                    m["status"] = "metadata-only";
                    j["metadata"].push(m);
                }
            }
            j["segments"].push(s);
        } else if (cmd == 2) {
            need(24);
            if (symSeen)
                throw std::runtime_error("duplicate symbol table");
            symSeen = true;
            symoff = r.u(p + 8, 4);
            nsyms = r.u(p + 12, 4);
            stroff = r.u(p + 16, 4);
            strsize = r.u(p + 20, 4);
            const size_t symbolEntrySize = wide ? 16 : 12;
            // nsyms is bounded by the actual slice's symbol-table byte range and
            // the shared analysis-work budget below, not an arbitrary 100,000
            // symbol policy cap. Keep the multiplication in uint64_t before the
            // range check (nsyms is read from a 32-bit Mach-O field).
            r.check(symoff, uint64_t(nsyms) * symbolEntrySize);
            r.check(stroff, strsize);
            j["symbolCount"] = uint64_t(nsyms);
            Json table = object();
            table["offset"] = uint64_t(symoff);
            table["count"] = uint64_t(nsyms);
            table["entrySize"] = uint64_t(wide ? 16 : 12);
            table["stringOffset"] = uint64_t(stroff);
            table["stringSize"] = uint64_t(strsize);
            j["symbolTable"] = table;
        } else if (cmd == 0xb) {
            need(80);
            Json d = object();
            const char *names[] = {"localIndex",
                                   "localCount",
                                   "externalIndex",
                                   "externalCount",
                                   "undefinedIndex",
                                   "undefinedCount",
                                   "tocOffset",
                                   "tocCount",
                                   "moduleOffset",
                                   "moduleCount",
                                   "referenceOffset",
                                   "referenceCount",
                                   "indirectOffset",
                                   "indirectCount",
                                   "externalRelocationOffset",
                                   "externalRelocationCount",
                                   "localRelocationOffset",
                                   "localRelocationCount"};
            for (size_t x = 0; x < 18; x++)
                d[names[x]] = r.u(p + 8 + x * 4, 4);
            const auto indirectOffset = r.u(p + 56, 4), indirectCount = r.u(p + 60, 4);
            r.check(indirectOffset, indirectCount * 4);
            const auto externalRelocationOffset = r.u(p + 64, 4), externalRelocationCount = r.u(p + 68, 4);
            const auto localRelocationOffset = r.u(p + 72, 4), localRelocationCount = r.u(p + 76, 4);
            if (includeSymbolDetails) {
                d["externalRelocations"] = relocations(r, externalRelocationOffset, externalRelocationCount, true);
                d["localRelocations"] = relocations(r, localRelocationOffset, localRelocationCount, true);
            } else {
                r.check(externalRelocationOffset, externalRelocationCount * 8);
                r.check(localRelocationOffset, localRelocationCount * 8);
            }
            // The indirect symbol table maps __la_symbol_ptr/__nl_symbol_ptr/__got slots to
            // symtab entries. Exporting it lets the host recovery layer resolve stub targets.
            // Device analysis only needs import names and avoids retaining this potentially
            // large implementation detail in memory.
            if (includeSymbolDetails) {
                r.consume(indirectCount);
                Json indirect = array();
                for (size_t x = 0; x < indirectCount; x++)
                    indirect.push(r.u(indirectOffset + x * 4, 4));
                d["indirectSymbols"] = indirect;
                j["dynamicSymbols"] = d;
            }
        } else if (cmd == 0xc || cmd == 0x18 || cmd == 0x80000018 || cmd == 0x8000001f || cmd == 0x80000023 ||
                   cmd == 0x20 || cmd == 0xd) {
            need(24);
            auto no = r.u(p + 8, 4);
            if (no < 24 || no >= len)
                throw std::runtime_error("invalid dylib name offset");
            Json d = object();
            d["path"] = r.str(p + no, end);
            d["command"] = cmd;
            d["currentVersion"] = r.u(p + 16, 4);
            d["compatibilityVersion"] = r.u(p + 20, 4);
            if (cmd == 0xd)
                j["dylibIdentity"] = d;
            else
                j["dependencies"].push(d);
        } else if (cmd == 0x8000001c) {
            need(12);
            auto no = r.u(p + 8, 4);
            if (no < 12 || no >= len)
                throw std::runtime_error("invalid rpath");
            if (!j.fields.count("rpaths"))
                j["rpaths"] = array();
            j["rpaths"].push(r.str(p + no, end));
        } else if (cmd == 0x21 || cmd == 0x2c) {
            need(cmd == 0x21 ? 20 : 24);
            auto off = r.u(p + 8, 4), size = r.u(p + 12, 4);
            r.check(off, size);
            Json e = object();
            e["offset"] = off;
            e["size"] = size;
            e["cryptId"] = r.u(p + 16, 4);
            j["encryption"] = e;
            if (r.u(p + 16, 4))
                j["encrypted"] = true;
        } else if (cmd == 0x80000028) {
            need(24);
            auto off = r.u(p + 8, 8);
            r.check(off, 1);
            j["entryOffset"] = off;
            j["stackSize"] = r.u(p + 16, 8);
        } else if (cmd == 0x1b) {
            need(24);
            char hex[33] = {0};
            for (size_t x = 0; x < 16; x++)
                std::snprintf(hex + x * 2, 3, "%02x", uint8_t(r.u(p + 8 + x, 1)));
            j["uuid"] = std::string(hex);
        } else if (cmd == 0x32) {
            need(24);
            j["buildVersion"] = Json::object();
            j["buildVersion"]["platform"] = r.u(p + 8, 4);
            j["buildVersion"]["minOS"] = r.u(p + 12, 4);
            j["buildVersion"]["sdk"] = r.u(p + 16, 4);
            j["buildVersion"]["toolCount"] = r.u(p + 20, 4);
        } else if (cmd == 0x24 || cmd == 0x25 || cmd == 0x2f || cmd == 0x30) {
            need(16);
            j["minVersion"] = Json::object();
            j["minVersion"]["command"] = cmd;
            j["minVersion"]["version"] = r.u(p + 8, 4);
            j["minVersion"]["sdk"] = r.u(p + 12, 4);
        } else if (cmd == 4 || cmd == 5) {
            need(16);
            Json t = object();
            t["flavor"] = r.u(p + 8, 4);
            t["count"] = r.u(p + 12, 4);
            if (r.u(p + 12, 4) > (len - 16) / 4)
                throw std::runtime_error("thread state truncated");
            auto flavor = r.u(p + 8, 4), count = r.u(p + 12, 4);
            if (cpu == 0x100000c && flavor == 6 && count >= 68) {
                t["programCounter"] = r.u(p + 16 + 256, 8);
                t["stackPointer"] = r.u(p + 16 + 248, 8);
                t["linkRegister"] = r.u(p + 16 + 240, 8);
                t["cpsr"] = r.u(p + 16 + 264, 4);
            } else if (cpu == 12 && flavor == 1 && count >= 17) {
                t["programCounter"] = r.u(p + 16 + 60, 4);
                t["stackPointer"] = r.u(p + 16 + 52, 4);
                t["linkRegister"] = r.u(p + 16 + 56, 4);
                t["cpsr"] = r.u(p + 16 + 64, 4);
            }
            j["threadEntry"] = t;
        } else if (cmd == 0x22 || cmd == 0x80000022) {
            need(48);
            const char *names[] = {"rebase", "bind", "weakBind", "lazyBind", "exportTrie"};
            for (size_t x = 0; x < 5; x++) {
                size_t off = r.u(p + 8 + x * 8, 4), n = r.u(p + 12 + x * 8, 4);
                r.check(off, n);
                Json l = object();
                l["kind"] = names[x];
                l["offset"] = uint64_t(off);
                l["size"] = uint64_t(n);
                j["linkedit"].push(l);
                if (x == 4) {
                    exportOff = off;
                    exportSize = n;
                } else if (x > 0 && n)
                    binds.push_back({off, n, names[x]});
            }
        } else if (cmd == 0x1d || cmd == 0x1e || cmd == 0x26 || cmd == 0x29 || cmd == 0x80000033 ||
                   cmd == 0x80000034) {
            need(16);
            size_t off = r.u(p + 8, 4), n = r.u(p + 12, 4);
            r.check(off, n);
            Json l = object();
            l["command"] = cmd;
            l["offset"] = uint64_t(off);
            l["size"] = uint64_t(n);
            j["linkedit"].push(l);
            if (cmd == 0x80000033) {
                exportOff = off;
                exportSize = n;
            }
            if (cmd == 0x80000034 && n) {
                if (n < 28)
                    throw std::runtime_error("short chained fixups");
                Json f = object();
                f["version"] = r.u(off, 4);
                f["startsOffset"] = r.u(off + 4, 4);
                auto io = r.u(off + 8, 4), so = r.u(off + 12, 4), count = r.u(off + 16, 4),
                     format = r.u(off + 20, 4);
                f["importsFormat"] = format;
                f["symbolsFormat"] = r.u(off + 24, 4);
                f["importsCount"] = count;
                f["imports"] = array();
                if (io > n || so > n || r.u(off + 4, 4) >= n)
                    throw std::runtime_error("invalid chained fixup offsets");
                size_t stride = format == 1 ? 4 : format == 2 ? 8 : format == 3 ? 16 : 0;
                if (!stride || count > (n - io) / stride)
                    throw std::runtime_error("invalid chained import format/table");
                if (r.u(off + 24, 4) == 0) {
                    for (size_t z = 0; z < count; z++) {
                        uint64_t word = r.u(off + io + z * stride, format == 3 ? 8 : 4),
                                 no = format == 3 ? word >> 32 : word >> 9;
                        if (no >= n - so)
                            throw std::runtime_error("invalid chained symbol");
                        Json im = object();
                        im["name"] = r.str(off + so + no, off + n);
                        im["ordinal"] = word & (format == 3 ? 65535 : 255);
                        im["weak"] = bool(word & (format == 3 ? 65536 : 256));
                        if (includeSymbolDetails)
                            f["imports"].push(im);
                        appendImport(im);
                    }
                }
                f["pointerTraversal"] = "not-implemented";
                f["symbolsDecoding"] = r.u(off + 24, 4) == 0 ? "uncompressed" : "unsupported-compression";
                auto starts = r.u(off + 4, 4);
                if (starts < 28 || starts > n || n - starts < 4)
                    throw std::runtime_error("invalid chained starts header");
                auto segmentCount = r.u(off + starts, 4);
                if (segmentCount > (n - starts - 4) / 4 || segmentCount > 65536)
                    throw std::runtime_error("invalid chained starts count");
                if (includeSymbolDetails)
                    f["segments"] = array();
                else
                    f["segmentCount"] = segmentCount;
                for (uint64_t index = 0; index < segmentCount; index++) {
                    auto relative = r.u(off + starts + 4 + index * 4, 4);
                    if (!relative)
                        continue;
                    if (relative > n - starts || n - starts - relative < 22)
                        throw std::runtime_error("invalid chained starts segment");
                    size_t at = off + starts + relative;
                    auto length = r.u(at, 4), pages = r.u(at + 20, 2);
                    if (length < 22 || length > n - starts - relative || pages > (length - 22) / 2)
                        throw std::runtime_error("invalid chained starts pages");
                    if (includeSymbolDetails) {
                        Json segment = object();
                        segment["index"] = index;
                        segment["pageSize"] = r.u(at + 4, 2);
                        segment["pointerFormat"] = r.u(at + 6, 2);
                        segment["segmentOffset"] = r.u(at + 8, 8);
                        segment["maxValidPointer"] = r.u(at + 16, 4);
                        segment["pageStarts"] = array();
                        for (uint64_t page = 0; page < pages; page++)
                            segment["pageStarts"].push(r.u(at + 22 + page * 2, 2));
                        f["segments"].push(segment);
                    }
                }
                j["chainedFixups"] = f;
            }
            if (includeSymbolDetails && cmd == 0x26 && n) {
                // LC_FUNCTION_STARTS: ULEB128 deltas relative to the __TEXT segment address.
                size_t at = off, stop = off + n;
                uint64_t address = textVmAddress;
                Json starts = array();
                size_t count = 0;
                while (at < stop) {
                    uint64_t delta = r.leb(at, stop);
                    if (delta > std::numeric_limits<uint64_t>::max() - address)
                        throw std::runtime_error("function start overflow");
                    address += delta;
                    if (++count > 2000000)
                        throw std::runtime_error("function-start table exceeds the 2,000,000-entry safety limit");
                    starts.push(address);
                }
                Json fs = object();
                fs["baseAddress"] = uint64_t(textVmAddress);
                fs["count"] = uint64_t(count);
                fs["addresses"] = starts;
                j["functionStarts"] = fs;
            }
            if (cmd == 0x1d && n) {
                Reader cr = r;
                cr.be = true;
                if (n < 12 || cr.u(off, 4) != 0xfade0cc0)
                    throw std::runtime_error("invalid signature superblob");
                size_t total = cr.u(off + 4, 4), count = cr.u(off + 8, 4);
                if (total > n || total < 12 || count > (total - 12) / 8)
                    throw std::runtime_error("invalid signature index");
                Json cs = object();
                cs["cryptographicVerification"] = "not-performed";
                cs["blobs"] = array();
                for (size_t x = 0; x < count; x++) {
                    auto bo = cr.u(off + 16 + x * 8, 4);
                    if (bo > total || total - bo < 8)
                        throw std::runtime_error("invalid signature blob");
                    auto bl = cr.u(off + bo + 4, 4);
                    if (bl < 8 || bl > total - bo)
                        throw std::runtime_error("invalid signature length");
                    Json b = object();
                    auto bm = cr.u(off + bo, 4);
                    b["magic"] = bm;
                    b["length"] = bl;
                    b["slot"] = cr.u(off + 12 + x * 8, 4);
                    if (bm == 0xfade0c02) {
                        if (bl < 44)
                            throw std::runtime_error("short code directory");
                        b["version"] = cr.u(off + bo + 8, 4);
                        b["flags"] = cr.u(off + bo + 12, 4);
                        auto id = cr.u(off + bo + 20, 4);
                        if (id >= bl)
                            throw std::runtime_error("invalid signature identifier");
                        b["identifier"] = cr.str(off + bo + id, off + bo + bl);
                    }
                    cs["blobs"].push(b);
                }
                j["codeSignature"] = cs;
            }
        }
        p = end;
        if (k + 1 == nc && p != hdr + sz)
            throw std::runtime_error("load command byte count mismatch");
    }
    for (size_t i = 0; i < nsyms; i++) {
        size_t p = symoff + i * (wide ? 16 : 12);
        auto index = r.u(p, 4), type = r.u(p + 4, 1);
        if (index >= strsize)
            throw std::runtime_error("invalid symbol string offset");
        const bool externalNonStab = !(type & 0xe0) && (type & 1);
        const bool undefined = (type & 0xe) == 0;
        if (includeSymbolDetails || (externalNonStab && undefined)) {
            Json s = object();
            s["name"] = r.str(stroff + index, stroff + strsize);
            if (includeSymbolDetails) {
                s["type"] = type;
                s["section"] = r.u(p + 5, 1);
                s["description"] = r.u(p + 6, 2);
                s["value"] = r.u(p + 8, wide ? 8 : 4);
                j["symbols"].push(s);
                if (externalNonStab && undefined)
                    appendImport(s);
                else if (externalNonStab && (type & 0xe) == 0xe)
                    j["exports"].push(s);
            } else {
                // The on-device mapper needs imported names, not every local and
                // exported nlist record. Keep its inventory compact and deduplicated.
                Json import = object();
                import["name"] = s["name"];
                appendImport(std::move(import));
            }
        }
    }
    // A malformed, oversized or only partially understood dyld opcode stream is a
    // fact about the input, not a reason to discard the whole analysis. Every
    // stream is decoded independently: what is understood is reported, the first
    // problem stops only that stream, and the reason is recorded so the report
    // stays honest instead of surfacing as an opaque IOException.
    j["bindDecodingComplete"] = true;
    j["bindDiagnostics"] = array();
    for (auto &b : binds) {
        size_t p = b.off, end = p + b.size;
        uint64_t decoded = 0, threaded = 0;
        auto decode = [&]() {
            std::string symbol;
            int64_t ordinal = 0, addend = 0;
            uint64_t seg = 0, address = 0, type = 1, flags = 0;
            bool segmentSet = false;
            const uint64_t pointerSize = wide ? 8 : 4;
            auto segmentSize = [&]() -> uint64_t {
                if (!segmentSet || seg >= j["segments"].items.size())
                    throw std::runtime_error("bind uses an invalid segment index");
                return std::stoull(j["segments"].items[seg].fields["vmSize"].value);
            };
            auto advance = [&](uint64_t amount) {
                auto vmSize = segmentSize();
                // Bind addresses are segment-relative offsets. Check the segment bound
                // before adding so malformed ULEBs cannot wrap the 64-bit cursor.
                if (address > vmSize || amount > vmSize - address)
                    throw std::runtime_error("dyld bind address outside segment");
                address += amount;
            };
            auto emit = [&]() {
                r.consume(symbol.size() + 64);
                if (symbol.empty())
                    throw std::runtime_error("bind without symbol");
                auto vmSize = segmentSize();
                uint64_t width = type == 1 ? pointerSize : 4;
                if (address > vmSize || width > vmSize - address)
                    throw std::runtime_error("dyld bind outside segment");
                Json im = object();
                im["name"] = symbol;
                im["ordinal"] = signedNumber(ordinal);
                im["addend"] = signedNumber(addend);
                im["segment"] = seg;
                im["offset"] = address;
                im["type"] = type;
                im["flags"] = flags;
                im["stream"] = b.kind;
                appendImport(std::move(im));
                decoded++;
            };
            while (p < end) {
                auto byte = r.u(p++, 1), op = byte & 0xf0, imm = byte & 15;
                switch (op) {
                case 0:
                    if (b.kind != "lazyBind")
                        p = end;
                    else {
                        symbol.clear();
                        ordinal = addend = 0;
                        seg = address = flags = 0;
                        segmentSet = false;
                        type = 1;
                    }
                    break;
                case 0x10:
                    ordinal = int64_t(imm);
                    break;
                case 0x20: {
                    auto value = r.leb(p, end);
                    if (value > uint64_t(std::numeric_limits<int64_t>::max()))
                        throw std::runtime_error("bind ordinal overflow");
                    ordinal = int64_t(value);
                    break;
                }
                case 0x30:
                    ordinal = imm ? int8_t(imm | 0xf0) : 0;
                    break;
                case 0x40:
                    symbol = r.str(p, end);
                    p += symbol.size() + 1;
                    flags = imm;
                    break;
                case 0x50:
                    if (imm < 1 || imm > 3)
                        throw std::runtime_error("invalid bind type");
                    type = imm;
                    break;
                case 0x60:
                    addend = r.sleb(p, end);
                    break;
                case 0x70: {
                    seg = imm;
                    address = r.leb(p, end);
                    segmentSet = true;
                    if (address > segmentSize())
                        throw std::runtime_error("dyld bind address outside segment");
                    break;
                }
                case 0x80:
                    advance(r.leb(p, end));
                    break;
                case 0x90:
                    emit();
                    advance(pointerSize);
                    break;
                case 0xa0:
                    emit();
                    advance(r.leb(p, end));
                    advance(pointerSize);
                    break;
                case 0xb0:
                    emit();
                    advance((imm + 1) * pointerSize);
                    break;
                case 0xc0: {
                    auto count = r.leb(p, end), skip = r.leb(p, end);
                    // Each emitted bind is checked against the segment and shared
                    // analysis-work budget; the stream length is not capped by an
                    // arbitrary number of binds.
                    for (uint64_t n = 0; n < count; n++) {
                        emit();
                        advance(skip);
                        advance(pointerSize);
                    }
                    break;
                }
                case 0xd0:
                    // Chained-fixup images keep the real pointers in
                    // LC_DYLD_CHAINED_FIXUPS; this stream only carries ordinals.
                    // Record them instead of discarding the whole analysis.
                    if (imm == 0) {
                        auto value = r.leb(p, end);
                        if (value > uint64_t(std::numeric_limits<int64_t>::max()))
                            throw std::runtime_error("bind ordinal overflow");
                        ordinal = int64_t(value);
                        threaded++;
                    } else if (imm != 1)
                        throw std::runtime_error("invalid threaded bind sub-opcode");
                    break;
                default:
                    throw std::runtime_error("invalid bind opcode");
                }
            }
        };
        Json stream = object();
        stream["kind"] = b.kind;
        stream["bytes"] = uint64_t(b.size);
        try {
            decode();
            stream["status"] = "decoded";
        } catch (const std::exception &e) {
            // Keep load commands, segments and other streams available for analysis,
            // but make an incomplete binding table explicit. Conversion backends must
            // fail closed whenever this flag is false.
            j["bindDecodingComplete"] = false;
            Json diagnostic = object();
            diagnostic["stream"] = b.kind;
            diagnostic["offset"] = uint64_t(b.off);
            diagnostic["size"] = uint64_t(b.size);
            diagnostic["message"] = e.what();
            j["bindDiagnostics"].push(diagnostic);
            stream["status"] = "partial";
            stream["reason"] = e.what();
            if (j["fixupAnomalies"].items.size() < 64) {
                Json a = object();
                a["stream"] = b.kind;
                a["reason"] = e.what();
                a["streamOffset"] = uint64_t(p > b.off ? p - b.off : 0);
                j["fixupAnomalies"].push(a);
            }
        }
        stream["decodedBinds"] = decoded;
        if (threaded)
            stream["threadedOrdinals"] = threaded;
        j["fixupStreams"].push(stream);
    }
    if (exportSize) {
        std::set<size_t> active;
        size_t visited = 0;
        std::function<void(size_t, std::string)> walk = [&](size_t node, std::string prefix) {
            r.consume(prefix.size() + 8);
            if (++visited > 1000000 || prefix.size() > 4096 || active.size() > 256 || node >= exportSize ||
                !active.insert(node).second)
                throw std::runtime_error("cyclic/oversized export trie");
            size_t p = exportOff + node, end = exportOff + exportSize;
            size_t len = r.leb(p, end);
            if (len > end - p)
                throw std::runtime_error("truncated export terminal");
            size_t next = p + len;
            if (len) {
                Json e = object();
                e["name"] = prefix;
                auto flags = r.leb(p, next);
                e["flags"] = flags;
                if (flags & 8) {
                    e["ordinal"] = r.leb(p, next);
                    e["importName"] = r.str(p, next);
                } else {
                    e["address"] = r.leb(p, next);
                    if (flags & 16)
                        e["resolver"] = r.leb(p, next);
                }
                j["exports"].push(e);
            }
            p = next;
            if (p >= end)
                throw std::runtime_error("truncated export children");
            size_t count = r.u(p++, 1);
            for (size_t x = 0; x < count; x++) {
                auto edge = r.str(p, end);
                p += edge.size() + 1;
                auto child = r.leb(p, end);
                walk(child, prefix + edge);
            }
            active.erase(node);
        };
        if (includeSymbolDetails)
            walk(0, "");
    }
    return j;
}
} // namespace
Json analyze(const std::vector<uint8_t> &data, bool includeSymbolDetails) {
    Reader r{data, 0, data.size(), true, std::make_shared<size_t>(analysisBudgetFor(data.size()))};
    auto m = r.u(0, 4);
    Json result = object();
    result["schemaVersion"] = uint64_t(1);
    result["slices"] = array();
    if (m == 0xcafebabe || m == 0xcafebabf || m == 0xbebafeca || m == 0xbfbafeca) {
        bool wide = m == 0xcafebabf || m == 0xbfbafeca;
        r.be = m == 0xcafebabe || m == 0xcafebabf;
        auto n = r.u(4, 4);
        size_t stride = wide ? 32 : 20;
        if (n == 0 || n > 64)
            throw std::runtime_error("invalid FAT slice count");
        r.check(8, n * stride);
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (size_t i = 0; i < n; i++) {
            size_t p = 8 + i * stride;
            auto off = r.u(p + 8, wide ? 8 : 4), size = r.u(p + (wide ? 16 : 12), wide ? 8 : 4),
                 align = r.u(p + (wide ? 24 : 16), 4);
            r.check(off, size);
            if (off < 8 + n * stride || !size || align > 31 || off % (uint64_t(1) << align))
                throw std::runtime_error("invalid FAT alignment/range");
            for (auto [a, b] : ranges)
                if (off < b && a < off + size)
                    throw std::runtime_error("overlapping FAT slices");
            ranges.emplace_back(off, off + size);
            auto s = thin(Reader{data, size_t(off), size_t(size), false, r.budget}, includeSymbolDetails);
            if (s.fields["cpuType"].value != std::to_string(r.u(p, 4)) ||
                s.fields["cpuSubtype"].value != std::to_string(r.u(p + 4, 4)))
                throw std::runtime_error("FAT architecture mismatch");
            result["slices"].push(s);
        }
    } else
        result["slices"].push(thin(Reader{data, 0, data.size(), false, r.budget}, includeSymbolDetails));
    return result;
}
} // namespace radek
