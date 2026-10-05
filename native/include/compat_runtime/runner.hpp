#pragma once

#include "compat_runtime/cpu.hpp"
#include "compat_runtime/macho_loader.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "json.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace radek::compat_runtime {

/** Loads one authorized IPA main executable and reports its first runtime blocker. */
class GuestRunner {
    const ShimRegistry &shims_;
    const CpuBackend &cpu_;
    std::size_t memoryLimit_;

  public:
    GuestRunner(const ShimRegistry &shims, const CpuBackend &cpu,
                std::size_t memoryLimit = 256U * 1024U * 1024U)
        : shims_(shims), cpu_(cpu), memoryLimit_(memoryLimit) {}

    radek::Json runMainBinary(const std::vector<std::uint8_t> &mainBinary,
                              bool authorizationConfirmed) const;
};

} // namespace radek::compat_runtime
