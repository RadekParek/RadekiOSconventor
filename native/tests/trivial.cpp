// Host tests for the bounded on-device conversion prover.
#include "trivial.hpp"
#include "macho.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using radek::proveArm64IntegerLeaf;

namespace {
std::vector<uint8_t> words(std::initializer_list<uint32_t> instructions) {
    std::vector<uint8_t> bytes;
    for (uint32_t word : instructions) {
        bytes.push_back(uint8_t(word & 0xFF));
        bytes.push_back(uint8_t((word >> 8) & 0xFF));
        bytes.push_back(uint8_t((word >> 16) & 0xFF));
        bytes.push_back(uint8_t((word >> 24) & 0xFF));
    }
    return bytes;
}

size_t prove(const std::vector<uint8_t> &code, std::string &reason) {
    return proveArm64IntegerLeaf(code.data(), code.size(), &reason);
}

void testHelloRoutineIsProven() {
    auto code = words({0x52800240, 0x11000400 | (1 << 10), 0xD65F03C0});
    std::string reason;
    const size_t consumed = prove(code, reason);
    assert(consumed == code.size());
    assert(reason.empty());
}

void testSampleLeafRoutineIsProven() {
    auto code = words({0x52800500, 0x2A0003E1, 0x11000821, 0x2A0103E0, 0xD65F03C0});
    std::string reason;
    assert(prove(code, reason) == code.size());
}

void testMovkRequiresInitialization() {
    auto code = words({0x72A00020, 0xD65F03C0}); // MOVK W0, #1 without MOVZ
    std::string reason;
    assert(prove(code, reason) == 0);
    assert(reason.find("uninitialized") != std::string::npos);
}

void testMovkAfterMovzIsProven() {
    auto code = words({0x52800000, 0x72A00020, 0xD65F03C0});
    std::string reason;
    assert(prove(code, reason) == code.size());
}

void testAddFromUninitializedRegisterRejected() {
    auto code = words({0x11000421, 0xD65F03C0}); // ADD W1, W1, #1 before init
    std::string reason;
    assert(prove(code, reason) == 0);
}

void testReturnWithoutInitializedResultRejected() {
    auto code = words({0x2A0003E1, 0xD65F03C0}); // MOV W1, W0; RET (w0 never set)
    std::string reason;
    assert(prove(code, reason) == 0);
    assert(reason.find("not a closed leaf") != std::string::npos);
    // RET directly with no initialized return register.
    auto bare = words({0xD65F03C0});
    reason.clear();
    assert(prove(bare, reason) == 0);
    assert(reason.find("return value") != std::string::npos);
}

void testMovFromZeroRegisterInitializes() {
    auto code = words({0x2A1F03E0, 0xD65F03C0}); // MOV W0, WZR; RET
    std::string reason;
    assert(prove(code, reason) == code.size());
}

void testCalleeSavedRegisterWriteRejected() {
    auto code = words({0x52800250, 0xD65F03C0}); // MOV W16, #18
    std::string reason;
    assert(prove(code, reason) == 0);
    assert(reason.find("register write") != std::string::npos);
}

void testBranchRejected() {
    auto code = words({0x52800240, 0x14000002, 0xD65F03C0});
    std::string reason;
    assert(prove(code, reason) == 0);
    assert(reason.find("proven subset") != std::string::npos);
}

void testMissingReturnRejected() {
    auto code = words({0x52800240, 0x11000400 | (1 << 10)});
    std::string reason;
    assert(prove(code, reason) == 0);
    assert(reason.find("terminate") != std::string::npos);
}

void testTruncatedInstructionRejected() {
    std::vector<uint8_t> code = {0x40, 0x02, 0x80}; // 3 bytes only
    std::string reason;
    assert(prove(code, reason) == 0);
}

void testEmptyInputRejected() {
    std::string reason;
    assert(proveArm64IntegerLeaf(nullptr, 0, &reason) == 0);
}

void testTranslateTrivialRejectsGarbage() {
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03};
    radek::Json result = radek::translateTrivial(garbage);
    assert(result.fields.at("status").value == "UNSUPPORTED");
}

void writeLe32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
}

void writeLe64(std::vector<uint8_t> &bytes, size_t offset, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
}

