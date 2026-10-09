#include "compat_runtime/objc_shims.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace radek::compat_runtime::objc {
namespace {
constexpr std::size_t kGuestPageSize = 4096;
constexpr std::size_t kClassNameOffset = 64;
constexpr std::size_t kMaximumGuestString = 4096;
constexpr std::size_t kMaximumImageClasses = 4096;
constexpr std::size_t kMaximumImageMethods = 65535;
constexpr std::size_t kMaximumImageIvars = 4096;
constexpr std::uint32_t kObjectiveCDataMask32 = 0xfffffffcU;
// Bounded virtual view surface used by the startup chain for CGRect returns.
// It is a deterministic diagnostic value; nothing renders on it.
constexpr float kVirtualViewWidth = 320.0f;
constexpr float kVirtualViewHeight = 480.0f;
// Height `-[UIScreen applicationFrame]` subtracts for the status bar on the
// 3.x-era devices these games target.
constexpr float kStatusBarHeight = 20.0f;
constexpr std::uint32_t kSmallMethodListFlag = 0x80000000U;

struct GuestClass32 {
    std::uint32_t isa = 0;
    std::uint32_t superclass = 0;
    std::uint32_t name = 0;
    std::uint32_t version = 0;
    std::uint32_t info = 0;
    std::uint32_t instanceSize = 0;
    std::uint32_t ivars = 0;
    std::uint32_t methodLists = 0;
    std::uint32_t cache = 0;
    std::uint32_t protocols = 0;
};

void writeWord(GuestAddressSpace &memory, GuestAddress address, std::uint32_t value) {
    if (!memory.write(address, &value, sizeof(value)))
        throw std::runtime_error("Objective-C adapter could not write guest memory");
}

bool readWord(const GuestAddressSpace &memory, GuestAddress address, std::uint32_t &value) {
    return memory.read(address, &value, sizeof(value));
}

std::size_t utf16Length(const std::string &value) {
    std::size_t units = 0;
    for (std::size_t offset = 0; offset < value.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        if (byte < 0x80U || (byte & 0xc0U) == 0x80U) {
            if ((byte & 0xc0U) != 0x80U)
                ++units;
        } else if ((byte & 0xe0U) == 0xc0U || (byte & 0xf0U) == 0xe0U) {
            ++units;
        } else if ((byte & 0xf8U) == 0xf0U) {
            units += 2;
        } else {
            ++units;
        }
    }
    return units;
}

std::string readMetadataString(const GuestAddressSpace &memory, GuestAddress address) {
    if (address == 0)
        throw std::runtime_error("Objective-C metadata contains a null string pointer");
    std::string value;
    value.reserve(32);
    for (std::size_t offset = 0; offset < kMaximumGuestString; ++offset) {
        const auto current = static_cast<std::uint64_t>(address) + offset;
        if (current > std::numeric_limits<GuestAddress>::max())
            throw std::runtime_error("Objective-C metadata string address overflowed");
        char character = 0;
        if (!memory.read(static_cast<GuestAddress>(current), &character, sizeof(character)))
            throw std::runtime_error("Objective-C metadata string is outside readable guest memory");
        if (character == '\0')
            return value;
        value.push_back(character);
    }
    throw std::runtime_error("Objective-C metadata string exceeds the adapter limit");
}

struct GuestMethodMetadata {
    std::string selector;
    std::string types;
    GuestAddress implementation = 0;
};

struct GuestClassMetadata {
    GuestAddress address = 0;
    GuestAddress metaclassAddress = 0;
    GuestAddress superclassAddress = 0;
    GuestAddress instanceMethods = 0;
    GuestAddress classMethods = 0;
    GuestAddress ivars = 0;
    std::string name;
    std::uint32_t instanceSize = 0;
    std::vector<std::string> ivarNames;
    std::vector<std::string> protocols;
};

struct GuestProtocolMetadata {
    GuestAddress address = 0;
    GuestAddress inheritedProtocols = 0;
    std::string name;
};

GuestProtocolMetadata readProtocolMetadata(const GuestAddressSpace &memory,
                                           GuestAddress address) {
    if (address == 0 || (address & 3U) != 0 ||
        address > std::numeric_limits<GuestAddress>::max() - 3U * sizeof(std::uint32_t) ||
        !memory.contains(address, 3U * sizeof(std::uint32_t), MemoryPermission::Read))
        throw std::runtime_error("Objective-C protocol object is invalid or outside readable guest memory");
    std::uint32_t nameAddress = 0;
    if (!readWord(memory, address + sizeof(std::uint32_t), nameAddress))
        throw std::runtime_error("Objective-C protocol name pointer is unreadable");
    GuestProtocolMetadata protocol;
    protocol.address = address;
    if (!readWord(memory, address + 2U * sizeof(std::uint32_t), protocol.inheritedProtocols))
        throw std::runtime_error("Objective-C protocol inheritance list pointer is unreadable");
    protocol.name = readMetadataString(memory, nameAddress);
    if (protocol.name.empty())
        throw std::runtime_error("Objective-C protocol name is empty");
    return protocol;
}

void readClassObject(const GuestAddressSpace &memory, GuestAddress address,
                     std::array<std::uint32_t, 5> &fields) {
    if (address == 0 || (address & 3U) != 0 ||
        !memory.read(address, fields.data(), sizeof(fields)))
        throw std::runtime_error("Objective-C class object is invalid or outside mapped guest memory");
}

void readClassReadOnlyData(const GuestAddressSpace &memory, GuestAddress classAddress,
                           std::array<std::uint32_t, 10> &fields) {
    std::array<std::uint32_t, 5> classFields{};
    readClassObject(memory, classAddress, classFields);
    const auto readOnlyAddress = classFields[4] & kObjectiveCDataMask32;
    if (readOnlyAddress == 0 || !memory.read(readOnlyAddress, fields.data(), sizeof(fields)))
        throw std::runtime_error("Objective-C class has unreadable 32-bit class_ro_t metadata");
}

std::vector<GuestMethodMetadata> readMethodList(const GuestAddressSpace &memory,
                                                GuestAddress address) {
    if (address == 0)
        return {};
    std::array<std::uint32_t, 2> header{};
    if (!memory.read(address, header.data(), sizeof(header)))
        throw std::runtime_error("Objective-C method-list header is outside readable guest memory");
    const auto entrySizeAndFlags = header[0];
    const auto count = header[1];
    if ((entrySizeAndFlags & kSmallMethodListFlag) != 0)
        throw std::runtime_error("relative Objective-C method lists are unsupported by the 32-bit runtime adapter");
    auto stride = entrySizeAndFlags & 0xfffcU;
    if (stride == 0)
        stride = 3U * sizeof(std::uint32_t);
    if (stride < 3U * sizeof(std::uint32_t) || stride > 128 ||
        count > kMaximumImageMethods ||
        static_cast<std::uint64_t>(count) * stride > 16U * 1024U * 1024U)
        throw std::runtime_error("Objective-C method-list size exceeds the supported ABI bounds");

    std::vector<GuestMethodMetadata> methods;
    methods.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto entryAddress = static_cast<std::uint64_t>(address) + sizeof(header) +
                                  static_cast<std::uint64_t>(index) * stride;
        if (entryAddress > std::numeric_limits<GuestAddress>::max())
            throw std::runtime_error("Objective-C method-list address overflowed");
        std::array<std::uint32_t, 3> entry{};
        if (!memory.read(static_cast<GuestAddress>(entryAddress), entry.data(), sizeof(entry)))
            throw std::runtime_error("Objective-C method entry is outside readable guest memory");
        GuestMethodMetadata method;
        method.selector = readMetadataString(memory, entry[0]);
        if (method.selector.empty())
            throw std::runtime_error("Objective-C method selector is empty");
        if (entry[1] != 0)
            method.types = readMetadataString(memory, entry[1]);
        method.implementation = entry[2];
        const auto codeAddress = method.implementation & ~GuestAddress{1};
        if (method.implementation == 0 ||
            !memory.contains(codeAddress, 2, MemoryPermission::Execute))
            throw std::runtime_error("Objective-C method IMP is outside executable guest memory");
        methods.push_back(std::move(method));
    }
    return methods;
}

std::vector<std::string> readIvarNames(const GuestAddressSpace &memory,
                                       GuestAddress address, std::uint32_t instanceSize) {
    if (address == 0)
        return {};
    std::array<std::uint32_t, 2> header{};
    if (!memory.read(address, header.data(), sizeof(header)))
        throw std::runtime_error("Objective-C ivar-list header is outside readable guest memory");
    auto stride = header[0] & 0xfffcU;
    if (stride == 0)
        stride = 3U * sizeof(std::uint32_t) + 2U * sizeof(std::uint32_t);
    const auto count = header[1];
    constexpr std::uint32_t minimumEntrySize = 3U * sizeof(std::uint32_t) +
                                                2U * sizeof(std::uint32_t);
    if (stride < minimumEntrySize || stride > 128 || count > kMaximumImageIvars ||
        static_cast<std::uint64_t>(count) * stride > 4U * 1024U * 1024U)
        throw std::runtime_error("Objective-C ivar-list size exceeds the supported ABI bounds");

    std::vector<std::string> names;
    names.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto entryAddress = static_cast<std::uint64_t>(address) + sizeof(header) +
                                  static_cast<std::uint64_t>(index) * stride;
        if (entryAddress > std::numeric_limits<GuestAddress>::max())
            throw std::runtime_error("Objective-C ivar-list address overflowed");
        std::array<std::uint32_t, 5> entry{};
        if (!memory.read(static_cast<GuestAddress>(entryAddress), entry.data(), sizeof(entry)))
            throw std::runtime_error("Objective-C ivar entry is outside readable guest memory");
        const auto name = readMetadataString(memory, entry[1]);
        if (name.empty())
            throw std::runtime_error("Objective-C ivar name is empty");
        if (entry[0] != 0) {
            std::uint32_t offset = 0;
            if (!readWord(memory, entry[0], offset) || offset < sizeof(std::uint32_t) ||
                offset > instanceSize || entry[4] > instanceSize - offset)
                throw std::runtime_error("Objective-C ivar offset or size exceeds its class instance");
        }
        names.push_back(name);
    }
    return names;
}
std::string hexWord(std::uint32_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4)
        text.push_back(digits[(value >> shift) & 0xf]);
    return text;
}

} // namespace

