#pragma once
#include "json.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace radek {
/// Instruction-level proof of the closed-integer ARM64 leaf subset
/// (MOV-immediate, MOVK, register MOV, immediate ADD/SUB, RET).
/// Returns the number of consumed source bytes on success, or 0 with a
/// human-readable reason on failure. Never executes the instructions.
size_t proveArm64IntegerLeaf(const uint8_t *code, size_t size, std::string *reason);

/// On-device bounded conversion assessment. Verifies that a whole Mach-O
/// executable is exactly one proven closed-integer ARM64 routine with no
/// imports, dependencies, fixups or metadata, and returns its machine code
/// plus recovered __cstring launch messages. Fails closed with UNSUPPORTED.
Json translateTrivial(const std::vector<uint8_t> &data);
} // namespace radek
