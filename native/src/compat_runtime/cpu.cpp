#include "compat_runtime/cpu.hpp"

#include <memory>

namespace radek::compat_runtime {
namespace {
class UnavailableArm32Backend final : public CpuBackend {
  public:
    const char *name() const noexcept override { return "arm32-backend-unavailable"; }
    bool available() const noexcept override { return false; }

    bool prepareGuestFunction(const GuestFunction &, const GuestMemoryCallbacks &,
                              PreparedGuestFunction &, std::string &reason) const override {
        reason = "ARM32 CPU backend is not linked; guest functions were not executed.";
        return false;
    }

    CpuExecutionResult executeGuestFunction(const PreparedGuestFunction &,
                                             const GuestMemoryCallbacks &,
                                             const CpuRegisterState &registers) const override {
        CpuExecutionResult result;
        result.status = CpuExecutionStatus::BackendUnavailable;
        result.registers = registers;
        result.message = "ARM32 CPU backend is not linked; guest functions were not executed.";
        return result;
    }
};
} // namespace

#ifdef RADEK_HAVE_UNICORN
std::unique_ptr<CpuBackend> createUnicornArm32Backend();
#endif

std::unique_ptr<CpuBackend> createArm32CpuBackend() {
#ifdef RADEK_HAVE_UNICORN
    return createUnicornArm32Backend();
#else
    return std::make_unique<UnavailableArm32Backend>();
#endif
}

} // namespace radek::compat_runtime