std::vector<uint8_t> imageWithTextRelocations(uint32_t relocationCount) {
    constexpr size_t headerSize = 32;
    constexpr size_t segmentCommandSize = 152; // LC_SEGMENT_64 + one section
    constexpr size_t mainCommandSize = 24;
    constexpr size_t commandBytes = segmentCommandSize + mainCommandSize;
    constexpr size_t segmentCommand = headerSize;
    constexpr size_t section = segmentCommand + 72;
    constexpr size_t mainCommand = segmentCommand + segmentCommandSize;
    constexpr size_t codeOffset = 0x100;
    constexpr size_t relocationOffset = 0x200;
    constexpr uint64_t textVmAddress = 0x100000000;
    const auto code = words({0x52800240, 0x11000400 | (1 << 10), 0xD65F03C0});
    const size_t imageSize = std::max(codeOffset + code.size(), relocationOffset + static_cast<size_t>(relocationCount) * 8);
    std::vector<uint8_t> image(imageSize, 0);

    // 64-bit little-endian arm64 MH_EXECUTE with __TEXT/__text and LC_MAIN.
    image[0] = 0xcf; image[1] = 0xfa; image[2] = 0xed; image[3] = 0xfe;
    writeLe32(image, 4, 0x0100000c);
    writeLe32(image, 12, 2);
    writeLe32(image, 16, 2);
    writeLe32(image, 20, commandBytes);

    writeLe32(image, segmentCommand, 0x19); // LC_SEGMENT_64
    writeLe32(image, segmentCommand + 4, segmentCommandSize);
    std::copy_n("__TEXT", 6, image.begin() + segmentCommand + 8);
    writeLe64(image, segmentCommand + 24, textVmAddress);
    writeLe64(image, segmentCommand + 32, imageSize);
    writeLe64(image, segmentCommand + 40, 0);
    writeLe64(image, segmentCommand + 48, imageSize);
    writeLe32(image, segmentCommand + 56, 7);
    writeLe32(image, segmentCommand + 60, 5); // VM_PROT_READ | VM_PROT_EXECUTE
    writeLe32(image, segmentCommand + 64, 1);
    std::copy_n("__text", 6, image.begin() + section);
    std::copy_n("__TEXT", 6, image.begin() + section + 16);
    writeLe64(image, section + 32, textVmAddress + codeOffset);
    writeLe64(image, section + 40, code.size());
    writeLe32(image, section + 48, codeOffset);
    writeLe32(image, section + 52, 2);
    writeLe32(image, section + 56, relocationCount ? relocationOffset : 0);
    writeLe32(image, section + 60, relocationCount);
    writeLe32(image, section + 64, 0x80000400);

    writeLe32(image, mainCommand, 0x80000028); // LC_MAIN
    writeLe32(image, mainCommand + 4, mainCommandSize);
    writeLe64(image, mainCommand + 8, codeOffset);
    std::copy(code.begin(), code.end(), image.begin() + codeOffset);
    return image;
}

std::vector<uint8_t> imageWithImportCount(uint32_t count) {
    constexpr size_t headerSize = 32;
    constexpr size_t commandSize = 24;
    constexpr size_t symbolOffset = headerSize + commandSize;
    constexpr size_t symbolEntrySize = 16;
    std::vector<uint32_t> stringIndexes;
    stringIndexes.reserve(count);
    std::string strings(1, '\0');
    for (uint32_t index = 0; index < count; ++index) {
        stringIndexes.push_back(static_cast<uint32_t>(strings.size()));
        strings += "_compact_import_" + std::to_string(index);
        strings.push_back('\0');
    }
    const size_t stringsOffset = symbolOffset + static_cast<size_t>(count) * symbolEntrySize;
    std::vector<uint8_t> image(stringsOffset + strings.size(), 0);
    // 64-bit little-endian arm64 MH_EXECUTE with one LC_SYMTAB.
    image[0] = 0xcf; image[1] = 0xfa; image[2] = 0xed; image[3] = 0xfe;
    writeLe32(image, 4, 0x0100000c);
    writeLe32(image, 12, 2);
    writeLe32(image, 16, 1);
    writeLe32(image, 20, commandSize);
    writeLe32(image, headerSize, 2); // LC_SYMTAB
    writeLe32(image, headerSize + 4, commandSize);
    writeLe32(image, headerSize + 8, symbolOffset);
    writeLe32(image, headerSize + 12, count);
    writeLe32(image, headerSize + 16, stringsOffset);
    writeLe32(image, headerSize + 20, strings.size());
    for (uint32_t index = 0; index < count; ++index) {
        const size_t entry = symbolOffset + static_cast<size_t>(index) * symbolEntrySize;
        writeLe32(image, entry, stringIndexes[index]);
        image[entry + 4] = 1; // N_EXT | N_UNDF
    }
    std::copy(strings.begin(), strings.end(), image.begin() + stringsOffset);
    return image;
}

