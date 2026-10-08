#include "compat_runtime/ndk_compat_shims.hpp"

#include "compat_runtime/ndk_full_import_catalog.hpp"
#include "compat_runtime/ndk_import_catalog.hpp"
#include "compat_runtime/virtual_file_system.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace radek::compat_runtime::ndk {
namespace {

constexpr GuestAddress kCalloutEnd = 0xf0100000U;
constexpr std::size_t kMaximumStringBytes = 1U * 1024U * 1024U;
constexpr int kErrnoInvalidArgument = 22;
constexpr int kErrnoNoSuchProcess = 3;
constexpr int kErrnoBusy = 16;
constexpr int kErrnoNotImplemented = 38;
constexpr int kEof = -1;

bool readGuest(const GuestAddressSpace &memory, GuestAddress address, void *destination,
               std::size_t size) {
    if (size == 0)
        return true;
    if (memory.read(address, destination, size))
        return true;
    auto *bytes = static_cast<std::uint8_t *>(destination);
    for (std::size_t index = 0; index < size; ++index) {
        if (!memory.read(static_cast<GuestAddress>(address + index), bytes + index, 1))
            return false;
    }
    return true;
}

bool writeGuest(GuestAddressSpace &memory, GuestAddress address, const void *source,
                std::size_t size) {
    if (size == 0)
        return true;
    if (memory.write(address, source, size))
        return true;
    const auto *bytes = static_cast<const std::uint8_t *>(source);
    for (std::size_t index = 0; index < size; ++index) {
        if (!memory.write(static_cast<GuestAddress>(address + index), bytes + index, 1))
            return false;
    }
    return true;
}

template <typename T>
bool readValue(const GuestAddressSpace &memory, GuestAddress address, T &value) {
    return readGuest(memory, address, &value, sizeof(value));
}

template <typename T>
bool writeValue(GuestAddressSpace &memory, GuestAddress address, const T &value) {
    return writeGuest(memory, address, &value, sizeof(value));
}

bool readCString(const GuestAddressSpace &memory, GuestAddress address, std::string &result,
                 std::size_t limit = kMaximumStringBytes) {
    result.clear();
    if (address == 0)
        return false;
    GuestAddress cursor = address;
    while (result.size() < limit) {
        std::array<char, 64> chunk{};
        if (!readGuest(memory, cursor, chunk.data(), chunk.size()))
            return false;
        const auto *terminator = static_cast<const char *>(
            std::memchr(chunk.data(), '\0', chunk.size()));
        if (terminator != nullptr) {
            result.append(chunk.data(), static_cast<std::size_t>(terminator - chunk.data()));
            return true;
        }
        result.append(chunk.data(), chunk.size());
        cursor = static_cast<GuestAddress>(cursor + chunk.size());
    }
    return false;
}

GuestAddress allocateGuestString(GuestAddressSpace &memory, const std::string &value,
                                 const char *name) {
    try {
        const auto address = memory.mapAny(value.size() + 1, MemoryPermission::Read |
                                                                        MemoryPermission::Write,
                                           name, 4);
        const auto text = value + '\0';
        if (!writeGuest(memory, address, text.data(), text.size()))
            return 0;
        return address;
    } catch (const std::exception &) {
        return 0;
    }
}

float singleArgument(const CpuRegisterState &registers, int index) {
    const auto bits = static_cast<std::uint32_t>(
        registers.d[static_cast<std::size_t>(index / 2)] >> ((index % 2) ? 32 : 0));
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

double doubleArgument(const CpuRegisterState &registers, int index) {
    double result = 0.0;
    const auto bits = registers.d[static_cast<std::size_t>(index)];
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

void setSingleResult(CpuRegisterState &registers, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    registers.d[0] = (registers.d[0] & ~std::uint64_t{0xffffffffU}) | bits;
}

void setDoubleResult(CpuRegisterState &registers, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    registers.d[0] = bits;
}

std::tm tmFromGuest(const GuestAddressSpace &memory, GuestAddress address, bool &ok) {
    std::array<std::int32_t, 9> values{};
    ok = readGuest(memory, address, values.data(), sizeof(values));
    std::tm result{};
    if (!ok)
        return result;
    result.tm_sec = values[0];
    result.tm_min = values[1];
    result.tm_hour = values[2];
    result.tm_mday = values[3];
    result.tm_mon = values[4];
    result.tm_year = values[5];
    result.tm_wday = values[6];
    result.tm_yday = values[7];
    result.tm_isdst = values[8];
    return result;
}

bool tmToGuest(GuestAddressSpace &memory, GuestAddress address, const std::tm &value) {
    const std::array<std::int32_t, 9> values{{
        value.tm_sec, value.tm_min, value.tm_hour, value.tm_mday, value.tm_mon,
        value.tm_year, value.tm_wday, value.tm_yday, value.tm_isdst,
    }};
    return writeGuest(memory, address, values.data(), sizeof(values));
}

bool readGuestStringForOutput(const GuestAddressSpace &memory, GuestAddress address,
                              std::string &value, std::string &reason) {
    if (!readCString(memory, address, value)) {
        reason = "NDK compatibility callout could not read a guest string";
        return false;
    }
    return true;
}

bool readGuestBytes(const GuestAddressSpace &memory, GuestAddress address, std::size_t size,
                    std::vector<std::uint8_t> &bytes, std::string &reason) {
    constexpr std::size_t kMaximumBuffer = 16U * 1024U * 1024U;
    if (size > kMaximumBuffer) {
        reason = "NDK compatibility byte operation exceeds the bounded buffer limit";
        return false;
    }
    bytes.resize(size);
    if (size != 0 && !readGuest(memory, address, bytes.data(), size)) {
        reason = "NDK compatibility byte operation could not read guest memory";
        return false;
    }
    return true;
}

std::uint32_t adler32(std::uint32_t initial, const std::vector<std::uint8_t> &bytes) {
    constexpr std::uint32_t kModulus = 65521U;
    std::uint32_t low = initial & 0xffffU;
    std::uint32_t high = (initial >> 16) & 0xffffU;
    if (initial == 0) { low = 1; high = 0; }
    for (const auto byte : bytes) {
        low = (low + byte) % kModulus;
        high = (high + low) % kModulus;
    }
    return (high << 16) | low;
}

std::uint32_t crc32(std::uint32_t initial, const std::vector<std::uint8_t> &bytes) {
    std::uint32_t crc = ~initial;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

bool assetLength(GuestAddress handle, bool remaining, std::uint64_t &result,
                 std::string &reason) {
    auto &filesystem = guestFileSystem();
    const auto length = filesystem.length(handle, reason);
    if (length < 0)
        return false;
    if (!remaining) {
        result = static_cast<std::uint64_t>(length);
        return true;
    }
    const auto position = filesystem.tell(handle, reason);
    if (position < 0 || position > length) {
        reason = "AAsset remaining length is outside the bounded file range";
        return false;
    }
    result = static_cast<std::uint64_t>(length - position);
    return true;
}

void setAssetLengthResult(CpuRegisterState &registers, std::uint64_t value,
                          bool wide) {
    registers.r[0] = static_cast<std::uint32_t>(value);
    if (wide)
        registers.r[1] = static_cast<std::uint32_t>(value >> 32);
}

bool writeOutput(GuestAddressSpace &memory, GuestAddress destination, std::size_t capacity,
                 const std::string &value, std::string &reason) {
    if (destination == 0 || capacity == 0)
        return true;
    const auto count = std::min(capacity - 1, value.size());
    if (!writeGuest(memory, destination, value.data(), count) ||
        !writeGuest(memory, static_cast<GuestAddress>(destination + count), "\0", 1)) {
        reason = "NDK compatibility callout could not write the guest output buffer";
        return false;
    }
    return true;
}

bool isDelimiter(char character, const std::string &delimiters) {
    return delimiters.find(character) != std::string::npos;
}

} // namespace

void ShimAdapter::registerFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName, Invoke invoke) {
    // Existing concrete adapters are more precise than this catch-all NDK
    // family. In particular, do not replace GLES, libSystem, or C++ ABI
    // bindings with a generic default.
    if (registry.resolve(symbol).has_value())
        return;
    if (nextCallout_ > kCalloutEnd - 4U)
        throw std::overflow_error("NDK compatibility callout window is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "Android NDK/system ABI";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    nextCallout_ += 4U;
    binding.invoke = [this, invoke = std::move(invoke)](CpuRegisterState &registers,
                                                        GuestAddressSpace &memory,
                                                        std::string &reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++calls_;
        }
        return invoke(registers, memory, reason);
    };
    registry.registerBinding(std::move(binding));
}

void ShimAdapter::registerExceptionBoundary(ShimRegistry &registry, const std::string &symbol,
                                            const std::string &adapterName,
                                            const std::string &reason) {
    if (registry.resolve(symbol).has_value())
        return;
    if (nextCallout_ > kCalloutEnd - 4U)
        throw std::overflow_error("NDK compatibility callout window is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "Android NDK/system ABI";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    nextCallout_ += 4U;
    binding.invokeException = [this, reason = std::move(reason)](CpuRegisterState &,
                                                                  GuestAddressSpace &,
                                                                  std::string &detail) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++calls_;
        }
        detail = reason;
        return true;
    };
    registry.registerBinding(std::move(binding));
}

void ShimAdapter::registerGenericCandidate(ShimRegistry &registry, const char *symbol,
                                           const char *family) {
    if (symbol == nullptr)
        throw std::logic_error("null NDK catalog symbol");
    if (family == nullptr)
        throw std::logic_error(std::string("null NDK catalog family for ") + symbol);
    const std::string name(symbol);
    if (name == "_environ") {
        if (registry.resolve(name).has_value())
            return;
        ShimBinding binding;
        binding.darwinSymbol = name;
        binding.library = "Android NDK/system ABI";
        binding.adapterName = "ndk-data-environ";
        binding.resolveGuestAddress = [this](GuestAddressSpace &memory, GuestAddress &address,
                                             std::string &reason) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = environCells_.find(&memory);
            if (found != environCells_.end()) {
                address = found->second;
                return true;
            }
            try {
                address = memory.mapAny(sizeof(GuestAddress), MemoryPermission::Read |
                                                                    MemoryPermission::Write,
                                         "ndk-environ-cell", alignof(GuestAddress));
                const GuestAddress nullPointer = 0;
                if (!writeValue(memory, address, nullPointer)) {
                    reason = "environ cell could not be initialized";
                    return false;
                }
                environCells_[&memory] = address;
                return true;
            } catch (const std::exception &) {
                reason = "environ cell could not be allocated in bounded guest memory";
                return false;
            }
        };
        registry.registerBinding(std::move(binding));
        return;
    }

