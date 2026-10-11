#pragma once

#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/shim_registry.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace radek::compat_runtime::darwin_compat {

/**
 * Minimal translation layer for the Darwin-only imports that have no Android
 * system export.
 *
 * Every binding here is an explicit, individually reported emulation of a name
 * Android does not ship (the `__p` stream variables, the Darwin ctype sweep
 * functions, the EAGL drawable string keys, the OpenAL entry points, and the
 * GCC SJLJ personality). None of them is a same-name NDK export, and the
 * adapter never claims more than it does:
 *
 * - the standard-stream cells carry real process-stream handles, so guest
 *   fread/fwrite/fprintf calls work against stdin/stdout/stderr;
 * - `__tolower`/`__toupper`/`__maskrune` are C-locale (ASCII) ctype;
 * - the EAGL keys are real guest `NSString` objects;
 * - `__error` returns a guest `int` errno cell;
 * - OpenAL is state-only: ids and parameters are tracked, no audio device is
 *   opened and no samples are produced.
 */
class ShimAdapter {
  public:
    explicit ShimAdapter(objc::ShimAdapter *strings = nullptr);

    /** Adds only the named Darwin-only symbols with an explicit adapter. */
    void registerBindings(ShimRegistry &registry);

    std::size_t boundSymbolCount() const noexcept { return boundSymbols_; }
    std::uint64_t ctypeCalls() const noexcept { return ctypeCalls_; }
    std::uint64_t openalCalls() const noexcept { return openalCalls_; }
    std::uint64_t streamCellCount() const noexcept { return streamCells_; }
    std::uint64_t personalityBoundaries() const noexcept { return personalityBoundaries_; }
    const std::vector<std::string> &diagnostics() const noexcept { return diagnostics_; }

  private:
    using Invoke = std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)>;

    void note(const std::string &detail);
    void registerFunction(ShimRegistry &registry, const std::string &symbol,
                          const std::string &adapterName, Invoke invoke);
    void registerExceptionFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName, Invoke invoke);
    void registerData(ShimRegistry &registry, const std::string &symbol,
                      const std::string &adapterName,
                      std::function<bool(GuestAddressSpace &, GuestAddress &, std::string &)> resolve);

    GuestAddress dataPage(GuestAddressSpace &memory, const std::string &label);
    GuestAddress writableCell(GuestAddressSpace &memory, const std::string &label,
                              std::uint32_t value);
    GuestAddress constantString(GuestAddressSpace &memory, const std::string &symbol,
                                const std::string &value, std::string &reason);
    GuestAddress errorCell(GuestAddressSpace &memory, std::string &reason);
    // Guest cell holding a constant's raw value (a CGFloat, or a CGRect/CGSize/CGAffineTransform).
    GuestAddress valueCell(GuestAddressSpace &memory, const std::string &symbol,
                           const std::vector<std::uint8_t> &bytes, std::string &reason);

    objc::ShimAdapter *strings_ = nullptr;
    GuestAddress nextCallout_ = 0xf0050000;
    std::size_t boundSymbols_ = 0;
    std::uint64_t ctypeCalls_ = 0;
    std::uint64_t openalCalls_ = 0;
    std::uint64_t streamCells_ = 0;
    std::uint64_t personalityBoundaries_ = 0;
    std::uint32_t nextBufferId_ = 1;
    std::uint32_t nextSourceId_ = 1;
    std::uint32_t nextDeviceToken_ = 0x0a000001;
    std::uint32_t nextContextToken_ = 0x0c000001;
    // Keyed by the address space the data symbol was materialized for, like the
    // Objective-C adapter does for its class objects.
    std::map<const GuestAddressSpace *, GuestAddress> errnoCells_;
    std::map<const GuestAddressSpace *, GuestAddress> dataPages_;
    std::map<const GuestAddressSpace *, std::map<std::string, GuestAddress>> constantStrings_;
    std::map<const GuestAddressSpace *, std::map<std::string, GuestAddress>> valueCells_;
    std::map<GuestAddress, std::map<std::uint32_t, std::uint32_t>> sourceIntState_;
    std::map<GuestAddress, std::map<std::uint32_t, std::uint32_t>> sourceFloatBits_;
    std::map<GuestAddress, std::vector<std::uint32_t>> sourceQueues_;
    std::vector<std::string> diagnostics_;
};

} // namespace radek::compat_runtime::darwin_compat
