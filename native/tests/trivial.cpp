// Host tests for the bounded on-device conversion prover.
#include "trivial.hpp"
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
    std::printf("bounded conversion prover tests passed\n");
    return 0;
}