    if (name == "___errno") {
        registerFunction(registry, name, "ndk-errno-cell", [this](CpuRegisterState &registers,
                                                                     GuestAddressSpace &memory,
                                                                     std::string &reason) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = errnoCells_.find(&memory);
            if (found == errnoCells_.end()) {
                try {
                    const auto address = memory.mapAny(sizeof(std::int32_t),
                                                       MemoryPermission::Read | MemoryPermission::Write,
                                                       "ndk-errno-cell", alignof(std::int32_t));
                    const std::int32_t zero = 0;
                    if (!writeValue(memory, address, zero)) {
                        reason = "errno cell could not be initialized";
                        return false;
                    }
                    found = errnoCells_.emplace(&memory, address).first;
                } catch (const std::exception &) {
                    reason = "errno cell could not be allocated in bounded guest memory";
                    return false;
                }
            }
            registers.r[0] = found->second;
            return true;
        });
        return;
    }

    // A stack protector failure or non-local process/thread transfer must not
    // return as if it succeeded. It is still registered so the loader can bind
    // the name and the report can identify the safe boundary.
    if (name.find("stack_chk_fail") != std::string::npos) {
        registerExceptionBoundary(registry, name, "ndk-stack-protector-boundary",
                                  "guest stack-protector failure raised a bounded exception boundary");
        return;
    }

    const std::string providerFamily = family == nullptr ? "unknown" : family;
    registerFunction(registry, name, "ndk-bounded-" + providerFamily,
                     [this](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         {
                             std::lock_guard<std::mutex> lock(mutex_);
                             ++genericCalls_;
                         }
                         // Generic providers are intentionally safe ABI
                         // boundaries, not silent claims of framework behavior.
                         // A type-agnostic zero is the only safe default here:
                         // it is NULL for pointer results and a failure/no-op
                         // value for the scalar or void families. Signatures
                         // that can be implemented safely are registered above
                         // with typed adapters instead of relying on this path.
                         registers.r[0] = 0;
                         return true;
                     });
}