ShimAdapter::ShimAdapter() {
    rootClass_ = runtime_.registerClass("NSObject");
    autoreleasePoolClass_ = runtime_.registerClass("NSAutoreleasePool", rootClass_);
    classes_.emplace("NSObject", rootClass_);
    classes_.emplace("NSAutoreleasePool", autoreleasePoolClass_);
    for (const auto *name : {
             "NSString", "NSArray", "NSDictionary", "NSNumber", "NSURL", "NSBundle", "NSThread",
             "UIWindow", "UIView", "UIScreen", "UIAccelerometer", "CAEAGLLayer",
             "EAGLContext", "NSTimer"}) {
        classes_.emplace(name, runtime_.registerClass(name, rootClass_));
    }
    // The lifecycle adapters wire the delegate through the singleton's first
    // instance slot, so UIApplication declares it explicitly.
    classes_.emplace("UIApplication",
                     runtime_.registerClass("UIApplication", rootClass_, {"delegate"}));
    auto *touchClass = runtime_.registerClass("UITouch", rootClass_, {"locationX", "locationY"});
    classes_.emplace("UITouch", touchClass);

    const auto selector = [this](const std::string &name) {
        const auto value = runtime_.selector(name);
        selectorNames_[value] = name;
        selectorIds_[name] = value;
        return value;
    };
    const auto newSelector = selector("new");
    const auto allocSelector = selector("alloc");
    const auto initSelector = selector("init");
    const auto classSelector = selector("class");
    const auto locationInViewSelector = selector("locationInView:");
    const auto copySelector = selector("copy");
    runtime_.addMethod(classes_.at("NSString"), copySelector,
                       [](const Receiver &receiver, const Arguments &) -> Value {
                           return reinterpret_cast<Value>(retain(receiver.object));
                       });
    runtime_.addMethod(classes_.at("NSArray"), copySelector,
                       [this](const Receiver &receiver, const Arguments &) -> Value {
                           auto *copy = runtime_.allocate(receiver.object->isa);
                           try {
                               copy->arrayItems.reserve(receiver.object->arrayItems.size());
                               for (auto *item : receiver.object->arrayItems) {
                                   retain(item);
                                   copy->arrayItems.push_back(item);
                               }
                           } catch (...) {
                               for (auto *item : copy->arrayItems)
                                   release(item);
                               release(copy);
                               throw;
                           }
                           return reinterpret_cast<Value>(copy);
                       });

    runtime_.addMethod(touchClass, locationInViewSelector,
                       [](const Receiver &receiver, const Arguments &) -> Value {
                           if (!receiver.object || receiver.object->ivars.size() < 2)
                               throw std::runtime_error("UITouch location is unavailable");
                           const auto x = static_cast<std::uint32_t>(receiver.object->ivars[0]);
                           const auto y = static_cast<std::uint32_t>(receiver.object->ivars[1]);
                           const auto point = static_cast<std::uint64_t>(x) |
                                              (static_cast<std::uint64_t>(y) << 32);
                           return static_cast<Value>(point);
                       });

    runtime_.addMethod(rootClass_->metaclass, allocSelector,
                       [this](const Receiver &receiver, const Arguments &) -> Value {
                           if (!receiver.isClassMethod || !receiver.classObject)
                               throw std::runtime_error("Objective-C +alloc requires a class receiver");
                           return reinterpret_cast<Value>(runtime_.allocate(receiver.classObject));
                       });
    runtime_.addMethod(rootClass_->metaclass, newSelector,
                       [this, initSelector](const Receiver &receiver, const Arguments &) -> Value {
                           if (!receiver.isClassMethod || !receiver.classObject)
                               throw std::runtime_error("Objective-C +new requires a class receiver");
                           auto *object = runtime_.allocate(receiver.classObject);
                           return runtime_.send(object, initSelector);
                       });
    runtime_.addMethod(rootClass_, initSelector,
                       [](const Receiver &receiver, const Arguments &) -> Value {
                           return reinterpret_cast<Value>(receiver.object);
                       });
    runtime_.addMethod(rootClass_, classSelector,
                       [](const Receiver &receiver, const Arguments &) -> Value {
                           return reinterpret_cast<Value>(receiver.object ? receiver.object->isa : nullptr);
                       });
}

ShimAdapter::~ShimAdapter() = default;

ShimAdapter::GuestState &ShimAdapter::guestState(GuestAddressSpace &memory) {
    const auto found = guestStates_.find(&memory);
    if (found != guestStates_.end())
        return *found->second;
    auto state = std::make_unique<GuestState>();
    auto *pointer = state.get();
    guestStates_.emplace(&memory, std::move(state));
    return *pointer;
}

GuestAddress ShimAdapter::ensureClassAddress(GuestAddressSpace &memory, const Class *klass) {
    if (!klass)
        return 0;
    auto &state = guestState(memory);
    if (const auto found = state.classAddresses.find(klass); found != state.classAddresses.end())
        return found->second;

    const auto address = memory.mapAny(kGuestPageSize,
        MemoryPermission::Read | MemoryPermission::Write,
        klass->isMetaclass ? "objc-metaclass:" + klass->name : "objc-class:" + klass->name);
    state.classAddresses.emplace(klass, address);
    state.classesByAddress.emplace(address, klass);

    GuestClass32 guestClass;
    guestClass.isa = ensureClassAddress(memory, klass->metaclass);
    guestClass.superclass = ensureClassAddress(memory, klass->superclass);
    guestClass.name = address + static_cast<GuestAddress>(kClassNameOffset);
    const std::uint64_t instanceSize = klass->isMetaclass
        ? sizeof(GuestClass32)
        : sizeof(std::uint32_t) * (1 + klass->instanceSlots);
    if (instanceSize > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("Objective-C class instance size exceeds the guest ABI");
    guestClass.instanceSize = static_cast<std::uint32_t>(instanceSize);
    if (!memory.write(address, &guestClass, sizeof(guestClass)))
        throw std::runtime_error("Objective-C adapter could not initialize a guest class object");
    const auto nameCapacity = kGuestPageSize - kClassNameOffset;
    if (klass->name.empty() || klass->name.size() >= nameCapacity ||
        !memory.write(guestClass.name, klass->name.c_str(), klass->name.size() + 1))
        throw std::runtime_error("Objective-C class name cannot be represented in guest memory");
    return address;
}

GuestAddress ShimAdapter::ensureObjectAddress(GuestAddressSpace &memory, Object *object) {
    if (!object)
        return 0;
    auto &state = guestState(memory);
    if (const auto found = state.objectAddresses.find(object); found != state.objectAddresses.end())
        return found->second;
    if (object->ivars.size() > (kGuestPageSize - sizeof(std::uint32_t)) / sizeof(std::uint32_t))
        throw std::length_error("Objective-C instance has too many guest ivars");
    const auto address = memory.mapAny(kGuestPageSize,
        MemoryPermission::Read | MemoryPermission::Write, "objc-instance:" + object->isa->name);
    state.objectAddresses.emplace(object, address);
    state.objectsByAddress.emplace(address, object);
    const auto classAddress = ensureClassAddress(memory, object->isa);
    writeWord(memory, address, classAddress);
    synchronizeObject(memory, object, address);
    return address;
}

std::string ShimAdapter::readGuestString(const GuestAddressSpace &memory,
                                         GuestAddress address) const {
    if (address == 0)
        throw std::runtime_error("Objective-C string argument is null");
    std::string result;
    for (std::size_t offset = 0; offset < kMaximumGuestString; ++offset) {
        const auto current = static_cast<std::uint64_t>(address) + offset;
        if (current > std::numeric_limits<GuestAddress>::max())
            throw std::runtime_error("Objective-C guest string address overflowed");
        char character = 0;
        if (!memory.read(static_cast<GuestAddress>(current), &character, sizeof(character)))
            throw std::runtime_error("Objective-C guest string is outside readable memory");
        if (character == '\0')
            return result;
        result.push_back(character);
    }
    throw std::runtime_error("Objective-C guest string exceeds the adapter limit");
}

Selector ShimAdapter::selectorForGuest(GuestAddressSpace &memory, GuestAddress value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto found = selectorNames_.find(value); found != selectorNames_.end())
            return value;
    }
    const auto name = readGuestString(memory, value);
    if (name.empty())
        throw std::runtime_error("Objective-C selector name is empty");
    const auto selectorValue = runtime_.selector(name);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selectorNames_[selectorValue] = name;
        selectorIds_[name] = selectorValue;
    }
    return selectorValue;
}

GuestAddress ShimAdapter::selectorStringAddress(GuestAddressSpace &memory, Selector selectorValue) {
    auto &state = guestState(memory);
    if (const auto found = state.selectorAddresses.find(selectorValue);
        found != state.selectorAddresses.end())
        return found->second;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = selectorNames_.find(selectorValue);
        if (found == selectorNames_.end())
            throw std::runtime_error("Objective-C selector is not registered");
        name = found->second;
    }
    const auto address = memory.mapAny(kGuestPageSize,
        MemoryPermission::Read | MemoryPermission::Write, "objc-selector-name");
    if (name.empty() || name.size() >= kGuestPageSize ||
        !memory.write(address, name.c_str(), name.size() + 1))
        throw std::runtime_error("Objective-C selector name cannot be represented in guest memory");
    state.selectorAddresses.emplace(selectorValue, address);
    return address;
}

GuestAddress ShimAdapter::emptyDataAddress(GuestAddressSpace &memory) {
    const auto found = emptyDataAddresses_.find(&memory);
    if (found != emptyDataAddresses_.end())
        return found->second;
    const auto address = memory.mapAny(kGuestPageSize,
        MemoryPermission::Read | MemoryPermission::Write, "objc-empty-cache-vtable");
    emptyDataAddresses_.emplace(&memory, address);
    return address;
}

bool ShimAdapter::registerFunction(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)> invoke) {
    if (nextCallout_ > 0xf000ffffU - 4)
        throw std::overflow_error("Objective-C shim callout range is exhausted");
    ShimBinding binding{symbol, "libobjc.A.dylib", adapterName, nextCallout_++,
                        std::move(invoke), {}, {}, {}};
    nextCallout_ = (nextCallout_ + 3U) & ~std::uint32_t{3};
    registry.registerBinding(std::move(binding));
    return true;
}

