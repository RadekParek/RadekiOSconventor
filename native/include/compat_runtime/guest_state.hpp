#pragma once

#include <array>
#include <cstdint>

namespace radek::compat_runtime {

struct CpuRegisterState {
    std::array<std::uint32_t, 16> r{};
    std::uint32_t cpsr = 0;
};

} // namespace radek::compat_runtime