void ShimAdapter::registerBindings(ShimRegistry &registry) {
    // ---- process/control boundaries ---------------------------------------
    registerExceptionBoundary(registry, "_abort", "ndk-abort-boundary",
                              "guest abort requested; execution stopped safely");
    registerExceptionBoundary(registry, "_exit", "ndk-exit-boundary",
                              "guest exit requested; execution stopped safely");
    registerExceptionBoundary(registry, "_longjmp", "ndk-longjmp-boundary",
                              "guest longjmp requires a guest stack transfer and is a bounded stop boundary");
    registerExceptionBoundary(registry, "_pthread_exit", "ndk-pthread-exit-boundary",
                              "guest pthread_exit requires a guest scheduler and is a bounded stop boundary");

    registerFunction(registry, "_setjmp", "ndk-setjmp-state",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         if (registers.r[0] == 0 || !memory.contains(registers.r[0], 32,
                                                                        MemoryPermission::Write)) {
                             reason = "setjmp received an invalid guest jump buffer";
                             return false;
                         }
                         std::array<std::uint32_t, 8> state{};
                         for (std::size_t index = 0; index < state.size(); ++index)
                             state[index] = registers.r[index];
                         if (!writeGuest(memory, registers.r[0], state.data(), sizeof(state))) {
                             reason = "setjmp could not save the bounded guest register state";
                             return false;
                         }
                         registers.r[0] = 0;
                         return true;
                     });

    // ---- scalar math -------------------------------------------------------
    registerFunction(registry, "_acosf", "ndk-math-acosf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::acos(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_asinf", "ndk-math-asinf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::asin(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_atanf", "ndk-math-atanf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::atan(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_atan2f", "ndk-math-atan2f",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::atan2(singleArgument(r, 0), singleArgument(r, 1)));
                         return true;
                     });
    registerFunction(registry, "_cosf", "ndk-math-cosf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::cos(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_coshf", "ndk-math-coshf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::cosh(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_expf", "ndk-math-expf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::exp(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_log10f", "ndk-math-log10f",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::log10(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_logf", "ndk-math-logf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::log(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_sinf", "ndk-math-sinf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::sin(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_sinhf", "ndk-math-sinhf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::sinh(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_tan", "ndk-math-tan",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setDoubleResult(r, std::tan(doubleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_tanf", "ndk-math-tanf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::tan(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_tanhf", "ndk-math-tanhf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setSingleResult(r, std::tanh(singleArgument(r, 0))); return true;
                     });
    registerFunction(registry, "_fmod", "ndk-math-fmod",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setDoubleResult(r, std::fmod(doubleArgument(r, 0), doubleArgument(r, 1)));
                         return true;
                     });
    registerFunction(registry, "_frexp", "ndk-math-frexp",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         int exponent = 0;
                         const auto result = std::frexp(doubleArgument(r, 0), &exponent);
                         if (r.r[0] != 0 && !writeValue(memory, r.r[0], exponent)) {
                             reason = "frexp could not write the guest exponent";
                             return false;
                         }
                         setDoubleResult(r, result);
                         return true;
                     });
    registerFunction(registry, "_ldexp", "ndk-math-ldexp",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setDoubleResult(r, std::ldexp(doubleArgument(r, 0),
                                                       static_cast<int>(r.r[0])));
                         return true;
                     });
    registerFunction(registry, "_modf", "ndk-math-modf",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         double integral = 0.0;
                         const auto result = std::modf(doubleArgument(r, 0), &integral);
                         if (r.r[0] != 0 && !writeGuest(memory, r.r[0], &integral, sizeof(integral))) {
                             reason = "modf could not write the guest integral part";
                             return false;
                         }
                         setDoubleResult(r, result);
                         return true;
                     });
    registerFunction(registry, "_atan2", "ndk-math-atan2",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setDoubleResult(r, std::atan2(doubleArgument(r, 0), doubleArgument(r, 1)));
                         return true;
                     });
    registerFunction(registry, "_difftime", "ndk-time-difftime",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         setDoubleResult(r, std::difftime(static_cast<std::time_t>(r.r[0]),
                                                          static_cast<std::time_t>(r.r[1])));
                         return true;
                     });

    // ---- basic strings not already owned by libSystem ---------------------
    registerFunction(registry, "_strcasecmp", "ndk-string-strcasecmp",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string left, right;
                         if (!readGuestStringForOutput(memory, r.r[0], left, reason) ||
                             !readGuestStringForOutput(memory, r.r[1], right, reason)) return false;
                         auto fold = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
                         std::size_t index = 0;
                         while (index < left.size() && index < right.size()) {
                             const char a = fold(static_cast<unsigned char>(left[index]));
                             const char b = fold(static_cast<unsigned char>(right[index]));
                             if (a != b) { r.r[0] = static_cast<std::uint32_t>(
                                 static_cast<int>(static_cast<unsigned char>(a)) -
                                 static_cast<int>(static_cast<unsigned char>(b))); return true; }
                             ++index;
                         }
                         r.r[0] = static_cast<std::uint32_t>(left.size() == right.size() ? 0 :
                             (left.size() < right.size() ? -1 : 1));
                         return true;
                     });
    registerFunction(registry, "_strcoll", "ndk-string-strcoll",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string left, right;
                         if (!readGuestStringForOutput(memory, r.r[0], left, reason) ||
                             !readGuestStringForOutput(memory, r.r[1], right, reason)) return false;
                         r.r[0] = static_cast<std::uint32_t>(left.compare(right)); return true;
                     });
    registerFunction(registry, "_strcspn", "ndk-string-strcspn",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string text, reject;
                         if (!readGuestStringForOutput(memory, r.r[0], text, reason) ||
                             !readGuestStringForOutput(memory, r.r[1], reject, reason)) return false;
                         r.r[0] = static_cast<std::uint32_t>(text.find_first_of(reject));
                         if (text.find_first_of(reject) == std::string::npos) r.r[0] = text.size();
                         return true;
                     });
    registerFunction(registry, "_strncat", "ndk-string-strncat",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string destination, suffix;
                         if (!readGuestStringForOutput(memory, r.r[0], destination, reason) ||
                             !readGuestStringForOutput(memory, r.r[1], suffix, reason)) return false;
                         destination.append(suffix, 0, std::min<std::size_t>(r.r[2], suffix.size()));
                         destination.push_back('\0');
                         if (!writeGuest(memory, r.r[0], destination.data(), destination.size())) {
                             reason = "strncat could not write the guest destination"; return false;
                         }
                         return true;
                     });
    registerFunction(registry, "_strpbrk", "ndk-string-strpbrk",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string text, accept;
                         if (!readGuestStringForOutput(memory, r.r[0], text, reason) ||
                             !readGuestStringForOutput(memory, r.r[1], accept, reason)) return false;
                         const auto position = text.find_first_of(accept);
                         r.r[0] = position == std::string::npos ? 0U :
                             static_cast<std::uint32_t>(r.r[0] + position);
                         return true;
                     });
    registerFunction(registry, "_strtok", "ndk-string-strtok",
                     [this](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         GuestAddress start = r.r[0];
                         std::string delimiters;
                         if (!readGuestStringForOutput(memory, r.r[1], delimiters, reason)) return false;
                         if (start == 0) {
                             std::lock_guard<std::mutex> lock(mutex_);
                             start = strtokCursor_[&memory];
                         }
                         if (start == 0) { r.r[0] = 0; return true; }
                         std::string text;
                         if (!readGuestStringForOutput(memory, start, text, reason)) return false;
                         std::size_t begin = 0;
                         while (begin < text.size() && isDelimiter(text[begin], delimiters)) ++begin;
                         if (begin == text.size()) {
                             std::lock_guard<std::mutex> lock(mutex_); strtokCursor_[&memory] = 0;
                             r.r[0] = 0; return true;
                         }
                         std::size_t end = begin;
                         while (end < text.size() && !isDelimiter(text[end], delimiters)) ++end;
                         if (end < text.size()) {
                             const std::uint8_t zero = 0;
                             if (!writeGuest(memory, static_cast<GuestAddress>(start + end), &zero, 1)) {
                                 reason = "strtok could not terminate the guest token"; return false;
                             }
                             std::lock_guard<std::mutex> lock(mutex_);
                             strtokCursor_[&memory] = static_cast<GuestAddress>(start + end + 1);
                         } else {
                             std::lock_guard<std::mutex> lock(mutex_); strtokCursor_[&memory] = 0;
                         }
                         r.r[0] = static_cast<GuestAddress>(start + begin);
                         return true;
                     });
    registerFunction(registry, "_strerror", "ndk-string-strerror",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) {
                         const auto text = std::strerror(static_cast<int>(r.r[0]));
                         r.r[0] = allocateGuestString(memory, text ? text : "unknown error", "ndk-strerror");
                         return true;
                     });
    registerFunction(registry, "_strtol", "ndk-string-strtol",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string text;
                         if (!readGuestStringForOutput(memory, r.r[0], text, reason)) return false;
                         char *end = nullptr;
                         const auto value = std::strtol(text.c_str(), &end, static_cast<int>(r.r[2]));
                         if (r.r[1] != 0 && end != nullptr) {
                             const auto pointer = static_cast<GuestAddress>(r.r[0] + (end - text.c_str()));
                             if (!writeValue(memory, r.r[1], pointer)) { reason = "strtol could not write endptr"; return false; }
                         }
                         r.r[0] = static_cast<std::uint32_t>(value); return true;
                     });
    registerFunction(registry, "_strtoul", "ndk-string-strtoul",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string text;
                         if (!readGuestStringForOutput(memory, r.r[0], text, reason)) return false;
                         char *end = nullptr;
                         const auto value = std::strtoul(text.c_str(), &end, static_cast<int>(r.r[2]));
                         if (r.r[1] != 0 && end != nullptr) {
                             const auto pointer = static_cast<GuestAddress>(r.r[0] + (end - text.c_str()));
                             if (!writeValue(memory, r.r[1], pointer)) { reason = "strtoul could not write endptr"; return false; }
                         }
                         r.r[0] = static_cast<std::uint32_t>(value); return true;
                     });
    registerFunction(registry, "_strtod", "ndk-string-strtod",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string text;
                         if (!readGuestStringForOutput(memory, r.r[0], text, reason)) return false;
                         char *end = nullptr;
                         const auto value = std::strtod(text.c_str(), &end);
                         if (r.r[1] != 0 && end != nullptr) {
                             const auto pointer = static_cast<GuestAddress>(r.r[0] + (end - text.c_str()));
                             if (!writeValue(memory, r.r[1], pointer)) { reason = "strtod could not write endptr"; return false; }
                         }
                         setDoubleResult(r, value); return true;
                     });

    // ---- stdio and virtual-file operations --------------------------------
    registerFunction(registry, "_clearerr", "ndk-stdio-clearerr",
                     [](CpuRegisterState &, GuestAddressSpace &, std::string &) { return true; });
    registerFunction(registry, "_fputc", "ndk-stdio-fputc",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         std::uint8_t byte = static_cast<std::uint8_t>(r.r[0]); std::string detail;
                         const auto written = guestFileSystem().write(r.r[1], &byte, 1, detail);
                         r.r[0] = written == 1 ? r.r[0] : static_cast<std::uint32_t>(kEof); return true;
                     });
    registerFunction(registry, "_fputs", "ndk-stdio-fputs",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string text;
                         if (!readGuestStringForOutput(memory, r.r[0], text, reason)) return false;
                         std::string detail;
                         r.r[0] = static_cast<std::uint32_t>(guestFileSystem().write(r.r[1], text.data(), text.size(), detail) == text.size() ? 0 : kEof);
                         return true;
                     });
    registerFunction(registry, "_getc", "ndk-stdio-getc",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         std::uint8_t byte = 0; std::string detail;
                         r.r[0] = guestFileSystem().read(r.r[0], &byte, 1, detail) == 1 ? byte : static_cast<std::uint32_t>(kEof);
                         return true;
                     });
    registerFunction(registry, "_ungetc", "ndk-stdio-ungetc",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         r.r[0] = static_cast<std::uint32_t>(kEof); return true;
                     });
    registerFunction(registry, "_setvbuf", "ndk-stdio-setvbuf",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_fscanf", "ndk-stdio-fscanf-boundary",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = static_cast<std::uint32_t>(kEof); return true; });
    registerFunction(registry, "_printf", "ndk-stdio-printf-literal",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string format;
                         if (!readGuestStringForOutput(memory, r.r[0], format, reason)) return false;
                         const auto handle = guestFileSystem().standardStream(VirtualFileSystem::StandardStream::Output);
                         std::string detail; guestFileSystem().write(handle, format.data(), format.size(), detail);
                         r.r[0] = static_cast<std::uint32_t>(format.size()); return true;
                     });
    registerFunction(registry, "_fprintf", "ndk-stdio-fprintf-literal",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string format;
                         if (!readGuestStringForOutput(memory, r.r[1], format, reason)) return false;
                         std::string detail; guestFileSystem().write(r.r[0], format.data(), format.size(), detail);
                         r.r[0] = static_cast<std::uint32_t>(format.size()); return true;
                     });
    registerFunction(registry, "_sprintf", "ndk-stdio-sprintf-literal",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string format;
                         if (!readGuestStringForOutput(memory, r.r[1], format, reason)) return false;
                         if (!writeOutput(memory, r.r[0], format.size() + 1, format, reason)) return false;
                         r.r[0] = static_cast<std::uint32_t>(format.size()); return true;
                     });
    registerFunction(registry, "_vsprintf", "ndk-stdio-vsprintf-literal",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string format;
                         if (!readGuestStringForOutput(memory, r.r[1], format, reason)) return false;
                         if (!writeOutput(memory, r.r[0], format.size() + 1, format, reason)) return false;
                         r.r[0] = static_cast<std::uint32_t>(format.size()); return true;
                     });
    registerFunction(registry, "_snprintf", "ndk-stdio-snprintf-literal",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string format;
                         if (!readGuestStringForOutput(memory, r.r[2], format, reason)) return false;
                         const auto capacity = static_cast<std::size_t>(r.r[1]);
                         if (!writeOutput(memory, r.r[0], capacity, format, reason)) return false;
                         r.r[0] = static_cast<std::uint32_t>(format.size()); return true;
                     });
    registerFunction(registry, "_freopen", "ndk-stdio-freopen",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string path, mode;
                         if (!readGuestStringForOutput(memory, r.r[0], path, reason) ||
                             !readGuestStringForOutput(memory, r.r[1], mode, reason)) return false;
                         guestFileSystem().close(r.r[2]); std::string detail;
                         r.r[0] = guestFileSystem().open(path, mode, detail); return true;
                     });
    registerFunction(registry, "_tmpfile", "ndk-stdio-tmpfile-boundary",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_tmpnam", "ndk-stdio-tmpnam",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         const std::string path = "/radek-home/radek-tmp-0";
                         if (r.r[0] == 0 || !writeGuest(memory, r.r[0], path.c_str(), path.size() + 1)) {
                             reason = "tmpnam could not write the guest path"; return false;
                         }
                         return true;
                     });

    // ---- file descriptors, environment, and time --------------------------
    registerFunction(registry, "_close", "ndk-posix-close",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         if (r.r[0] <= 2) { r.r[0] = 0; return true; }
                         r.r[0] = guestFileSystem().close(r.r[0]) ? 0U : static_cast<std::uint32_t>(-1); return true;
                     });
    registerFunction(registry, "_read", "ndk-posix-read",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         constexpr std::size_t kMaximumRead = 1U * 1024U * 1024U;
                         const auto size = static_cast<std::size_t>(r.r[2]);
                         if (size > kMaximumRead) {
                             reason = "read exceeds the bounded NDK compatibility buffer";
                             return false;
                         }
                         std::vector<std::uint8_t> buffer(size);
                         std::string detail;
                         const auto count = guestFileSystem().read(r.r[0], buffer.data(), size, detail);
                         if (count > 0 && !writeGuest(memory, r.r[1], buffer.data(), count)) {
                             reason = "read could not write the guest buffer";
                             return false;
                         }
                         r.r[0] = static_cast<std::uint32_t>(count);
                         return true;
                     });
    registerFunction(registry, "_lseek", "ndk-posix-lseek",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &reason) {
                         const auto offset = static_cast<std::int32_t>(r.r[1]); const auto whence = static_cast<int>(r.r[2]);
                         if (!guestFileSystem().seek(r.r[0], offset, whence, reason)) { r.r[0] = static_cast<std::uint32_t>(-1); return true; }
                         const auto position = guestFileSystem().tell(r.r[0], reason); r.r[0] = static_cast<std::uint32_t>(position); return true;
                     });
    registerFunction(registry, "_fcntl", "ndk-posix-fcntl",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_select", "ndk-posix-select",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_sched_yield", "ndk-posix-sched-yield",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { std::this_thread::yield(); r.r[0] = 0; return true; });
    registerFunction(registry, "_usleep", "ndk-posix-usleep",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_getenv", "ndk-environment-empty",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_clock", "ndk-time-clock",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = static_cast<std::uint32_t>(std::clock()); return true; });
    registerFunction(registry, "_gmtime", "ndk-time-gmtime",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::int32_t seconds = 0;
                         if (!readValue(memory, r.r[0], seconds)) {
                             reason = "gmtime could not read 32-bit guest time_t";
                             return false;
                         }
                         const std::time_t input = seconds;
                         const auto *value = std::gmtime(&input);
                         if (value == nullptr) { r.r[0] = 0; return true; }
                         try {
                             const auto address = memory.mapAny(64, MemoryPermission::Read | MemoryPermission::Write,
                                                               "ndk-gmtime", 4);
                             if (!tmToGuest(memory, address, *value)) {
                                 reason = "gmtime could not write tm"; return false;
                             }
                             r.r[0] = address;
                             return true;
                         } catch (const std::exception &) {
                             reason = "gmtime could not allocate bounded guest tm storage";
                             return false;
                         }
                     });
    registerFunction(registry, "_localtime", "ndk-time-localtime",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::int32_t seconds = 0;
                         if (!readValue(memory, r.r[0], seconds)) {
                             reason = "localtime could not read 32-bit guest time_t";
                             return false;
                         }
                         const std::time_t input = seconds;
                         const auto *value = std::localtime(&input);
                         if (value == nullptr) { r.r[0] = 0; return true; }
                         try {
                             const auto address = memory.mapAny(64, MemoryPermission::Read | MemoryPermission::Write,
                                                               "ndk-localtime", 4);
                             if (!tmToGuest(memory, address, *value)) {
                                 reason = "localtime could not write tm"; return false;
                             }
                             r.r[0] = address;
                             return true;
                         } catch (const std::exception &) {
                             reason = "localtime could not allocate bounded guest tm storage";
                             return false;
                         }
                     });
    registerFunction(registry, "_mktime", "ndk-time-mktime",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         bool ok = false; auto value = tmFromGuest(memory, r.r[0], ok); if (!ok) { reason = "mktime could not read tm"; return false; }
                         r.r[0] = static_cast<std::uint32_t>(std::mktime(&value)); return true;
                     });
    registerFunction(registry, "_strftime", "ndk-time-strftime",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string format;
                         if (!readGuestStringForOutput(memory, r.r[2], format, reason)) return false;
                         bool ok = false;
                         const auto tm = tmFromGuest(memory, r.r[3], ok);
                         if (!ok) { reason = "strftime could not read tm"; return false; }
                         const auto capacity = static_cast<std::size_t>(r.r[1]);
                         if (capacity > 1U * 1024U * 1024U) {
                             reason = "strftime exceeds the bounded NDK compatibility buffer";
                             return false;
                         }
                         std::vector<char> output(capacity ? capacity : 1, '\0');
                         const auto count = std::strftime(output.data(), output.size(), format.c_str(), &tm);
                         if (count > 0 && !writeGuest(memory, r.r[0], output.data(), count + 1)) {
                             reason = "strftime could not write output"; return false;
                         }
                         r.r[0] = static_cast<std::uint32_t>(count);
                         return true;
                     });
    registerFunction(registry, "_setlocale", "ndk-locale-c",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         if (r.r[1] != 0) { std::string locale; if (!readGuestStringForOutput(memory, r.r[1], locale, reason)) return false; r.r[0] = r.r[1]; return true; }
                         r.r[0] = allocateGuestString(memory, "C", "ndk-locale"); return true;
                     });
    registerFunction(registry, "_localeconv", "ndk-localeconv",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         const auto empty = allocateGuestString(memory, "", "ndk-locale-empty");
                         if (empty == 0) { reason = "localeconv could not allocate empty locale strings"; return false; }
                         std::array<std::uint32_t, 18> value{};
                         for (std::size_t index = 0; index < 12; ++index) value[index] = empty;
                         try {
                             const auto address = memory.mapAny(sizeof(value), MemoryPermission::Read | MemoryPermission::Write,
                                                               "ndk-localeconv", 4);
                             if (!writeGuest(memory, address, value.data(), sizeof(value))) {
                                 reason = "localeconv could not write its bounded guest structure";
                                 return false;
                             }
                             r.r[0] = address;
                             return true;
                         } catch (const std::exception &) {
                             reason = "localeconv could not allocate bounded guest storage";
                             return false;
                         }
                     });

    // ---- deterministic random numbers -------------------------------------
    registerFunction(registry, "_srand", "ndk-random-srand",
                     [this](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         std::lock_guard<std::mutex> lock(mutex_); randomState_ = r.r[0]; return true;
                     });
    registerFunction(registry, "_rand", "ndk-random-rand",
                     [this](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         std::lock_guard<std::mutex> lock(mutex_); randomState_ = randomState_ * 1103515245U + 12345U; r.r[0] = (randomState_ >> 1) & 0x7fffffffU; return true;
                     });

    // ---- broader scalar math -----------------------------------------------
    // The fixture exercises only a subset of libm, but the reviewed NDK
    // inventory contains the complete public scalar family. These wrappers use
    // the guest VFP bank and preserve the exact host libm operation rather than
    // resolving an arm64 function pointer into the ARM32 guest.
    auto bindDoubleUnary = [this, &registry](const char *symbol, const char *adapter,
                                              std::function<double(double)> operation) {
        registerFunction(registry, symbol, adapter,
                         [operation](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                             setDoubleResult(r, operation(doubleArgument(r, 0))); return true;
                         });
    };
    auto bindFloatUnary = [this, &registry](const char *symbol, const char *adapter,
                                            std::function<float(float)> operation) {
        registerFunction(registry, symbol, adapter,
                         [operation](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                             setSingleResult(r, operation(singleArgument(r, 0))); return true;
                         });
    };
    auto bindDoubleBinary = [this, &registry](const char *symbol, const char *adapter,
                                               std::function<double(double, double)> operation) {
        registerFunction(registry, symbol, adapter,
                         [operation](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                             setDoubleResult(r, operation(doubleArgument(r, 0), doubleArgument(r, 1)));
                             return true;
                         });
    };
    auto bindFloatBinary = [this, &registry](const char *symbol, const char *adapter,
                                             std::function<float(float, float)> operation) {
        registerFunction(registry, symbol, adapter,
                         [operation](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                             setSingleResult(r, operation(singleArgument(r, 0), singleArgument(r, 1)));
                             return true;
                         });
    };
    for (const auto &entry : std::array<std::pair<const char *, std::function<double(double)>>, 30>{{
             {"_acos", static_cast<double (*)(double)>(std::acos)},
             {"_acosh", static_cast<double (*)(double)>(std::acosh)},
             {"_asin", static_cast<double (*)(double)>(std::asin)},
             {"_asinh", static_cast<double (*)(double)>(std::asinh)},
             {"_atan", static_cast<double (*)(double)>(std::atan)},
             {"_atanh", static_cast<double (*)(double)>(std::atanh)},
             {"_cbrt", static_cast<double (*)(double)>(std::cbrt)},
             {"_ceil", static_cast<double (*)(double)>(std::ceil)},
             {"_cos", static_cast<double (*)(double)>(std::cos)},
             {"_cosh", static_cast<double (*)(double)>(std::cosh)},
             {"_erf", static_cast<double (*)(double)>(std::erf)},
             {"_erfc", static_cast<double (*)(double)>(std::erfc)},
             {"_exp", static_cast<double (*)(double)>(std::exp)},
             {"_exp2", static_cast<double (*)(double)>(std::exp2)},
             {"_expm1", static_cast<double (*)(double)>(std::expm1)},
             {"_fabs", static_cast<double (*)(double)>(std::fabs)},
             {"_floor", static_cast<double (*)(double)>(std::floor)},
             {"_log", static_cast<double (*)(double)>(std::log)},
             {"_log10", static_cast<double (*)(double)>(std::log10)},
             {"_log1p", static_cast<double (*)(double)>(std::log1p)},
             {"_log2", static_cast<double (*)(double)>(std::log2)},
             {"_logb", static_cast<double (*)(double)>(std::logb)},
             {"_nearbyint", static_cast<double (*)(double)>(std::nearbyint)},
             {"_rint", static_cast<double (*)(double)>(std::rint)},
             {"_round", static_cast<double (*)(double)>(std::round)},
             {"_sin", static_cast<double (*)(double)>(std::sin)},
             {"_sqrt", static_cast<double (*)(double)>(std::sqrt)},
             {"_tan", static_cast<double (*)(double)>(std::tan)},
             {"_tanh", static_cast<double (*)(double)>(std::tanh)},
             {"_trunc", static_cast<double (*)(double)>(std::trunc)},
         }})
        bindDoubleUnary(entry.first, "ndk-libm-double", entry.second);
    for (const auto &entry : std::array<std::pair<const char *, std::function<float(float)>>, 24>{{
             {"_acosf", static_cast<float (*)(float)>(std::acos)},
             {"_acoshf", static_cast<float (*)(float)>(std::acosh)},
             {"_asinf", static_cast<float (*)(float)>(std::asin)},
             {"_asinhf", static_cast<float (*)(float)>(std::asinh)},
             {"_atanf", static_cast<float (*)(float)>(std::atan)},
             {"_atanhf", static_cast<float (*)(float)>(std::atanh)},
             {"_cbrtf", static_cast<float (*)(float)>(std::cbrt)},
             {"_ceilf", static_cast<float (*)(float)>(std::ceil)},
             {"_cosf", static_cast<float (*)(float)>(std::cos)},
             {"_coshf", static_cast<float (*)(float)>(std::cosh)},
             {"_erff", static_cast<float (*)(float)>(std::erf)},
             {"_erfcf", static_cast<float (*)(float)>(std::erfc)},
             {"_expf", static_cast<float (*)(float)>(std::exp)},
             {"_exp2f", static_cast<float (*)(float)>(std::exp2)},
             {"_expm1f", static_cast<float (*)(float)>(std::expm1)},
             {"_fabsf", static_cast<float (*)(float)>(std::fabs)},
             {"_floorf", static_cast<float (*)(float)>(std::floor)},
             {"_logf", static_cast<float (*)(float)>(std::log)},
             {"_log10f", static_cast<float (*)(float)>(std::log10)},
             {"_log1pf", static_cast<float (*)(float)>(std::log1p)},
             {"_log2f", static_cast<float (*)(float)>(std::log2)},
             {"_sinf", static_cast<float (*)(float)>(std::sin)},
             {"_sqrtf", static_cast<float (*)(float)>(std::sqrt)},
             {"_tanf", static_cast<float (*)(float)>(std::tan)},
         }})
        bindFloatUnary(entry.first, "ndk-libm-float", entry.second);
    for (const auto &entry : std::array<std::pair<const char *, std::function<double(double, double)>>, 10>{{
             {"_copysign", static_cast<double (*)(double, double)>(std::copysign)},
             {"_fdim", static_cast<double (*)(double, double)>(std::fdim)},
             {"_fmax", static_cast<double (*)(double, double)>(std::fmax)},
             {"_fmin", static_cast<double (*)(double, double)>(std::fmin)},
             {"_fmod", static_cast<double (*)(double, double)>(std::fmod)},
             {"_hypot", static_cast<double (*)(double, double)>(std::hypot)},
             {"_pow", static_cast<double (*)(double, double)>(std::pow)},
             {"_remainder", static_cast<double (*)(double, double)>(std::remainder)},
             {"_atan2", static_cast<double (*)(double, double)>(std::atan2)},
             {"_nextafter", static_cast<double (*)(double, double)>(std::nextafter)},
         }})
        bindDoubleBinary(entry.first, "ndk-libm-double-binary", entry.second);
    for (const auto &entry : std::array<std::pair<const char *, std::function<float(float, float)>>, 9>{{
             {"_copysignf", static_cast<float (*)(float, float)>(std::copysign)},
             {"_fdimf", static_cast<float (*)(float, float)>(std::fdim)},
             {"_fmaxf", static_cast<float (*)(float, float)>(std::fmax)},
             {"_fminf", static_cast<float (*)(float, float)>(std::fmin)},
             {"_fmodf", static_cast<float (*)(float, float)>(std::fmod)},
             {"_hypotf", static_cast<float (*)(float, float)>(std::hypot)},
             {"_powf", static_cast<float (*)(float, float)>(std::pow)},
             {"_remainderf", static_cast<float (*)(float, float)>(std::remainder)},
             {"_nextafterf", static_cast<float (*)(float, float)>(std::nextafter)},
         }})
        bindFloatBinary(entry.first, "ndk-libm-float-binary", entry.second);

    // ---- zlib primitives ---------------------------------------------------
    // These checksums are self-contained so the ARM32 guest does not depend on
    // an arm64 host zlib function pointer. Stateful deflate/inflate streams
    // remain explicit bounded boundaries when their opaque ABI is not mapped.
    registerFunction(registry, "_adler32", "ndk-zlib-adler32",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::vector<std::uint8_t> bytes;
                         if (!readGuestBytes(memory, r.r[1], r.r[2], bytes, reason)) return false;
                         r.r[0] = adler32(r.r[0], bytes); return true;
                     });
    registerFunction(registry, "_crc32", "ndk-zlib-crc32",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::vector<std::uint8_t> bytes;
                         if (!readGuestBytes(memory, r.r[1], r.r[2], bytes, reason)) return false;
                         r.r[0] = crc32(r.r[0], bytes); return true;
                     });
    registerFunction(registry, "_compressBound", "ndk-zlib-compress-bound",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         const auto length = static_cast<std::uint64_t>(r.r[0]);
                         r.r[0] = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                             length + length / 1000U + 13U, std::numeric_limits<std::uint32_t>::max()));
                         return true;
                     });
    registerFunction(registry, "_zlibVersion", "ndk-zlib-version",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) {
                         r.r[0] = allocateGuestString(memory, "1.2.13", "ndk-zlib-version");
                         return true;
                     });

    // ---- Android asset-manager subset -------------------------------------
    // The launcher mounts the extracted bundle in the same virtual filesystem
    // used by stdio. An AAsset handle is therefore a bounded guest file handle,
    // not a host pointer that an ARM32 guest could dereference.
    registerFunction(registry, "_AAssetManager_open", "ndk-asset-open",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         std::string relative;
                         if (!readGuestStringForOutput(memory, r.r[1], relative, reason)) return false;
                         const std::string path = relative.rfind('/', 0) == 0
                             ? relative
                             : std::string(bundleGuestPath()) + "/" + relative;
                         std::string detail;
                         r.r[0] = guestFileSystem().open(path, "rb", detail);
                         return true;
                     });
    registerFunction(registry, "_AAsset_close", "ndk-asset-close",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                         r.r[0] = guestFileSystem().close(r.r[0]) ? 0U : static_cast<std::uint32_t>(-1);
                         return true;
                     });
    registerFunction(registry, "_AAsset_read", "ndk-asset-read",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
                         const auto size = static_cast<std::size_t>(r.r[2]);
                         std::vector<std::uint8_t> bytes;
                         if (size > 16U * 1024U * 1024U) { reason = "AAsset_read exceeds the bounded buffer"; return false; }
                         bytes.resize(size);
                         std::string detail;
                         const auto count = guestFileSystem().read(r.r[0], bytes.data(), size, detail);
                         if (count != 0 && !writeGuest(memory, r.r[1], bytes.data(), count)) { reason = "AAsset_read could not write the guest buffer"; return false; }
                         r.r[0] = static_cast<std::uint32_t>(count);
                         return true;
                     });
    registerFunction(registry, "_AAsset_seek", "ndk-asset-seek",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &reason) {
                         if (!guestFileSystem().seek(r.r[0], static_cast<long>(static_cast<std::int32_t>(r.r[1])), static_cast<int>(r.r[2]), reason)) { r.r[0] = static_cast<std::uint32_t>(-1); return true; }
                         r.r[0] = static_cast<std::uint32_t>(guestFileSystem().tell(r.r[0], reason)); return true;
                     });
    registerFunction(registry, "_AAsset_getBuffer", "ndk-asset-buffer-boundary",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_AAsset_getLength", "ndk-asset-length",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &reason) {
                         std::uint64_t length = 0;
                         if (!assetLength(r.r[0], false, length, reason)) {
                             r.r[0] = static_cast<std::uint32_t>(-1);
                             return true;
                         }
                         setAssetLengthResult(r, length, false);
                         return true;
                     });
    registerFunction(registry, "_AAsset_getLength64", "ndk-asset-length64",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &reason) {
                         std::uint64_t length = 0;
                         if (!assetLength(r.r[0], false, length, reason)) {
                             r.r[0] = static_cast<std::uint32_t>(-1);
                             r.r[1] = static_cast<std::uint32_t>(-1);
                             return true;
                         }
                         setAssetLengthResult(r, length, true);
                         return true;
                     });
    registerFunction(registry, "_AAsset_getRemainingLength", "ndk-asset-remaining-length",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &reason) {
                         std::uint64_t length = 0;
                         if (!assetLength(r.r[0], true, length, reason)) {
                             r.r[0] = static_cast<std::uint32_t>(-1);
                             return true;
                         }
                         setAssetLengthResult(r, length, false);
                         return true;
                     });
    registerFunction(registry, "_AAsset_getRemainingLength64", "ndk-asset-remaining-length64",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &reason) {
                         std::uint64_t length = 0;
                         if (!assetLength(r.r[0], true, length, reason)) {
                             r.r[0] = static_cast<std::uint32_t>(-1);
                             r.r[1] = static_cast<std::uint32_t>(-1);
                             return true;
                         }
                         setAssetLengthResult(r, length, true);
                         return true;
                     });
    for (const char *symbol : {"_AAsset_openFileDescriptor", "_AAsset_openFileDescriptor64"}) {
        registerFunction(registry, symbol, "ndk-asset-file-descriptor-boundary",
                         [](CpuRegisterState &r, GuestAddressSpace &, std::string &) {
                             r.r[0] = static_cast<std::uint32_t>(-1);
                             return true;
                         });
    }

    // ---- pthread state -----------------------------------------------------
    registerFunction(registry, "_pthread_mutex_init", "ndk-pthread-mutex-init",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { const std::uint32_t zero = 0; r.r[0] = r.r[0] && writeValue(memory, r.r[0], zero) ? 0U : kErrnoInvalidArgument; return true; });
    registerFunction(registry, "_pthread_mutex_destroy", "ndk-pthread-mutex-destroy",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { const std::uint32_t zero = 0; r.r[0] = r.r[0] && writeValue(memory, r.r[0], zero) ? 0U : kErrnoInvalidArgument; return true; });
    registerFunction(registry, "_pthread_mutex_lock", "ndk-pthread-mutex-lock",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { std::uint32_t value = 0; if (!r.r[0] || !readValue(memory, r.r[0], value)) { r.r[0] = kErrnoInvalidArgument; return true; } if (value != 0) { r.r[0] = kErrnoBusy; return true; } value = 1; writeValue(memory, r.r[0], value); r.r[0] = 0; return true; });
    registerFunction(registry, "_pthread_mutex_trylock", "ndk-pthread-mutex-trylock",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { std::uint32_t value = 0; if (!r.r[0] || !readValue(memory, r.r[0], value)) { r.r[0] = kErrnoInvalidArgument; return true; } if (value != 0) { r.r[0] = kErrnoBusy; return true; } value = 1; writeValue(memory, r.r[0], value); r.r[0] = 0; return true; });
    registerFunction(registry, "_pthread_mutex_unlock", "ndk-pthread-mutex-unlock",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { const std::uint32_t zero = 0; r.r[0] = r.r[0] && writeValue(memory, r.r[0], zero) ? 0U : kErrnoInvalidArgument; return true; });
    registerFunction(registry, "_pthread_mutexattr_init", "ndk-pthread-mutexattr-init",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { const std::uint32_t zero = 0; r.r[0] = r.r[0] && writeValue(memory, r.r[0], zero) ? 0U : kErrnoInvalidArgument; return true; });
    registerFunction(registry, "_pthread_mutexattr_destroy", "ndk-pthread-mutexattr-destroy",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { const std::uint32_t zero = 0; r.r[0] = r.r[0] && writeValue(memory, r.r[0], zero) ? 0U : kErrnoInvalidArgument; return true; });
    registerFunction(registry, "_pthread_mutexattr_settype", "ndk-pthread-mutexattr-settype",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_pthread_create", "ndk-pthread-create-boundary",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { if (r.r[0]) { const std::uint32_t zero = 0; writeValue(memory, r.r[0], zero); } r.r[0] = kErrnoNotImplemented; return true; });
    registerFunction(registry, "_pthread_join", "ndk-pthread-join-boundary",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = kErrnoNoSuchProcess; return true; });
    registerFunction(registry, "_pthread_getschedparam", "ndk-pthread-getschedparam",
                     [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &) { if (r.r[1]) { const std::uint32_t zero = 0; writeValue(memory, r.r[1], zero); } if (r.r[2]) { const std::uint32_t zero = 0; writeValue(memory, r.r[2], zero); } r.r[0] = 0; return true; });
    registerFunction(registry, "_pthread_setschedparam", "ndk-pthread-setschedparam",
                     [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });

    // ---- remaining small C/POSIX entry points ------------------------------
    registerFunction(registry, "_fcntl", "ndk-fcntl", [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = 0; return true; });
    registerFunction(registry, "_rename", "ndk-rename", [](CpuRegisterState &r, GuestAddressSpace &memory, std::string &reason) {
        std::string from, to; if (!readGuestStringForOutput(memory, r.r[0], from, reason) || !readGuestStringForOutput(memory, r.r[1], to, reason)) return false;
        std::string source, target; bool sw = false, tw = false;
        if (!guestFileSystem().resolve(from, source, sw) || !guestFileSystem().resolve(to, target, tw) || !sw || !tw) { r.r[0] = static_cast<std::uint32_t>(-1); return true; }
        r.r[0] = std::rename(source.c_str(), target.c_str()) == 0 ? 0U : static_cast<std::uint32_t>(-1); return true;
    });
    registerFunction(registry, "_system", "ndk-system-disabled", [](CpuRegisterState &r, GuestAddressSpace &, std::string &) { r.r[0] = static_cast<std::uint32_t>(-1); return true; });

    // Cover the complete reviewed NDK candidate inventory, not only the
    // currently observed 181-symbol fixture. Typed adapters above win for
    // known signatures; the remaining entries receive an explicitly named
    // bounded ABI provider so they cannot become an accidental unresolved
    // import or a fake same-name export claim.
    std::size_t fullProviderIndex = 0;
    try {
        for (const auto &provider : ndk_full_import_catalog::kProviders) {
            registerGenericCandidate(registry, provider.symbol, provider.family);
            ++fullProviderIndex;
        }
    } catch (const std::exception &error) {
        throw std::logic_error("full NDK provider index " + std::to_string(fullProviderIndex) +
                               " failed: " + error.what());
    }

    // The catalogs are intentionally compiled into the adapter's test surface;
    // these assertions catch drift between the analyzer inventory and runtime.
    if (ndk_import_catalog::kProviderCount != 181 ||
        ndk_full_import_catalog::kProviderCount != 1229)
        throw std::logic_error("NDK provider catalog count drifted");

    std::size_t genericProviders = 0;
    std::size_t typedProviders = 0;
    for (const auto &provider : ndk_full_import_catalog::kProviders) {
        const auto binding = registry.resolve(provider.symbol);
        if (!binding.has_value())
            throw std::logic_error("NDK provider registration unexpectedly disappeared");
        if (binding->adapterName.rfind("ndk-bounded-", 0) == 0)
            ++genericProviders;
        else
            ++typedProviders;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        genericProviders_ = genericProviders;
        typedProviders_ = typedProviders;
    }
}

std::uint64_t ShimAdapter::callCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
}

std::uint64_t ShimAdapter::genericCallCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return genericCalls_;
}

std::size_t ShimAdapter::genericProviderCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return genericProviders_;
}

std::size_t ShimAdapter::typedProviderCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return typedProviders_;
}

std::size_t ShimAdapter::registeredCalloutCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(nextCallout_ - 0xf00c0000U) / 4U;
}

} // namespace radek::compat_runtime::ndk