bool ShimAdapter::registerExceptionFunction(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)> invoke) {
    if (nextCallout_ > 0xf000ffffU - 4)
        throw std::overflow_error("Objective-C shim callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libobjc.A.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_++;
    binding.invokeException = std::move(invoke);
    nextCallout_ = (nextCallout_ + 3U) & ~std::uint32_t{3};
    registry.registerBinding(std::move(binding));
    return true;
}

bool ShimAdapter::registerTransferFunction(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, GuestAddress &,
                       std::string &)> invoke) {
    if (nextCallout_ > 0xf000ffffU - 4)
        throw std::overflow_error("Objective-C shim callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libobjc.A.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_++;
    binding.invokeTransfer = std::move(invoke);
    nextCallout_ = (nextCallout_ + 3U) & ~std::uint32_t{3};
    registry.registerBinding(std::move(binding));
    return true;
}

void ShimAdapter::registerClassSymbols(ShimRegistry &registry) {
    for (const auto &[name, klass] : classes_) {
        const auto registerData = [this, &registry](const std::string &symbol, const Class *value) {
            ShimBinding binding{symbol, "libobjc.A.dylib", "objc-class-data", 0, {}, {}, {}, {}};
            binding.resolveGuestAddress = [this, value](GuestAddressSpace &memory,
                                                        GuestAddress &address,
                                                        std::string &reason) {
                try {
                    address = ensureClassAddress(memory, value);
                    return address != 0;
                } catch (const std::exception &error) {
                    reason = error.what();
                    return false;
                }
            };
            registry.registerBinding(std::move(binding));
        };
        registerData("_OBJC_CLASS_$_" + name, klass);
        registerData("_OBJC_METACLASS_$_" + name, klass->metaclass);
    }
}

void ShimAdapter::registerEmptyDataSymbol(ShimRegistry &registry, const std::string &symbol) {
    ShimBinding binding{symbol, "libobjc.A.dylib", "objc-empty-data", 0, {}, {}, {}, {}};
    binding.resolveGuestAddress = [this](GuestAddressSpace &memory, GuestAddress &address,
                                         std::string &reason) {
        try {
            address = emptyDataAddress(memory);
            return address != 0;
        } catch (const std::exception &error) {
            reason = error.what();
            return false;
        }
    };
    registry.registerBinding(std::move(binding));
}

bool ShimAdapter::initializeImage(GuestAddressSpace &memory,
                                  const std::vector<GuestImageSection> &sections,
                                  std::string &reason) {
    try {
        auto &guestStateForImage = guestState(memory);
        std::set<GuestAddress> protocolStack;
        std::set<GuestAddress> parsedProtocols;
        std::function<std::string(GuestAddress)> registerProtocol;
        std::function<std::vector<std::string>(GuestAddress)> readProtocolList;
        readProtocolList = [&](GuestAddress listAddress) {
            std::vector<std::string> names;
            if (listAddress == 0)
                return names;
            std::uint32_t count = 0;
            if ((listAddress & 3U) != 0 || !readWord(memory, listAddress, count) ||
                count > kMaximumImageClasses ||
                static_cast<std::uint64_t>(count) * sizeof(GuestAddress) + sizeof(count) >
                    16U * 1024U * 1024U ||
                !memory.contains(listAddress,
                                 sizeof(count) + static_cast<std::size_t>(count) * sizeof(GuestAddress),
                                 MemoryPermission::Read))
                throw std::runtime_error("Objective-C protocol list is invalid or exceeds runtime limits");
            names.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto slot64 = static_cast<std::uint64_t>(listAddress) + sizeof(count) +
                                    static_cast<std::uint64_t>(index) * sizeof(GuestAddress);
                if (slot64 > std::numeric_limits<GuestAddress>::max())
                    throw std::runtime_error("Objective-C protocol-list address overflowed");
                GuestAddress protocolAddress = 0;
                if (!readWord(memory, static_cast<GuestAddress>(slot64), protocolAddress))
                    throw std::runtime_error("Objective-C protocol-list entry is unreadable");
                if (protocolAddress == 0)
                    continue;
                names.push_back(registerProtocol(protocolAddress));
            }
            return names;
        };
        registerProtocol = [&](GuestAddress protocolAddress) {
            if (protocolStack.count(protocolAddress) != 0)
                throw std::runtime_error("Objective-C protocol inheritance contains a cycle");
            if (parsedProtocols.count(protocolAddress) != 0)
                return guestStateForImage.protocolNames.at(protocolAddress);
            const auto protocol = readProtocolMetadata(memory, protocolAddress);
            const auto [addressEntry, insertedAddress] = guestStateForImage.protocolNames.emplace(
                protocol.address, protocol.name);
            if (!insertedAddress && addressEntry->second != protocol.name)
                throw std::runtime_error("Objective-C protocol address resolves to conflicting names");
            const auto [nameEntry, insertedName] = guestStateForImage.protocolAddresses.emplace(
                protocol.name, protocol.address);
            if (!insertedName && nameEntry->second != protocol.address)
                throw std::runtime_error("Objective-C protocol name has multiple guest objects");
            protocolStack.insert(protocolAddress);
            const auto parents = readProtocolList(protocol.inheritedProtocols);
            protocolStack.erase(protocolAddress);
            const auto [parentEntry, insertedParents] = guestStateForImage.protocolParents.emplace(
                protocol.name, parents);
            if (!insertedParents && parentEntry->second != parents)
                throw std::runtime_error("Objective-C protocol has conflicting inherited protocol lists");
            parsedProtocols.insert(protocolAddress);
            return protocol.name;
        };
        for (const auto &section : sections) {
            if (section.name != "__objc_protolist")
                continue;
            if ((section.size % sizeof(GuestAddress)) != 0 ||
                section.size / sizeof(GuestAddress) > kMaximumImageClasses)
                throw std::runtime_error("Objective-C protocol section has an invalid pointer count");
            for (std::uint32_t offset = 0; offset < section.size; offset += sizeof(GuestAddress)) {
                const auto slot64 = static_cast<std::uint64_t>(section.address) + offset;
                if (slot64 > std::numeric_limits<GuestAddress>::max())
                    throw std::runtime_error("Objective-C protocol-section address overflowed");
                GuestAddress protocolAddress = 0;
                if (!readWord(memory, static_cast<GuestAddress>(slot64), protocolAddress))
                    throw std::runtime_error("Objective-C protocol-section entry is unreadable");
                if (protocolAddress != 0)
                    (void)registerProtocol(protocolAddress);
            }
        }

        std::vector<GuestAddress> classAddresses;
        for (const auto &section : sections) {
            if (section.name != "__objc_classlist")
                continue;
            if ((section.size % sizeof(GuestAddress)) != 0)
                throw std::runtime_error("Objective-C class list has a partial 32-bit pointer");
            const auto count = section.size / sizeof(GuestAddress);
            if (count > kMaximumImageClasses - classAddresses.size())
                throw std::runtime_error("Objective-C class count exceeds the runtime adapter limit");
            for (std::uint32_t offset = 0; offset < section.size; offset += sizeof(GuestAddress)) {
                const auto slot64 = static_cast<std::uint64_t>(section.address) + offset;
                if (slot64 > std::numeric_limits<GuestAddress>::max())
                    throw std::runtime_error("Objective-C class-list address overflowed");
                GuestAddress address = 0;
                if (!readWord(memory, static_cast<GuestAddress>(slot64), address))
                    throw std::runtime_error("Objective-C class-list entry is unreadable");
                if (address != 0)
                    classAddresses.push_back(address);
            }
        }
        std::vector<GuestClassMetadata> pending;
        pending.reserve(classAddresses.size());
        std::set<GuestAddress> uniqueAddresses;
        std::set<std::string> uniqueNames;
        for (const auto address : classAddresses) {
            if (!uniqueAddresses.insert(address).second)
                throw std::runtime_error("Objective-C class list contains a duplicate class pointer");
            std::array<std::uint32_t, 5> classFields{};
            readClassObject(memory, address, classFields);
            GuestClassMetadata metadata;
            metadata.address = address;
            metadata.metaclassAddress = classFields[0] & ~GuestAddress{3};
            metadata.superclassAddress = classFields[1] & ~GuestAddress{3};
            std::array<std::uint32_t, 10> classRO{};
            readClassReadOnlyData(memory, address, classRO);
            metadata.instanceSize = classRO[2];
            metadata.name = readMetadataString(memory, classRO[4]);
            metadata.instanceMethods = classRO[5];
            metadata.protocols = readProtocolList(classRO[6]);
            metadata.ivars = classRO[7];
            metadata.ivarNames = readIvarNames(memory, metadata.ivars, metadata.instanceSize);
            std::array<std::uint32_t, 10> metaclassRO{};
            readClassReadOnlyData(memory, metadata.metaclassAddress, metaclassRO);
            metadata.classMethods = metaclassRO[5];
            if (metadata.name.empty() || !uniqueNames.insert(metadata.name).second)
                throw std::runtime_error("Objective-C class list contains an empty or duplicate name");
            pending.push_back(std::move(metadata));
        }

        auto registerMethods = [this, &memory](const GuestClassMetadata &metadata,
                                               Class *targetClass,
                                               GuestAddress methodListAddress) {
            for (const auto &method : readMethodList(memory, methodListAddress)) {
                const auto selectorValue = runtime_.selector(method.selector);
                selectorNames_[selectorValue] = method.selector;
                selectorIds_[method.selector] = selectorValue;
                guestImplementations_[{targetClass, selectorValue}] =
                    GuestImplementation{method.implementation, method.types};
            }
            (void)metadata;
        };

        while (!pending.empty()) {
            bool progress = false;
            for (auto current = pending.begin(); current != pending.end();) {
                Class *superclass = current->superclassAddress == 0
                    ? nullptr
                    : const_cast<Class *>(classForGuest(memory, current->superclassAddress));
                if (current->superclassAddress != 0 && !superclass) {
                    ++current;
                    continue;
                }
                if (classes_.count(current->name) != 0)
                    throw std::runtime_error("app Objective-C class collides with a registered framework class: " +
                                             current->name);

                auto *klass = runtime_.registerClassWithInstanceSize(
                    current->name, superclass, current->ivarNames, current->instanceSize);
                klass->protocols.insert(current->protocols.begin(), current->protocols.end());
                classes_.emplace(current->name, klass);
                auto &state = guestState(memory);
                if (state.classAddresses.count(klass) != 0 ||
                    state.classesByAddress.count(current->address) != 0 ||
                    state.classAddresses.count(klass->metaclass) != 0 ||
                    state.classesByAddress.count(current->metaclassAddress) != 0)
                    throw std::runtime_error("Objective-C app class address conflicts with a registered class");
                state.classAddresses.emplace(klass, current->address);
                state.classesByAddress.emplace(current->address, klass);
                state.classAddresses.emplace(klass->metaclass, current->metaclassAddress);
                state.classesByAddress.emplace(current->metaclassAddress, klass->metaclass);

                registerMethods(*current, klass, current->instanceMethods);
                registerMethods(*current, klass->metaclass, current->classMethods);
                current = pending.erase(current);
                progress = true;
            }
            if (!progress)
                throw std::runtime_error("Objective-C class list has an unresolved or cyclic superclass reference");
        }

        for (const auto &section : sections) {
            if (section.name != "__objc_catlist")
                continue;
            if ((section.size % sizeof(GuestAddress)) != 0)
                throw std::runtime_error("Objective-C category list has a partial 32-bit pointer");
            const auto count = section.size / sizeof(GuestAddress);
            if (count > kMaximumImageClasses)
                throw std::runtime_error("Objective-C category count exceeds the runtime adapter limit");
            for (std::uint32_t offset = 0; offset < section.size; offset += sizeof(GuestAddress)) {
                const auto categorySlot64 = static_cast<std::uint64_t>(section.address) + offset;
                if (categorySlot64 > std::numeric_limits<GuestAddress>::max())
                    throw std::runtime_error("Objective-C category-list address overflowed");
                GuestAddress categoryAddress = 0;
                if (!readWord(memory, static_cast<GuestAddress>(categorySlot64), categoryAddress))
                    throw std::runtime_error("Objective-C category-list entry is unreadable");
                if (categoryAddress == 0)
                    continue;
                std::array<std::uint32_t, 6> category{};
                if (!memory.read(categoryAddress, category.data(), sizeof(category)))
                    throw std::runtime_error("Objective-C category metadata is outside readable guest memory");
                (void)readMetadataString(memory, category[0]);
                auto *targetClass = const_cast<Class *>(classForGuest(memory, category[1]));
                if (!targetClass || targetClass->isMetaclass)
                    throw std::runtime_error("Objective-C category target class is unknown");
                GuestClassMetadata categoryMetadata;
                registerMethods(categoryMetadata, targetClass, category[2]);
                registerMethods(categoryMetadata, targetClass->metaclass, category[3]);
                const auto protocols = readProtocolList(category[4]);
                targetClass->protocols.insert(protocols.begin(), protocols.end());
            }
        }
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

void ShimAdapter::registerBindings(ShimRegistry &registry) {
    if (registered_)
        throw std::runtime_error("Objective-C shim adapter is already registered");
    registerClassSymbols(registry);
    registerEmptyDataSymbol(registry, "__objc_empty_cache");
    registerEmptyDataSymbol(registry, "__objc_empty_vtable");
    registerTransferFunction(registry, "_objc_msgSend", "objc_msgSend",
                             [this](auto &registers, auto &memory, auto &target, auto &reason) {
                                 return dispatch(registers, memory, reason, false, nullptr, &target);
                             });
    registerTransferFunction(registry, "_objc_msgSend_stret", "objc_msgSend_stret",
                             [this](auto &registers, auto &memory, auto &target, auto &reason) {
                                 return dispatchStret(registers, memory, reason, &target);
                             });
    registerTransferFunction(registry, "_objc_msgSendSuper2", "objc_msgSendSuper2",
                             [this](auto &registers, auto &memory, auto &target, auto &reason) {
                                 return dispatch(registers, memory, reason, true, nullptr, &target);
                             });
    registerExceptionFunction(registry, "_objc_enumerationMutation",
                              "objc_enumerationMutation_exception-boundary",
                              [](CpuRegisterState &registers, GuestAddressSpace &,
                                 std::string &reason) {
                                  reason = "Objective-C fast-enumeration mutation at guest "
                                           "collection address " +
                                           std::to_string(registers.r[0]) +
                                           "; guest catch/unwind is unsupported.";
                                  return true;
                              });
    registerFunction(registry, "_objc_getClass", "objc_getClass",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return getClass(registers, memory, reason, false);
                     });
    registerFunction(registry, "_objc_getMetaClass", "objc_getMetaClass",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return getClass(registers, memory, reason, true);
                     });
    registerFunction(registry, "_objc_getProtocol", "objc_getProtocol",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         try {
                             if (registers.r[0] == 0) {
                                 registers.r[0] = 0;
                                 return true;
                             }
                             const auto name = readGuestString(memory, registers.r[0]);
                             auto &state = guestState(memory);
                             std::lock_guard<std::mutex> lock(mutex_);
                             const auto protocol = state.protocolAddresses.find(name);
                             registers.r[0] = protocol == state.protocolAddresses.end()
                                 ? 0
                                 : protocol->second;
                             return true;
                         } catch (const std::exception &error) {
                             reason = error.what();
                             return false;
                         }
                     });
    registerFunction(registry, "_sel_registerName", "sel_registerName",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         try {
                             const auto name = readGuestString(memory, registers.r[0]);
                             if (name.empty())
                                 throw std::runtime_error("Objective-C selector name is empty");
                             const auto value = runtime_.selector(name);
                             {
                                 std::lock_guard<std::mutex> lock(mutex_);
                                 selectorNames_[value] = name;
                                 selectorIds_[name] = value;
                             }
                             registers.r[0] = value;
                             return true;
                         } catch (const std::exception &error) {
                             reason = error.what();
                             return false;
                         }
                     });
    registerFunction(registry, "_objc_retain", "objc_retain",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         try {
                             auto *object = objectForGuest(memory, registers.r[0]);
                             if (!object)
                                 throw std::runtime_error("objc_retain received an unknown guest object");
                             retain(object);
                             registers.r[0] = ensureObjectAddress(memory, object);
                             return true;
                         } catch (const std::exception &error) {
                             reason = error.what();
                             return false;
                         }
                     });
    registerFunction(registry, "_objc_release", "objc_release",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         try {
                             auto *object = objectForGuest(memory, registers.r[0]);
                             if (!object)
                                 throw std::runtime_error("objc_release received an unknown guest object");
                             if (autoreleasePools_.count(registers.r[0]) != 0)
                                 drainPool(memory, object);
                             else
                                 releaseObject(memory, object);
                             registers.r[0] = 0;
                             return true;
                         } catch (const std::exception &error) {
                             reason = error.what();
                             return false;
                         }
                     });
    registerFunction(registry, "_objc_autorelease", "objc_autorelease",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         try {
                             auto *object = objectForGuest(memory, registers.r[0]);
                             if (!object || activeAutoreleasePools_.empty())
                                 throw std::runtime_error("objc_autorelease requires an object and active pool");
                             autoreleasePools_.at(activeAutoreleasePools_.back()).objects.push_back(object);
                             registers.r[0] = ensureObjectAddress(memory, object);
                             return true;
                         } catch (const std::exception &error) {
                             reason = error.what();
                             return false;
                         }
                     });
    registerFunction(registry, "_objc_autoreleasePoolPush", "objc_autoreleasePoolPush",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return autoreleasePoolPush(registers, memory, reason);
                     });
    registerFunction(registry, "_objc_autoreleasePoolPop", "objc_autoreleasePoolPop",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return autoreleasePoolPop(registers, memory, reason);
                     });
    registerFunction(registry, "_object_getClass", "object_getClass",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         try {
                             auto *object = objectForGuest(memory, registers.r[0]);
                             if (!object)
                                 throw std::runtime_error("object_getClass received an unknown guest object");
                             registers.r[0] = ensureClassAddress(memory, object->isa);
                             return true;
                         } catch (const std::exception &error) {
                             reason = error.what();
                             return false;
                         }
                     });
    registerFunction(registry, "_class_getName", "class_getName",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         try {
                             const auto *klass = classForGuest(memory, registers.r[0]);
                             if (!klass)
                                 throw std::runtime_error("class_getName received an unknown guest class");
                             registers.r[0] = ensureClassAddress(memory, klass) +
                                              static_cast<GuestAddress>(kClassNameOffset);
                             return true;
                         } catch (const std::exception &error) {
                             reason = error.what();
                             return false;
                         }
                     });
    registerFunction(registry, "_sel_getName", "sel_getName",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         try {
                             registers.r[0] = selectorStringAddress(memory, registers.r[0]);
                             return true;
                         } catch (const std::exception &error) {
                             reason = error.what();
                             return false;
                         }
                     });
    if (nextCallout_ > 0xf000ffffU - 4U)
        throw std::overflow_error("Foundation shim callout range is exhausted");
    ShimBinding searchPathsBinding;
    searchPathsBinding.darwinSymbol = "_NSSearchPathForDirectoriesInDomains";
    searchPathsBinding.library = "Foundation";
    searchPathsBinding.adapterName = "foundation-search-paths-virtual-user-domain";
    searchPathsBinding.guestAddress = nextCallout_;
    searchPathsBinding.invoke = [this](auto &registers, auto &memory, auto &reason) {
        return searchPaths(registers, memory, reason);
    };
    nextCallout_ += 4U;
    registry.registerBinding(std::move(searchPathsBinding));

    // Foundation's path functions return virtual guest paths, never host
    // filesystem names. The runtime mounts "/" to app-private storage and
    // gives Documents/Library/tmp more-specific mounts.
    const auto registerFoundationPath = [this, &registry](const std::string &symbol,
                                                         const std::string &adapter,
                                                         const std::string &path) {
        if (nextCallout_ > 0xf000ffffU - 4U)
            throw std::overflow_error("Foundation path callout range is exhausted");
        ShimBinding binding;
        binding.darwinSymbol = symbol;
        binding.library = "Foundation";
        binding.adapterName = adapter;
        binding.guestAddress = nextCallout_;
        binding.invoke = [this, path](CpuRegisterState &registers, GuestAddressSpace &memory,
                                      std::string &reason) {
            try {
                registers.r[0] = createGuestString(memory, path, true);
                return registers.r[0] != 0;
            } catch (const std::exception &error) {
                reason = error.what();
                return false;
            }
        };
        nextCallout_ += 4U;
        registry.registerBinding(std::move(binding));
    };
    registerFoundationPath("_NSHomeDirectory", "foundation-home-directory-app-sandbox-root", "/");
    registerFoundationPath("_NSTemporaryDirectory", "foundation-temporary-directory-app-sandbox", "/tmp");

    // --- bounded application lifecycle ("startup chain") ------------------
    if (nextCallout_ > 0xf000ffffU - 4U)
        throw std::overflow_error("Objective-C lifecycle callout range is exhausted");
    ShimBinding lifecycleContinuationBinding;
    lifecycleContinuationBinding.darwinSymbol = "_radek_lifecycle_continuation";
    lifecycleContinuationBinding.library = "UIKit";
    lifecycleContinuationBinding.adapterName = "uikit-lifecycle-continuation";
    lifecycleContinuationBinding.guestAddress = nextCallout_;
    // Registered as a *transfer*, not a plain callout: when CFRunLoop has
    // another ready source the continuation hands control straight to that
    // guest callback instead of returning, which is what produces the next
    // frame. Frames that have no follow-up leave the target at zero, so the
    // callout simply returns to the guest exactly as it always did.
    lifecycleContinuationBinding.invokeTransfer =
        [this](auto &registers, auto &memory, auto &target, auto &reason) {
            return lifecycleContinuation(registers, memory, reason, &target);
        };
    nextCallout_ += 4U;
    registry.registerBinding(std::move(lifecycleContinuationBinding));
    const auto lifecycleContinuation = registry.resolve("_radek_lifecycle_continuation");
    if (!lifecycleContinuation || !lifecycleContinuation->invokeTransfer)
        throw std::runtime_error("Objective-C lifecycle continuation was not registered");
    lifecycleContinuationAddress_ = lifecycleContinuation->guestAddress;
    registerTransferFunction(registry, "_UIApplicationMain", "uikit-application-main",
                             [this](auto &registers, auto &memory, auto &target, auto &reason) {
                                 return applicationMain(registers, memory, target, reason);
                             });

    // --- run loop: keeps the guest drawing past its startup callback --------
    // A game that returns from `main()` after `applicationDidFinishLaunching:`
    // never draws a second frame. CFRunLoopRun services the app's scheduled
    // timers, deferred selectors and posted input events instead of returning,
    // which is what a real UIApplicationMain does for the lifetime of the app.
    // Transfers, not returns: the loop redirects the PC into the guest callback,
    // exactly like `_UIApplicationMain` does for the startup message.
    registerTransferFunction(registry, "_CFRunLoopRun", "corefoundation-run-loop-run",
                             [this](auto &registers, auto &memory, auto &target, auto &reason) {
                                 return runLoopRun(registers, memory, target, reason);
                             });
    registerTransferFunction(registry, "_CFRunLoopRunInMode", "corefoundation-run-loop-run-in-mode",
                             [this](auto &registers, auto &memory, auto &target, auto &reason) {
                                 return runLoopRun(registers, memory, target, reason);
                             });
    registerFunction(registry, "_CFRunLoopStop", "corefoundation-run-loop-stop",
                     [this](auto &registers, auto &memory, auto &) {
                         runLoopStop(memory);
                         registers.r[0] = 0;
                         return true;
                     });
    // The run loop object itself is a token: the bounded adapter has one loop.
    registerFunction(registry, "_CFRunLoopGetMain", "corefoundation-run-loop-get-main",
                     [](auto &registers, auto &, auto &) {
                         registers.r[0] = 1;
                         return true;
                     });
    registerFunction(registry, "_CFRunLoopGetCurrent", "corefoundation-run-loop-get-current",
                     [](auto &registers, auto &, auto &) {
                         registers.r[0] = 1;
                         return true;
                     });

    constexpr const char *copyContinuationSymbol =
        "_radek_objc_setProperty_copy_continuation";
    registerFunction(registry, copyContinuationSymbol,
                     "objc_setProperty_copy_continuation",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return continuePropertyCopy(registers, memory, reason);
                     });
    const auto copyContinuation = registry.resolve(copyContinuationSymbol);
    if (!copyContinuation || !copyContinuation->invoke)
        throw std::runtime_error("Objective-C property-copy continuation was not registered");
    propertyCopyContinuationAddress_ = copyContinuation->guestAddress;
    registerTransferFunction(registry, "_objc_setProperty", "objc_setProperty",
                             [this](auto &registers, auto &memory, auto &target, auto &reason) {
                                 return setPropertyOrCopy(registers, memory, target, reason);
                             });
    registry.registerImageInitializer(
        "objc-runtime-core",
        [this](GuestAddressSpace &memory, const std::vector<GuestImageSection> &sections,
               std::string &reason) {
            return initializeImage(memory, sections, reason);
        });
    registered_ = true;
}