std::vector<uint8_t> imageWithSymbolCount(uint32_t count) {
    constexpr size_t headerSize = 32;
    constexpr size_t commandSize = 24;
    constexpr size_t symbolOffset = headerSize + commandSize;
    constexpr size_t symbolEntrySize = 16;
    const std::string importedName = "_mach_absolute_time";
    const size_t stringsOffset = symbolOffset + static_cast<size_t>(count) * symbolEntrySize;
    const size_t stringSize = importedName.size() + 2;
    std::vector<uint8_t> image(stringsOffset + stringSize, 0);
    // 64-bit little-endian arm64 MH_EXECUTE with one LC_SYMTAB.
    image[0] = 0xcf; image[1] = 0xfa; image[2] = 0xed; image[3] = 0xfe;
    writeLe32(image, 4, 0x0100000c);
    writeLe32(image, 12, 2);
    writeLe32(image, 16, 1);
    writeLe32(image, 20, commandSize);
    writeLe32(image, headerSize, 2); // LC_SYMTAB
    writeLe32(image, headerSize + 4, commandSize);
    writeLe32(image, headerSize + 8, symbolOffset);
    writeLe32(image, headerSize + 12, count);
    writeLe32(image, headerSize + 16, stringsOffset);
    writeLe32(image, headerSize + 20, stringSize);
    const size_t importedEntry = symbolOffset + static_cast<size_t>(count - 1) * symbolEntrySize;
    writeLe32(image, importedEntry, 1); // n_strx points past the empty string
    image[importedEntry + 4] = 1;       // N_EXT | N_UNDF
    std::copy(importedName.begin(), importedName.end(), image.begin() + stringsOffset + 1);
    return image;
}

void testTextRelocationsRejectConversion() {
    const auto unrelocated = radek::translateTrivial(imageWithTextRelocations(0));
    assert(unrelocated.fields.at("status").value == "PROVEN");

    const auto relocated = radek::translateTrivial(imageWithTextRelocations(1));
    assert(relocated.fields.at("status").value == "UNSUPPORTED");
    assert(relocated.fields.at("reason").value.find("relocations in the proven __text section") != std::string::npos);
}

void testCompactImportInventoryIsBoundedAndMarkedTruncated() {
    constexpr uint32_t kImports = 100001;
    const auto compact = radek::analyze(imageWithImportCount(kImports), false);
    const auto &slice = compact.fields.at("slices").items.front();
    assert(slice.fields.at("symbolCount").value == std::to_string(kImports));
    assert(slice.fields.at("imports").items.size() == 100000);
    assert(slice.fields.at("importsTruncated").kind == radek::Json::Bool);
    assert(slice.fields.at("importsTruncated").value == "true");
}

void testSymbolTablesAboveFormerLimitAreParsed() {
    constexpr uint32_t kSymbols = 100001;
    const auto image = imageWithSymbolCount(kSymbols);
    const auto full = radek::analyze(image);
    const auto &slice = full.fields.at("slices").items.front();
    assert(slice.fields.at("symbols").items.size() == kSymbols);
    assert(slice.fields.at("symbolCount").value == std::to_string(kSymbols));
    assert(slice.fields.at("imports").items.size() == 1);

    // The Android importer uses the compact path: it still counts and scans the
    // entire table and retains the one undefined import, but does not retain
    // 100,000 local symbol objects in JNI JSON.
    const auto compact = radek::analyze(image, false);
    const auto &compactSlice = compact.fields.at("slices").items.front();
    assert(compactSlice.fields.at("symbolCount").value == std::to_string(kSymbols));
    assert(compactSlice.fields.at("symbols").items.empty());
    assert(compactSlice.fields.at("imports").items.size() == 1);
    assert(compactSlice.fields.at("imports").items.front().fields.at("name").value == "_mach_absolute_time");
}
} // namespace

int main() {
    testHelloRoutineIsProven();
    testSampleLeafRoutineIsProven();
    testMovkRequiresInitialization();
    testMovkAfterMovzIsProven();
    testAddFromUninitializedRegisterRejected();
    testReturnWithoutInitializedResultRejected();
    testMovFromZeroRegisterInitializes();
    testCalleeSavedRegisterWriteRejected();
    testBranchRejected();
    testMissingReturnRejected();
    testTruncatedInstructionRejected();
    testEmptyInputRejected();
    testTranslateTrivialRejectsGarbage();
    testTextRelocationsRejectConversion();
    testCompactImportInventoryIsBoundedAndMarkedTruncated();
    testSymbolTablesAboveFormerLimitAreParsed();
    std::printf("bounded conversion prover tests passed\n");
    return 0;
}
