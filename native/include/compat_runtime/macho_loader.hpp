#pragma once

#include "compat_runtime/cpu.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace radek::compat_runtime {

struct MachOLoadReport {
    bool mapped = false;
    std::string status = "BLOCKED";
    std::string error;
    GuestAddress entryPoint = 0;
    bool thumb = false;
    CpuRegisterState initialRegisters;
    std::uint32_t segmentCount = 0;
    std::uint32_t rebasesApplied = 0;
    std::vector<radek::Json> resolvedSymbols;
    std::vector<radek::Json> unresolvedSymbols;
    std::optional<std::string> firstMissingImport;

    bool imageMapped() const noexcept;
    radek::Json toJson() const;
};

/** Bounded loader for thin or FAT 32-bit little-endian ARM Mach-O executables. */
class MachOLoader {
  public:
    MachOLoadReport load(const std::vector<std::uint8_t> &mainBinary,
                         GuestAddressSpace &addressSpace,
                         const ShimRegistry &shims,
                         GuestAddress slide = 0) const;
};

} // namespace radek::compat_runtime