Object *ShimAdapter::objectForGuest(GuestAddressSpace &memory, GuestAddress address) {
    auto &state = guestState(memory);
    const auto found = state.objectsByAddress.find(address);
    return found == state.objectsByAddress.end() ? nullptr : found->second;
}

const Class *ShimAdapter::classForGuest(GuestAddressSpace &memory, GuestAddress address) {
    auto &state = guestState(memory);
    const auto found = state.classesByAddress.find(address);
    return found == state.classesByAddress.end() ? nullptr : found->second;
}

void ShimAdapter::synchronizeObject(GuestAddressSpace &memory, Object *object,
                                    GuestAddress address) {
    if (object->ivars.size() > (kGuestPageSize - sizeof(std::uint32_t)) / sizeof(std::uint32_t))
        throw std::length_error("Objective-C instance has too many guest ivars");
    for (std::size_t index = 0; index < object->ivars.size(); ++index) {
        const auto slotAddress = address + static_cast<GuestAddress>(sizeof(std::uint32_t) * (index + 1));
        std::uint32_t value = 0;
        if (!memory.read(slotAddress, &value, sizeof(value)))
            throw std::runtime_error("Objective-C instance ivar is outside guest memory");
        object->ivars[index] = value;
    }
    writeWord(memory, address, ensureClassAddress(memory, object->isa));
    for (std::size_t index = 0; index < object->ivars.size(); ++index) {
        const auto slotAddress = address + static_cast<GuestAddress>(sizeof(std::uint32_t) * (index + 1));
        const auto value = static_cast<std::uint32_t>(object->ivars[index]);
        writeWord(memory, slotAddress, value);
    }
}

