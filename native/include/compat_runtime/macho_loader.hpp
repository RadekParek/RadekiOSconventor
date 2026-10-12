#pragma once

#include "compat_runtime/cpu.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/trap_shims.hpp"
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
    std::string entryPointSource;
    bool thumb = false;
    CpuRegisterState initialRegisters;
    // Guest addresses from every __mod_init_func entry (C++ static constructors).
    // Real dyld runs them before main; the runner must do the same.
    std::vector<GuestAddress> initializers;
    std::uint32_t segmentCount = 0;
    std::uint32_t rebasesApplied = 0;
    std::vector<radek::Json> resolvedSymbols;
    std::vector<radek::Json> unresolvedSymbols;
    // Imports bound to abort-on-call traps by loadWithTraps. They have no
    // implementation; the slot holds a trap address so execution can proceed
    // until the import is actually used. Empty for load().
    std::vector<radek::Json> trappedSymbols;
    // Undefined symbols with no loader binding location (nlist-only). Seen by
    // loadWithTraps only; load() keeps them in unresolvedSymbols.
    std::vector<radek::Json> unboundNlistSymbols;
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

    /**
     * Load and bind every trappable unresolved import to an abort-on-call
     * trap. The registry gains one trap binding per unimplemented import;
     * trapped imports are reported under trappedSymbols (never resolved) and
     * firstMissingImport still names the first import without an
     * implementation. Status is LOADED_WITH_TRAPS when every slot-backed
     * import is bound and only traps (or unbindable nlist names) remain.
     */
    MachOLoadReport loadWithTraps(const std::vector<std::uint8_t> &mainBinary,
                                  GuestAddressSpace &addressSpace,
                                  ShimRegistry &shims,
                                  TrapShimAdapter &traps,
                                  GuestAddress slide = 0) const;
};

} // namespace radek::compat_runtime