void ShimAdapter::releaseObject(GuestAddressSpace &memory, Object *object) {
    if (!object)
        return;
    auto &state = guestState(memory);
    const bool finalRelease = object->references.load() == 1;
    if (finalRelease && object->isa && object->isa->name == "NSArray") {
        auto items = std::move(object->arrayItems);
        for (auto iterator = items.rbegin(); iterator != items.rend(); ++iterator)
            releaseObject(memory, *iterator);
    }
    const auto found = state.objectAddresses.find(object);
    if (found != state.objectAddresses.end() && finalRelease) {
        const auto address = found->second;
        state.objectAddresses.erase(found);
        state.objectsByAddress.erase(address);
        memory.unmap(address);
    }
    release(object);
}

void ShimAdapter::drainPool(GuestAddressSpace &memory, Object *poolObject) {
    if (!poolObject)
        throw std::runtime_error("Objective-C autorelease pool receiver is nil");
    auto &state = guestState(memory);
    const auto foundAddress = state.objectAddresses.find(poolObject);
    if (foundAddress == state.objectAddresses.end())
        throw std::runtime_error("Objective-C autorelease pool is not a guest object");
    const auto token = foundAddress->second;
    const auto foundPool = autoreleasePools_.find(token);
    if (foundPool == autoreleasePools_.end())
        throw std::runtime_error("Objective-C autorelease pool was not initialized");
    if (activeAutoreleasePools_.empty() || activeAutoreleasePools_.back() != token)
        throw std::runtime_error("Objective-C autorelease pools must be drained in LIFO order");
    auto objects = std::move(foundPool->second.objects);
    autoreleasePools_.erase(foundPool);
    activeAutoreleasePools_.pop_back();
    for (auto iterator = objects.rbegin(); iterator != objects.rend(); ++iterator)
        releaseObject(memory, *iterator);
    releaseObject(memory, poolObject);
}

bool ShimAdapter::dispatchStret(CpuRegisterState &registers, GuestAddressSpace &memory,
                                std::string &reason, GuestAddress *guestTarget) {
    try {
        const auto originalRegisters = registers;
        const auto returnBuffer = registers.r[0];
        if (returnBuffer == 0 || (returnBuffer & 3U) != 0 ||
            !memory.contains(returnBuffer, sizeof(std::uint64_t), MemoryPermission::Write))
            throw std::runtime_error("objc_msgSend_stret received an invalid result buffer");
        std::uint32_t secondArgument = 0;
        if (registers.r[13] == 0 ||
            !memory.read(registers.r[13], &secondArgument, sizeof(secondArgument)))
            throw std::runtime_error("objc_msgSend_stret could not read its stack arguments");

        const auto receiver = registers.r[1];
        const auto selector = registers.r[2];
        const auto firstArgument = registers.r[3];
        // A 16-byte CGRect return (UIView/UIWindow/UIScreen bounds or frame) has
        // no register arguments, so it is served directly from the bounded view
        // state instead of the generic two-word path below.
        {
            const auto selectorValue = selectorForGuest(memory, selector);
            std::string selectorName;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                selectorName = selectorNames_.at(selectorValue);
            }
            if (selectorName == "bounds" || selectorName == "frame" ||
                selectorName == "applicationFrame") {
                auto *receiverObject = objectForGuest(memory, receiver);
                if (!receiverObject)
                    throw std::runtime_error("struct-return selector '" + selectorName +
                                             "' received an unknown guest receiver");
                if (!(receiverObject->isa &&
                      (receiverObject->isa->name == "UIScreen" ||
                       receiverObject->isa->name == "UIView" ||
                       receiverObject->isa->name == "UIWindow" ||
                       receiverObject->isa->name == "CAEAGLLayer" ||
                       receiverObject->isa->name == "UIApplication")))
                    throw std::runtime_error("struct-return selector '" + selectorName +
                                             "' is only implemented for UIKit views");
                // The window/screen rectangle defaults to the host viewport when
                // the guest never materialized a frame, so a landscape game is
                // not handed a hardcoded portrait rectangle.
                std::array<float, 4> storedFrame{0.0f, 0.0f, kVirtualViewWidth,
                                                 kVirtualViewHeight};
                std::uint32_t viewportWidth = 0;
                std::uint32_t viewportHeight = 0;
                effectiveViewport(memory, viewportWidth, viewportHeight);
                if (viewportWidth != 0 && viewportHeight != 0) {
                    storedFrame[2] = static_cast<float>(viewportWidth);
                    storedFrame[3] = static_cast<float>(viewportHeight);
                }
                const auto bitsToFloat = [](Value value) {
                    const auto bits = static_cast<std::uint32_t>(value);
                    float result = 0.0f;
                    std::memcpy(&result, &bits, sizeof(result));
                    return result;
                };
                if (receiverObject->ivars.size() >= 4 &&
                    bitsToFloat(receiverObject->ivars[2]) >= 1.0f &&
                    bitsToFloat(receiverObject->ivars[3]) >= 1.0f) {
                    for (std::size_t index = 0; index < storedFrame.size(); ++index) {
                        const auto bits = static_cast<std::uint32_t>(receiverObject->ivars[index]);
                        std::memcpy(&storedFrame[index], &bits, sizeof(bits));
                    }
                }
                // `applicationFrame` is the screen rectangle minus the status
                // bar; 2009-era games use it to size their window.
                const bool applicationFrame = selectorName == "applicationFrame";
                const float statusBarHeight = applicationFrame && !statusBarHidden(memory)
                                                  ? kStatusBarHeight
                                                  : 0.0f;
                std::array<float, 4> frame =
                    selectorName == "bounds"
                        ? std::array<float, 4>{0.0f, 0.0f, storedFrame[2], storedFrame[3]}
                        : storedFrame;
                if (applicationFrame) {
                    frame = {0.0f, statusBarHeight, storedFrame[2],
                             std::max(storedFrame[3] - statusBarHeight, 1.0f)};
                }
                if (!memory.write(returnBuffer, frame.data(), sizeof(frame)))
                    throw std::runtime_error("objc_msgSend_stret could not write the CGRect result");
                registers.r[0] = returnBuffer;
                return true;
            }
        }
        registers.r[0] = receiver;
        registers.r[1] = selector;
        registers.r[2] = firstArgument;
        registers.r[3] = secondArgument;
        Value returnedValue = 0;
        if (!dispatch(registers, memory, reason, false, &returnedValue, guestTarget))
            return false;
        if (guestTarget && *guestTarget != 0) {
            registers = originalRegisters;
            return true;
        }

        const auto bits = static_cast<std::uint64_t>(returnedValue);
        std::array<std::uint8_t, sizeof(bits)> resultBytes{};
        for (std::size_t index = 0; index < resultBytes.size(); ++index)
            resultBytes[index] = static_cast<std::uint8_t>(bits >> (index * 8));
        if (!memory.write(returnBuffer, resultBytes.data(), resultBytes.size()))
            throw std::runtime_error("objc_msgSend_stret could not write its result buffer");
        registers.r[0] = returnBuffer;
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::setProperty(CpuRegisterState &registers, GuestAddressSpace &memory,
                              std::string &reason) {
    try {
        const auto receiverAddress = registers.r[0];
        const auto signedOffset = static_cast<std::int32_t>(registers.r[2]);
        if (receiverAddress == 0 || signedOffset < static_cast<std::int32_t>(sizeof(std::uint32_t)) ||
            (signedOffset % static_cast<std::int32_t>(sizeof(std::uint32_t))) != 0)
            throw std::runtime_error("objc_setProperty received an invalid receiver or ivar offset");
        const auto slot64 = static_cast<std::uint64_t>(receiverAddress) +
                            static_cast<std::uint32_t>(signedOffset);
        if (slot64 > std::numeric_limits<GuestAddress>::max())
            throw std::runtime_error("objc_setProperty ivar address overflowed");
        const auto slotAddress = static_cast<GuestAddress>(slot64);
        if (!memory.contains(slotAddress, sizeof(std::uint32_t),
                             MemoryPermission::Read | MemoryPermission::Write))
            throw std::runtime_error("objc_setProperty ivar is outside writable guest memory");

        std::array<std::uint32_t, 2> flags{};
        if (registers.r[13] == 0 ||
            !memory.read(registers.r[13], flags.data(), sizeof(flags)))
            throw std::runtime_error("objc_setProperty could not read its stack arguments");
        if (flags[1] != 0)
            throw std::runtime_error("objc_setProperty copy semantics are not implemented");

        std::unique_lock<std::mutex> atomicLock(propertyMutex_, std::defer_lock);
        if (flags[0] != 0)
            atomicLock.lock();

        std::uint32_t oldValueAddress = 0;
        if (!memory.read(slotAddress, &oldValueAddress, sizeof(oldValueAddress)))
            throw std::runtime_error("objc_setProperty could not read the existing ivar");
        const auto newValueAddress = registers.r[3];
        auto *newValue = newValueAddress == 0 ? nullptr : objectForGuest(memory, newValueAddress);
        auto *oldValue = oldValueAddress == 0 ? nullptr : objectForGuest(memory, oldValueAddress);
        if (newValueAddress != 0 && !newValue)
            throw std::runtime_error("objc_setProperty received an unknown new object");
        if (oldValueAddress != 0 && !oldValue)
            throw std::runtime_error("objc_setProperty cannot release the existing guest object");

        auto *receiver = objectForGuest(memory, receiverAddress);
        std::size_t ivarIndex = 0;
        if (receiver) {
            ivarIndex = static_cast<std::size_t>(signedOffset) / sizeof(std::uint32_t) - 1;
            if (ivarIndex >= receiver->ivars.size())
                throw std::runtime_error("objc_setProperty offset is not a registered object ivar");
        }

        if (newValue != oldValue)
            retain(newValue);
        writeWord(memory, slotAddress, newValueAddress);
        if (receiver)
            receiver->ivars[ivarIndex] = newValueAddress;
        if (newValue != oldValue)
            releaseObject(memory, oldValue);
        registers.r[0] = 0;
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

void ShimAdapter::autoreleaseGuestObject(Object *object) {
    if (activeAutoreleasePools_.empty())
        return;
    const auto found = autoreleasePools_.find(activeAutoreleasePools_.back());
    if (found == autoreleasePools_.end())
        throw std::runtime_error("Objective-C autorelease pool state is inconsistent");
    found->second.objects.push_back(object);
}

GuestAddress ShimAdapter::createGuestString(GuestAddressSpace &memory,
                                            const std::string &value,
                                            bool autorelease) {
    if (value.size() + 1 > kGuestPageSize - kClassNameOffset)
        throw std::length_error("Foundation string exceeds the bounded guest object payload");
    auto *object = runtime_.allocate(classes_.at("NSString"));
    try {
        object->stringValue = value;
        const auto address = ensureObjectAddress(memory, object);
        if (autorelease)
            autoreleaseGuestObject(object);
        return address;
    } catch (...) {
        releaseObject(memory, object);
        throw;
    }
}

GuestAddress ShimAdapter::createConstantString(GuestAddressSpace &memory,
                                              const std::string &value, std::string &reason) {
    try {
        return createGuestString(memory, value, false);
    } catch (const std::exception &error) {
        reason = std::string("constant string could not be materialized: ") + error.what();
        return 0;
    }
}

GuestAddress ShimAdapter::createGuestStringArray(GuestAddressSpace &memory,
                                                 const std::vector<std::string> &values,
                                                 bool autorelease) {
    if (values.size() > kMaximumImageMethods)
        throw std::length_error("Foundation array exceeds the bounded guest element count");
    auto *array = runtime_.allocate(classes_.at("NSArray"));
    GuestAddress arrayAddress = 0;
    try {
        array->arrayItems.reserve(values.size());
        for (const auto &value : values) {
            auto *item = runtime_.allocate(classes_.at("NSString"));
            try {
                item->stringValue = value;
                array->arrayItems.push_back(item);
            } catch (...) {
                release(item);
                throw;
            }
            (void)ensureObjectAddress(memory, item);
        }
        arrayAddress = ensureObjectAddress(memory, array);
        if (autorelease)
            autoreleaseGuestObject(array);
        return arrayAddress;
    } catch (...) {
        if (arrayAddress != 0) {
            releaseObject(memory, array);
        } else {
            auto items = std::move(array->arrayItems);
            for (auto *item : items)
                releaseObject(memory, item);
            release(array);
        }
        throw;
    }
}

bool ShimAdapter::searchPaths(CpuRegisterState &registers, GuestAddressSpace &memory,
                              std::string &reason) {
    try {
        constexpr std::uint32_t kUserDomainMask = 1U;
        const auto directory = registers.r[0];
        const auto domainMask = registers.r[1];
        const bool expandTilde = registers.r[2] != 0;
        std::string path;
        if ((domainMask & kUserDomainMask) != 0) {
            switch (directory) {
            case 5: // NSLibraryDirectory
                path = "/Library";
                break;
            case 9: // NSDocumentDirectory
                path = "/Documents";
                break;
            case 11: // NSAutosavedInformationDirectory
                path = "/Library/Autosave Information";
                break;
            case 13: // NSCachesDirectory
                path = "/Library/Caches";
                break;
            case 14: // NSApplicationSupportDirectory
                path = "/Library/Application Support";
                break;
            default:
                break;
            }
        }
        std::vector<std::string> paths;
        if (!path.empty()) {
            if (!expandTilde)
                path = "~" + path;
            paths.push_back(std::move(path));
        }
        registers.r[0] = createGuestStringArray(memory, paths, true);
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::storePropertyValue(CpuRegisterState &registers, GuestAddressSpace &memory,
                                      GuestAddress receiver, std::int32_t offset,
                                      GuestAddress value, bool atomic,
                                      GuestAddress originalStackPointer,
                                      GuestAddress originalReturnAddress,
                                      std::string &reason) {
    GuestAddress flagsAddress = 0;
    bool stored = false;
    try {
        flagsAddress = memory.mapAny(kGuestPageSize,
            MemoryPermission::Read | MemoryPermission::Write, "objc-property-store-flags");
        const std::array<std::uint32_t, 2> flags{{atomic ? 1U : 0U, 0U}};
        if (!memory.write(flagsAddress, flags.data(), sizeof(flags)))
            throw std::runtime_error("objc_setProperty could not create temporary setter flags");
        auto setterRegisters = registers;
        setterRegisters.r[0] = receiver;
        setterRegisters.r[2] = static_cast<GuestAddress>(offset);
        setterRegisters.r[3] = value;
        setterRegisters.r[13] = flagsAddress;
        setterRegisters.r[14] = originalReturnAddress;
        stored = setProperty(setterRegisters, memory, reason);
    } catch (const std::exception &error) {
        reason = error.what();
        stored = false;
    }
    if (flagsAddress != 0)
        memory.unmap(flagsAddress);
    registers.r[0] = 0;
    registers.r[13] = originalStackPointer;
    registers.r[14] = originalReturnAddress;
    return stored;
}

bool ShimAdapter::setPropertyOrCopy(CpuRegisterState &registers, GuestAddressSpace &memory,
                                    GuestAddress &guestTarget, std::string &reason) {
    guestTarget = 0;
    try {
        const auto receiverAddress = registers.r[0];
        const auto signedOffset = static_cast<std::int32_t>(registers.r[2]);
        const auto originalStackPointer = registers.r[13];
        const auto originalReturnAddress = registers.r[14];
        std::array<std::uint32_t, 2> flags{};
        if (originalStackPointer == 0 ||
            !memory.read(originalStackPointer, flags.data(), sizeof(flags)))
            throw std::runtime_error("objc_setProperty could not read its stack arguments");
        if (flags[1] == 0)
            return setProperty(registers, memory, reason);

        if (receiverAddress == 0 ||
            signedOffset < static_cast<std::int32_t>(sizeof(std::uint32_t)) ||
            (signedOffset % static_cast<std::int32_t>(sizeof(std::uint32_t))) != 0)
            throw std::runtime_error("objc_setProperty received an invalid receiver or ivar offset");
        const auto slot64 = static_cast<std::uint64_t>(receiverAddress) +
                            static_cast<std::uint32_t>(signedOffset);
        if (slot64 > std::numeric_limits<GuestAddress>::max())
            throw std::runtime_error("objc_setProperty ivar address overflowed");
        const auto slotAddress = static_cast<GuestAddress>(slot64);
        if (!memory.contains(slotAddress, sizeof(std::uint32_t),
                             MemoryPermission::Read | MemoryPermission::Write))
            throw std::runtime_error("objc_setProperty ivar is outside writable guest memory");
        std::uint32_t oldValueAddress = 0;
        if (!memory.read(slotAddress, &oldValueAddress, sizeof(oldValueAddress)))
            throw std::runtime_error("objc_setProperty could not read the existing ivar");
        auto *oldValue = oldValueAddress == 0 ? nullptr : objectForGuest(memory, oldValueAddress);
        if (oldValueAddress != 0 && !oldValue)
            throw std::runtime_error("objc_setProperty cannot release the existing guest object");
        auto *receiverObject = objectForGuest(memory, receiverAddress);
        if (receiverObject) {
            const auto ivarIndex = static_cast<std::size_t>(signedOffset) /
                                   sizeof(std::uint32_t) - 1;
            if (ivarIndex >= receiverObject->ivars.size())
                throw std::runtime_error("objc_setProperty offset is not a registered object ivar");
        }
        const auto newValueAddress = registers.r[3];
        if (newValueAddress != 0 && !objectForGuest(memory, newValueAddress))
            throw std::runtime_error("objc_setProperty received an unknown new object");
        if (newValueAddress == 0)
            return storePropertyValue(registers, memory, receiverAddress, signedOffset, 0,
                                      flags[0] != 0, originalStackPointer,
                                      originalReturnAddress, reason);

        Selector copySelector = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copySelector = selectorIds_.at("copy");
        }
        const auto copySelectorAddress = selectorStringAddress(memory, copySelector);
        CpuRegisterState copyRegisters = registers;
        copyRegisters.r[0] = newValueAddress;
        copyRegisters.r[1] = copySelectorAddress;
        copyRegisters.r[2] = 0;
        copyRegisters.r[3] = 0;
        GuestAddress copyTarget = 0;
        if (!dispatch(copyRegisters, memory, reason, false, nullptr, &copyTarget))
            return false;
        const auto copiedValueAddress = copyRegisters.r[0];
        if (copyTarget == 0) {
            registers = copyRegisters;
            const auto stored = storePropertyValue(registers, memory, receiverAddress,
                signedOffset, copiedValueAddress, flags[0] != 0, originalStackPointer,
                originalReturnAddress, reason);
            auto *copiedObject = copiedValueAddress == 0
                ? nullptr
                : objectForGuest(memory, copiedValueAddress);
            if (copiedObject)
                releaseObject(memory, copiedObject);
            return stored;
        }

        if (propertyCopyContinuationAddress_ == 0 || originalStackPointer < 8U ||
            (originalStackPointer & 7U) != 0)
            throw std::runtime_error("objc_setProperty cannot create an aligned copy continuation frame");
        const auto continuationStackPointer = originalStackPointer - 8U;
        if (!memory.contains(continuationStackPointer, 8,
                             MemoryPermission::Read | MemoryPermission::Write))
            throw std::runtime_error("objc_setProperty copy continuation exceeds writable guest stack");
        std::uint32_t originalStackWord = 0;
        if (!memory.read(continuationStackPointer, &originalStackWord, sizeof(originalStackWord)))
            throw std::runtime_error("objc_setProperty cannot preserve its copy continuation stack word");

        std::uint32_t token = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (nextPropertyCopyToken_ == 0)
                throw std::overflow_error("Objective-C property-copy continuation tokens are exhausted");
            token = nextPropertyCopyToken_++;
        }
        auto &state = guestState(memory);
        const PropertyCopyContinuation continuation{
            receiverAddress, signedOffset, continuationStackPointer,
            originalStackPointer, originalStackWord, originalReturnAddress, flags[0] != 0};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!state.pendingPropertyCopies.emplace(token, continuation).second)
                throw std::runtime_error("Objective-C property-copy continuation token is duplicated");
        }
        if (!memory.write(continuationStackPointer, &token, sizeof(token))) {
            std::lock_guard<std::mutex> lock(mutex_);
            state.pendingPropertyCopies.erase(token);
            throw std::runtime_error("objc_setProperty could not write its copy continuation token");
        }
        registers = copyRegisters;
        registers.r[13] = continuationStackPointer;
        registers.r[14] = propertyCopyContinuationAddress_;
        guestTarget = copyTarget;
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::continuePropertyCopy(CpuRegisterState &registers,
                                       GuestAddressSpace &memory, std::string &reason) {
    try {
        const auto continuationStackPointer = registers.r[13];
        std::uint32_t token = 0;
        if (continuationStackPointer == 0 ||
            !memory.read(continuationStackPointer, &token, sizeof(token)))
            throw std::runtime_error("objc_setProperty continuation token is unreadable");
        auto &state = guestState(memory);
        PropertyCopyContinuation continuation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto pending = state.pendingPropertyCopies.find(token);
            if (pending == state.pendingPropertyCopies.end() ||
                pending->second.continuationStackPointer != continuationStackPointer)
                throw std::runtime_error("objc_setProperty continuation token is unknown");
            continuation = pending->second;
            state.pendingPropertyCopies.erase(pending);
        }
        const auto copiedValueAddress = registers.r[0];
        if (!memory.write(continuation.continuationStackPointer,
                          &continuation.originalStackWord,
                          sizeof(continuation.originalStackWord)))
            throw std::runtime_error("objc_setProperty could not restore its copy continuation stack word");
        const auto stored = storePropertyValue(registers, memory, continuation.receiver,
            continuation.offset, copiedValueAddress, continuation.atomic,
            continuation.originalStackPointer, continuation.originalReturnAddress, reason);
        auto *copiedObject = copiedValueAddress == 0
            ? nullptr
            : objectForGuest(memory, copiedValueAddress);
        if (copiedObject)
            releaseObject(memory, copiedObject);
        return stored;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::getClass(CpuRegisterState &registers, GuestAddressSpace &memory,
                           std::string &reason, bool metaclass) {
    try {
        const auto name = readGuestString(memory, registers.r[0]);
        const auto found = classes_.find(name);
        if (found == classes_.end()) {
            registers.r[0] = 0;
            return true;
        }
        const Class *klass = found->second;
        if (metaclass)
            klass = klass->metaclass;
        registers.r[0] = ensureClassAddress(memory, klass);
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::autoreleasePoolPush(CpuRegisterState &registers, GuestAddressSpace &memory,
                                      std::string &reason) {
    try {
        const auto token = memory.mapAny(kGuestPageSize,
            MemoryPermission::Read | MemoryPermission::Write, "objc-autorelease-pool-token");
        autoreleasePools_.emplace(token, AutoreleasePoolState{});
        activeAutoreleasePools_.push_back(token);
        registers.r[0] = token;
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::autoreleasePoolPop(CpuRegisterState &registers, GuestAddressSpace &memory,
                                     std::string &reason) {
    const auto token = registers.r[0];
    const auto found = autoreleasePools_.find(token);
    if (found == autoreleasePools_.end() || found->second.poolObject ||
        activeAutoreleasePools_.empty() || activeAutoreleasePools_.back() != token) {
        reason = "Objective-C autorelease-pool token is unknown or out of LIFO order";
        return false;
    }
    auto objects = std::move(found->second.objects);
    autoreleasePools_.erase(found);
    activeAutoreleasePools_.pop_back();
    for (auto iterator = objects.rbegin(); iterator != objects.rend(); ++iterator)
        releaseObject(memory, *iterator);
    memory.unmap(token);
    registers.r[0] = 0;
    return true;
}

bool ShimAdapter::dispatch(CpuRegisterState &registers, GuestAddressSpace &memory,
                           std::string &reason, bool superDispatch, Value *rawReturn,
                           GuestAddress *guestTarget) {
    try {
        GuestAddress receiverAddress = registers.r[0];
        const auto selectorGuestAddress = registers.r[1];
        const auto selectorValue = selectorForGuest(memory, selectorGuestAddress);
        std::string selectorName;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            selectorName = selectorNames_.at(selectorValue);
        }
        const Arguments arguments{registers.r[2], registers.r[3]};
        const Class *receiverClass = nullptr;
        Object *receiverObject = nullptr;
        Class *superClass = nullptr;
        if (superDispatch) {
            std::array<GuestAddress, 2> objcSuper{};
            if (!memory.read(registers.r[0], objcSuper.data(), sizeof(objcSuper)))
                throw std::runtime_error("objc_msgSendSuper2 received an invalid objc_super pointer");
            receiverAddress = objcSuper[0];
            superClass = const_cast<Class *>(classForGuest(memory, objcSuper[1]));
            if (!superClass)
                throw std::runtime_error("objc_msgSendSuper2 received an unknown superclass");
        }
        receiverClass = classForGuest(memory, receiverAddress);
        if (!receiverClass)
            receiverObject = objectForGuest(memory, receiverAddress);
        if (!receiverClass && !receiverObject) {
            if (receiverAddress == 0) {
                registers.r[0] = 0;
                if (rawReturn)
                    *rawReturn = 0;
                return true;
            }
            // Name the call site: the guest link register still holds the
            // return address of the `bl` that reached this callout.
            throw std::runtime_error("objc_msgSend received an unknown guest receiver " +
                                     hexWord(receiverAddress) + " for selector '" + selectorName +
                                     "' (call site " + hexWord(registers.r[14]) + ")");
        }
        if (receiverObject)
            synchronizeObject(memory, receiverObject, receiverAddress);

        if (selectorName == "respondsToSelector:") {
            bool responds = false;
            const auto queriedSelectorAddress = registers.r[2];
            if (receiverObject && queriedSelectorAddress != 0) {
                const auto queriedSelector = selectorForGuest(memory, queriedSelectorAddress);
                std::string knownName;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    knownName = selectorNames_.at(queriedSelector);
                }
                responds = knownName == "retain" || knownName == "release" ||
                           knownName == "autorelease" || knownName == "class" ||
                           knownName == "respondsToSelector:" ||
                           knownName == "isKindOfClass:" ||
                           knownName == "isMemberOfClass:" ||
                           knownName == "conformsToProtocol:";
                if (receiverObject->isa && receiverObject->isa->name == "NSString")
                    responds = responds || knownName == "UTF8String" ||
                               knownName == "length" || knownName == "description" ||
                               knownName == "isEqualToString:" ||
                               knownName == "stringByAppendingString:" ||
                               knownName == "stringByAppendingPathComponent:";
                if (receiverObject->isa && receiverObject->isa->name == "NSArray")
                    responds = responds || knownName == "count" ||
                               knownName == "objectAtIndex:" ||
                               knownName == "firstObject" || knownName == "lastObject";
                if (autoreleasePools_.count(receiverAddress) != 0 && knownName == "drain")
                    responds = true;
                for (auto *current = receiverObject->isa; current && !responds;
                     current = current->superclass) {
                    if (current->methods.count(queriedSelector) != 0 ||
                        guestImplementations_.count({current, queriedSelector}) != 0)
                        responds = true;
                }
            }
            registers.r[0] = responds ? 1U : 0U;
            if (rawReturn)
                *rawReturn = registers.r[0];
            return true;
        }
        if (selectorName == "isMemberOfClass:" || selectorName == "isKindOfClass:") {
            bool matches = false;
            if (receiverObject && registers.r[2] != 0) {
                const auto *targetClass = classForGuest(memory, registers.r[2]);
                if (!targetClass)
                    throw std::runtime_error(selectorName +
                                             " received an unknown guest class object");
                if (selectorName == "isMemberOfClass:") {
                    matches = receiverObject->isa == targetClass;
                } else {
                    for (auto *current = receiverObject->isa; current;
                         current = current->superclass) {
                        if (current == targetClass) {
                            matches = true;
                            break;
                        }
                    }
                }
            }
            registers.r[0] = matches ? 1U : 0U;
            if (rawReturn)
                *rawReturn = registers.r[0];
            return true;
        }

        if (selectorName == "conformsToProtocol:") {
            bool conforms = false;
            if (receiverObject && registers.r[2] != 0) {
                auto &state = guestState(memory);
                std::string targetProtocol;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto protocol = state.protocolNames.find(registers.r[2]);
                    if (protocol == state.protocolNames.end())
                        throw std::runtime_error(
                            "conformsToProtocol: received an unknown guest Protocol object");
                    targetProtocol = protocol->second;
                }
                std::function<bool(const std::string &, std::set<std::string> &)> inherits;
                inherits = [&state, &targetProtocol, &inherits](
                               const std::string &protocolName,
                               std::set<std::string> &visited) {
                    if (protocolName == targetProtocol)
                        return true;
                    if (!visited.insert(protocolName).second)
                        return false;
                    const auto parents = state.protocolParents.find(protocolName);
                    if (parents == state.protocolParents.end())
                        return false;
                    for (const auto &parent : parents->second) {
                        if (inherits(parent, visited))
                            return true;
                    }
                    return false;
                };
                for (auto *current = receiverObject->isa; current && !conforms;
                     current = current->superclass) {
                    for (const auto &protocolName : current->protocols) {
                        std::set<std::string> visited;
                        if (inherits(protocolName, visited)) {
                            conforms = true;
                            break;
                        }
                    }
                }
            }
            registers.r[0] = conforms ? 1U : 0U;
            if (rawReturn)
                *rawReturn = registers.r[0];
            return true;
        }

        if (receiverObject && (selectorName == "retain" || selectorName == "release" ||
                               selectorName == "autorelease")) {
            if (selectorName == "retain") {
                retain(receiverObject);
                registers.r[0] = receiverAddress;
            } else if (selectorName == "release") {
                if (autoreleasePools_.count(receiverAddress) != 0)
                    drainPool(memory, receiverObject);
                else
                    releaseObject(memory, receiverObject);
                registers.r[0] = 0;
            } else {
                if (activeAutoreleasePools_.empty())
                    throw std::runtime_error("Objective-C autorelease requires an active pool");
                autoreleasePools_.at(activeAutoreleasePools_.back()).objects.push_back(receiverObject);
                registers.r[0] = receiverAddress;
            }
            if (rawReturn)
                *rawReturn = registers.r[0];
            return true;
        }
        if (receiverObject && selectorName == "drain" &&
            autoreleasePools_.count(receiverAddress) != 0) {
            drainPool(memory, receiverObject);
            registers.r[0] = 0;
            if (rawReturn)
                *rawReturn = 0;
            return true;
        }

        if (guestTarget) {
            const Class *lookupClass = nullptr;
            if (superDispatch) {
                lookupClass = receiverObject
                    ? superClass->superclass
                    : (receiverClass && superClass->metaclass
                           ? superClass->metaclass->superclass
                           : nullptr);
            } else {
                lookupClass = receiverObject
                    ? receiverObject->isa
                    : (receiverClass ? receiverClass->metaclass : nullptr);
            }
            for (auto *current = lookupClass; current; current = current->superclass) {
                const auto method = guestImplementations_.find({current, selectorValue});
                if (method == guestImplementations_.end())
                    continue;
                if (superDispatch) {
                    registers.r[0] = receiverAddress;
                    registers.r[1] = selectorGuestAddress;
                }
                *guestTarget = method->second.address;
                return true;
            }
        }

        // The bounded application-lifecycle subset (UIKit startup chain) runs
        // after guest implementations: an image's own method always wins.
        std::string lifecycleReason;
        if (lifecycleSelector(registers, memory, selectorName, receiverAddress, receiverObject,
                              receiverClass, rawReturn, guestTarget, lifecycleReason))
            return true;
        if (!lifecycleReason.empty()) {
            reason = std::move(lifecycleReason);
            return false;
        }

        if (receiverObject && !superDispatch && receiverObject->isa &&
            receiverObject->isa->name == "NSString" &&
            (selectorName == "description" || selectorName == "length" ||
             selectorName == "UTF8String" || selectorName == "isEqualToString:" ||
             selectorName == "stringByAppendingString:" ||
             selectorName == "stringByAppendingPathComponent:")) {
            if (selectorName == "description") {
                registers.r[0] = receiverAddress;
            } else if (selectorName == "length") {
                registers.r[0] = static_cast<GuestAddress>(
                    utf16Length(receiverObject->stringValue));
            } else if (selectorName == "UTF8String") {
                const auto dataAddress64 = static_cast<std::uint64_t>(receiverAddress) +
                                           kClassNameOffset;
                if (dataAddress64 > std::numeric_limits<GuestAddress>::max())
                    throw std::runtime_error("NSString UTF8String address overflowed");
                const auto dataAddress = static_cast<GuestAddress>(dataAddress64);
                const auto &value = receiverObject->stringValue;
                if (value.size() + 1 > kGuestPageSize - kClassNameOffset ||
                    !memory.write(dataAddress, value.c_str(), value.size() + 1))
                    throw std::runtime_error("NSString UTF8String does not fit its guest object");
                registers.r[0] = dataAddress;
            } else if (selectorName == "isEqualToString:") {
                auto *other = registers.r[2] == 0
                    ? nullptr
                    : objectForGuest(memory, registers.r[2]);
                if (registers.r[2] != 0 &&
                    (!other || !other->isa || other->isa->name != "NSString"))
                    throw std::runtime_error("isEqualToString: received a non-NSString guest object");
                registers.r[0] = other &&
                    receiverObject->stringValue == other->stringValue ? 1U : 0U;
            } else if (selectorName == "stringByAppendingString:" ||
                       selectorName == "stringByAppendingPathComponent:") {
                auto *other = registers.r[2] == 0
                    ? nullptr
                    : objectForGuest(memory, registers.r[2]);
                if (!other || !other->isa || other->isa->name != "NSString")
                    throw std::runtime_error(selectorName +
                                             " received a non-NSString guest argument");
                auto joined = receiverObject->stringValue;
                if (selectorName == "stringByAppendingPathComponent:") {
                    auto component = other->stringValue;
                    while (!component.empty() && component.front() == '/')
                        component.erase(component.begin());
                    while (joined.size() > 1 && joined.back() == '/')
                        joined.pop_back();
                    if (joined.empty())
                        joined = std::move(component);
                    else if (!component.empty()) {
                        if (joined.back() != '/')
                            joined.push_back('/');
                        joined += component;
                    }
                } else {
                    joined += other->stringValue;
                }
                registers.r[0] = createGuestString(memory, joined, true);
            }
            if (rawReturn)
                *rawReturn = registers.r[0];
            return true;
        }
        if (receiverObject && !superDispatch && receiverObject->isa &&
            receiverObject->isa->name == "NSArray" &&
            (selectorName == "count" || selectorName == "objectAtIndex:" ||
             selectorName == "firstObject" || selectorName == "lastObject")) {
            if (selectorName == "count") {
                if (receiverObject->arrayItems.size() >
                    std::numeric_limits<GuestAddress>::max())
                    throw std::runtime_error("NSArray count exceeds the guest ABI range");
                registers.r[0] = static_cast<GuestAddress>(receiverObject->arrayItems.size());
            } else {
                std::size_t index = 0;
                if (selectorName == "firstObject") {
                    index = 0;
                } else if (selectorName == "lastObject") {
                    if (receiverObject->arrayItems.empty()) {
                        registers.r[0] = 0;
                        if (rawReturn)
                            *rawReturn = 0;
                        return true;
                    }
                    index = receiverObject->arrayItems.size() - 1;
                } else if (selectorName == "objectAtIndex:") {
                    index = registers.r[2];
                } else {
                    throw std::runtime_error("NSArray selector is not implemented: " + selectorName);
                }
                if (index >= receiverObject->arrayItems.size())
                    throw std::out_of_range("NSArray objectAtIndex: index is out of range");
                registers.r[0] = ensureObjectAddress(memory,
                    receiverObject->arrayItems[index]);
            }
            if (rawReturn)
                *rawReturn = registers.r[0];
            return true;
        }

        Value result = 0;
        try {
        if (superDispatch) {
            if (receiverObject)
                result = runtime_.sendSuper(receiverObject, superClass, selectorValue, arguments);
            else
                result = runtime_.sendSuper(const_cast<Class *>(receiverClass), superClass,
                                            selectorValue, arguments);
        } else if (receiverObject) {
            result = runtime_.send(receiverObject, selectorValue, arguments);
        } else {
            result = runtime_.send(const_cast<Class *>(receiverClass), selectorValue, arguments);
        }
        } catch (const std::exception &error) {
            const std::string owner = receiverObject
                ? (receiverObject->isa ? receiverObject->isa->name : std::string("?"))
                : (receiverClass ? receiverClass->name : std::string("?"));
            throw std::runtime_error(
                std::string("unimplemented framework selector ") +
                (receiverObject ? "-" : "+") + "[" + owner + " " + selectorName + "]: " +
                error.what());
        }

        const Class *resultClass = nullptr;
        Value guestResult = result;
        if (selectorName == "new" || selectorName == "alloc" || selectorName == "init") {
            auto *object = reinterpret_cast<Object *>(result);
            const auto guestAddress = ensureObjectAddress(memory, object);
            if (selectorName == "new" && receiverClass == autoreleasePoolClass_) {
                autoreleasePools_.emplace(guestAddress, AutoreleasePoolState{object, {}});
                activeAutoreleasePools_.push_back(guestAddress);
            }
            guestResult = guestAddress;
        } else if (selectorName == "class") {
            resultClass = reinterpret_cast<const Class *>(result);
            guestResult = ensureClassAddress(memory, resultClass);
        } else if (selectorName == "copy") {
            auto *returnedObject = reinterpret_cast<Object *>(result);
            guestResult = returnedObject ? ensureObjectAddress(memory, returnedObject) : 0;
        } else {
            const auto &state = guestState(memory);
            const auto *returnedObject = reinterpret_cast<const Object *>(result);
            const auto object = state.objectAddresses.find(const_cast<Object *>(returnedObject));
            if (object != state.objectAddresses.end())
                guestResult = object->second;
            else {
                const auto returnedClass = state.classAddresses.find(resultClass =
                    reinterpret_cast<const Class *>(result));
                if (returnedClass != state.classAddresses.end())
                    guestResult = returnedClass->second;
            }
        }
        registers.r[0] = static_cast<GuestAddress>(guestResult);
        if (rawReturn)
            *rawReturn = guestResult;
        if (receiverObject && objectForGuest(memory, receiverAddress) == receiverObject)
            synchronizeObject(memory, receiverObject, receiverAddress);
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

} // namespace radek::compat_runtime::objc
