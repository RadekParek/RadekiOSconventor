#include "compat_runtime/cxx_string_shims.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace radek::compat_runtime::cxx_string {

namespace {

constexpr GuestAddress kCalloutEnd = 0xf0150000u;
constexpr std::uint32_t kRepHeaderBytes = 12; // length, capacity, reference word
constexpr std::uint32_t kLeakedReference = 0xffffffffu;
constexpr std::uint64_t kNpos = 0xffffffffu;
constexpr std::uint64_t kMaxLength = 0x3fffffffu;
constexpr std::uint64_t kMaxCopyBytes = 1u << 26;
constexpr std::uint32_t kMaxCStringScan = 1u << 24;

using Registers = CpuRegisterState;

/** Guest access for one call: memory, heap, the shared empty representation, and the reason slot. */
struct Guest {
    GuestAddressSpace &memory;
    libsystem::ShimAdapter &heap;
    std::string &reason;
    GuestAddress empty; // address of the empty representation header

    bool fail(const std::string &message) const {
        reason = message;
        return false;
    }
    bool word(GuestAddress address, std::uint32_t &value) const {
        std::array<std::uint8_t, 4> bytes{};
        if (!memory.read(address, bytes.data(), bytes.size()))
            return false;
        value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                (static_cast<std::uint32_t>(bytes[2]) << 16) |
                (static_cast<std::uint32_t>(bytes[3]) << 24);
        return true;
    }
    bool setWord(GuestAddress address, std::uint32_t value) const {
        const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(value & 0xff), static_cast<std::uint8_t>((value >> 8) & 0xff),
            static_cast<std::uint8_t>((value >> 16) & 0xff),
            static_cast<std::uint8_t>((value >> 24) & 0xff)};
        return memory.write(address, bytes.data(), bytes.size());
    }
    bool bytesAt(GuestAddress address, std::uint64_t count, std::vector<std::uint8_t> &out) const {
        if (count > kMaxCopyBytes)
            return fail("string operation exceeds the bounded copy size");
        out.resize(static_cast<std::size_t>(count));
        if (count == 0)
            return true;
        return memory.read(address, out.data(), out.size()) ||
               fail("string bytes are not readable guest memory");
    }
    bool writeBytes(GuestAddress address, const std::vector<std::uint8_t> &source) const {
        return source.empty() || memory.write(address, source.data(), source.size());
    }
};

/** Snapshot of one string object and its representation header. */
struct View {
    GuestAddress object = 0;
    GuestAddress data = 0;
    std::uint32_t length = 0;
    std::uint32_t capacity = 0;
    std::uint32_t reference = 0;

    GuestAddress rep() const { return data - kRepHeaderBytes; }
    bool leaked() const { return reference == kLeakedReference; }
    bool shared() const { return reference != 0 && !leaked(); }
};

bool view(const Guest &g, GuestAddress object, View &out) {
    View v;
    v.object = object;
    if (!g.word(object, v.data))
        return g.fail("string object is not readable guest memory");
    if (v.data < kRepHeaderBytes)
        return g.fail("string object has no libstdc++ representation");
    if (!g.word(v.rep(), v.length) || !g.word(v.rep() + 4, v.capacity) ||
        !g.word(v.rep() + 8, v.reference))
        return g.fail("string representation header is not readable guest memory");
    if (v.length > v.capacity)
        return g.fail("string representation is corrupt: length exceeds capacity");
    out = v;
    return true;
}

bool readCString(const Guest &g, GuestAddress address, std::vector<std::uint8_t> &out) {
    out.clear();
    if (address == 0)
        return g.fail("C string argument is null");
    for (std::uint32_t i = 0; i < kMaxCStringScan; ++i) {
        std::uint8_t byte = 0;
        if (!g.memory.read(address + i, &byte, 1))
            return g.fail("C string argument is not readable guest memory");
        if (byte == 0)
            return true;
        out.push_back(byte);
    }
    return g.fail("C string argument is not terminated within the bounded scan");
}

/** Allocates a representation with room for `capacity` characters, libstdc++ `_S_create` growth rule. */
bool createRep(const Guest &g, std::uint64_t capacity, std::uint64_t oldCapacity, GuestAddress &rep) {
    if (capacity > oldCapacity && capacity < 2 * oldCapacity)
        capacity = 2 * oldCapacity;
    if (capacity > kMaxLength)
        return g.fail("string length exceeds the 32-bit length limit");
    rep = g.heap.guestAllocate(g.memory, static_cast<std::size_t>(kRepHeaderBytes + capacity + 1));
    if (rep == 0)
        return g.fail("string allocation could not be satisfied from the guest heap");
    if (!g.setWord(rep, 0) || !g.setWord(rep + 4, static_cast<std::uint32_t>(capacity)) ||
        !g.setWord(rep + 8, 0))
        return g.fail("string representation could not be written to guest memory");
    return true;
}

/** libstdc++ `_M_set_length_and_sharable`: sets length, marks sharable, writes the terminator. */
bool setLength(const Guest &g, GuestAddress data, std::uint64_t length) {
    const GuestAddress rep = data - kRepHeaderBytes;
    if (rep == g.empty) {
        if (length != 0)
            return g.fail("the shared empty representation cannot hold characters");
    } else if (!g.setWord(rep + 8, 0)) {
        return g.fail("string reference word is not writable guest memory");
    }
    if (!g.setWord(rep, static_cast<std::uint32_t>(length)))
        return g.fail("string length is not writable guest memory");
    const std::uint8_t terminator = 0;
    return g.memory.write(data + static_cast<GuestAddress>(length), &terminator, 1);
}

/** libstdc++ `_M_dispose`: drops one reference; the block is released when the last reference goes. */
bool dispose(const Guest &g, GuestAddress data) {
    const GuestAddress rep = data - kRepHeaderBytes;
    std::uint32_t raw = 0;
    if (!g.word(rep + 8, raw))
        return g.fail("string reference word is not readable guest memory");
    if (raw == kLeakedReference)
        return true; // leaked (including the empty representation) is never released
    if (raw == 0) {
        g.heap.guestRelease(g.memory, rep); // sole owner
        return true;
    }
    if (!g.setWord(rep + 8, raw - 1)) // one fewer sharer
        return g.fail("string reference word is not writable guest memory");
    return true;
}

/** Points `object` at a fresh string holding `bytes`. */
bool initialize(const Guest &g, GuestAddress object, const std::vector<std::uint8_t> &bytes) {
    GuestAddress data = g.empty + kRepHeaderBytes;
    if (!bytes.empty()) {
        GuestAddress rep = 0;
        if (!createRep(g, bytes.size(), 0, rep))
            return false;
        data = rep + kRepHeaderBytes;
        if (!g.writeBytes(data, bytes) || !setLength(g, data, bytes.size()))
            return false;
    }
    if (!g.setWord(object, data))
        return g.fail("string object is not writable guest memory");
    return true;
}

/** libstdc++ `_M_mutate`: makes room for `len2` characters in place of `len1` at `pos`. */
bool mutate(const Guest &g, GuestAddress object, std::uint64_t pos, std::uint64_t len1,
            std::uint64_t len2) {
    View v;
    if (!view(g, object, v))
        return false;
    if (pos > v.length)
        return g.fail("string position is past the end of the string");
    len1 = std::min<std::uint64_t>(len1, v.length - pos);
    const std::uint64_t oldSize = v.length;
    const std::uint64_t newSize = oldSize - len1 + len2;
    if (newSize > kMaxLength)
        return g.fail("string length exceeds the 32-bit length limit");
    const std::uint64_t howMuch = oldSize - pos - len1;
    GuestAddress data = v.data;
    if (newSize > v.capacity || v.shared()) {
        GuestAddress rep = 0;
        if (!createRep(g, newSize, v.capacity, rep))
            return false;
        const GuestAddress fresh = rep + kRepHeaderBytes;
        std::vector<std::uint8_t> prefix;
        std::vector<std::uint8_t> suffix;
        if (!g.bytesAt(v.data, pos, prefix) || !g.bytesAt(v.data + pos + len1, howMuch, suffix))
            return false;
        if (!g.writeBytes(fresh, prefix) ||
            !g.writeBytes(fresh + static_cast<GuestAddress>(pos + len2), suffix))
            return false;
        if (!dispose(g, v.data))
            return false;
        if (!g.setWord(object, fresh))
            return g.fail("string object is not writable guest memory");
        data = fresh;
    } else if (howMuch != 0 && len1 != len2) {
        std::vector<std::uint8_t> suffix;
        if (!g.bytesAt(v.data + pos + len1, howMuch, suffix) ||
            !g.writeBytes(v.data + pos + len2, suffix))
            return false;
    }
    return setLength(g, data, newSize);
}

/** Replaces `len1` characters at `pos` with `source`. `source` is already a host copy, so aliasing is safe. */
bool replaceWith(const Guest &g, GuestAddress object, std::uint64_t pos, std::uint64_t len1,
                 const std::vector<std::uint8_t> &source) {
    if (!mutate(g, object, pos, len1, source.size()))
        return false;
    View v;
    if (!view(g, object, v))
        return false;
    return g.writeBytes(v.data + static_cast<GuestAddress>(pos), source);
}

/** Copy-construct or assign from another string: share its representation, or clone a leaked one. */
bool grab(const Guest &g, GuestAddress object, const View &other) {
    if (other.data == g.empty + kRepHeaderBytes)
        return g.setWord(object, other.data) || g.fail("string object is not writable guest memory");
    if (other.leaked()) {
        std::vector<std::uint8_t> bytes;
        return g.bytesAt(other.data, other.length, bytes) && initialize(g, object, bytes);
    }
    if (!g.setWord(other.rep() + 8, other.reference + 1))
        return g.fail("string reference word is not writable guest memory");
    return g.setWord(object, other.data) || g.fail("string object is not writable guest memory");
}

/** libstdc++ `_M_leak_hard`: the string gets its own representation and is never shared again. */
bool leak(const Guest &g, GuestAddress object) {
    View v;
    if (!view(g, object, v))
        return false;
    if (v.data == g.empty + kRepHeaderBytes || v.leaked())
        return true;
    if (v.shared() && !mutate(g, object, 0, 0, 0))
        return false;
    if (!view(g, object, v))
        return false;
    return g.setWord(v.rep() + 8, kLeakedReference) || g.fail("string reference word is not writable");
}

bool contents(const Guest &g, GuestAddress object, std::vector<std::uint8_t> &out) {
    View v;
    return view(g, object, v) && g.bytesAt(v.data, v.length, out);
}

int compareBytes(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) {
    const std::size_t common = std::min(a.size(), b.size());
    const int order = common == 0 ? 0 : std::memcmp(a.data(), b.data(), common);
    if (order != 0)
        return order < 0 ? -1 : 1;
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

std::uint64_t findBytes(const std::vector<std::uint8_t> &haystack,
                        const std::vector<std::uint8_t> &needle, std::uint64_t pos) {
    if (needle.empty())
        return pos <= haystack.size() ? pos : kNpos;
    if (needle.size() > haystack.size())
        return kNpos;
    for (std::uint64_t i = pos; i + needle.size() <= haystack.size(); ++i) {
        if (std::memcmp(haystack.data() + i, needle.data(), needle.size()) == 0)
            return i;
    }
    return kNpos;
}

std::uint64_t findFirstOf(const std::vector<std::uint8_t> &haystack,
                          const std::vector<std::uint8_t> &set, std::uint64_t pos) {
    for (std::uint64_t i = pos; i < haystack.size(); ++i) {
        if (std::find(set.begin(), set.end(), haystack[static_cast<std::size_t>(i)]) != set.end())
            return i;
    }
    return kNpos;
}

} // namespace

ShimAdapter::ShimAdapter(libsystem::ShimAdapter *heap) : heap_(heap) {}

bool ShimAdapter::enter(GuestAddressSpace &memory, std::string &reason, GuestAddress &empty) {
    ++calls_;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = emptyReps_.find(&memory);
    if (found != emptyReps_.end()) {
        empty = found->second;
        return true;
    }
    const auto base = memory.mapAny(4096, MemoryPermission::Read | MemoryPermission::Write,
                                    "libstdc++-empty-string-representation");
    if (base == 0) {
        reason = "the empty string representation could not be mapped";
        return false;
    }
    // Header {length 0, capacity 0, reference -1 (leaked)} followed by the terminator.
    Guest setup{memory, *heap_, reason, base};
    if (!setup.setWord(base, 0) || !setup.setWord(base + 4, 0) ||
        !setup.setWord(base + 8, kLeakedReference)) {
        reason = "the empty string representation could not be written";
        return false;
    }
    const std::uint8_t terminator = 0;
    if (!memory.write(base + kRepHeaderBytes, &terminator, 1)) {
        reason = "the empty string representation could not be terminated";
        return false;
    }
    emptyReps_.emplace(&memory, base);
    empty = base;
    return true;
}

void ShimAdapter::registerFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName, Invoke invoke) {
    if (nextCallout_ > kCalloutEnd - 4U)
        throw std::overflow_error("libstdc++ string callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libstdc++.6.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    binding.invoke = std::move(invoke);
    nextCallout_ += 4U;
    registry.registerBinding(std::move(binding));
    ++boundSymbols_;
}

void ShimAdapter::registerData(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(GuestAddressSpace &, GuestAddress &, std::string &)> resolve) {
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libstdc++.6.dylib";
    binding.adapterName = adapterName;
    binding.resolveGuestAddress = std::move(resolve);
    registry.registerBinding(std::move(binding));
    ++boundSymbols_;
}

void ShimAdapter::registerBindings(ShimRegistry &registry) {
// Every entry point enters through enter(), which supplies the shared empty representation.
#define RADEK_CXX_STRING_ENTRY(body)                                                       \
    [this](Registers &r, GuestAddressSpace &memory, std::string &reason) -> bool {        \
        GuestAddress empty = 0;                                                             \
        if (!enter(memory, reason, empty))                                                  \
            return false;                                                                   \
        const Guest g{memory, *heap_, reason, empty};                                       \
        (void)r;                                                                            \
        body                                                                                \
    }

    registerData(registry, "__ZNSs4_Rep20_S_empty_rep_storageE", "libstdc++-empty-rep-storage",
                 [this](GuestAddressSpace &memory, GuestAddress &address, std::string &reason) {
                     GuestAddress empty = 0;
                     if (!enter(memory, reason, empty))
                         return false;
                     address = empty;
                     return true;
                 });

    // Constructors and destructor.
    registerFunction(registry, "__ZNSsC1EPKcRKSaIcE", "libstdc++-string-ctor-cstr",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> bytes;
            return readCString(g, r.r[1], bytes) && initialize(g, r.r[0], bytes);
        }));
    registerFunction(registry, "__ZNSsC1ERKSs", "libstdc++-string-ctor-copy",
        RADEK_CXX_STRING_ENTRY({
            View other;
            return view(g, r.r[1], other) && grab(g, r.r[0], other);
        }));
    registerFunction(registry, "__ZNSsD2Ev", "libstdc++-string-dtor",
        RADEK_CXX_STRING_ENTRY({
            View v;
            return view(g, r.r[0], v) && dispose(g, v.data);
        }));

    // Element access and assignment.
    registerFunction(registry, "__ZNSsixEm", "libstdc++-string-subscript",
        RADEK_CXX_STRING_ENTRY({
            View v;
            if (!view(g, r.r[0], v))
                return false;
            if (r.r[1] > v.length)
                return g.fail("string subscript is past the end of the string");
            if (!leak(g, r.r[0]) || !view(g, r.r[0], v))
                return false;
            r.r[0] = v.data + r.r[1];
            return true;
        }));
    registerFunction(registry, "__ZNSs6assignEPKcm", "libstdc++-string-assign-cstr-n",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> bytes;
            return g.bytesAt(r.r[1], r.r[2], bytes) && replaceWith(g, r.r[0], 0, std::numeric_limits<std::uint64_t>::max(), bytes);
        }));
    registerFunction(registry, "__ZNSs6assignERKSs", "libstdc++-string-assign-copy",
        RADEK_CXX_STRING_ENTRY({
            View other;
            View self;
            if (!view(g, r.r[1], other) || !view(g, r.r[0], self))
                return false;
            if (self.data == other.data)
                return true;
            return dispose(g, self.data) && grab(g, r.r[0], other);
        }));

    // Append, resize, reserve, and the replace family.
    registerFunction(registry, "__ZNSs6appendEPKcm", "libstdc++-string-append-cstr-n",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> bytes;
            View v;
            if (!g.bytesAt(r.r[1], r.r[2], bytes) || !view(g, r.r[0], v))
                return false;
            return replaceWith(g, r.r[0], v.length, 0, bytes);
        }));
    registerFunction(registry, "__ZNSs6appendERKSs", "libstdc++-string-append-copy",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> bytes;
            View v;
            if (!contents(g, r.r[1], bytes) || !view(g, r.r[0], v))
                return false;
            return replaceWith(g, r.r[0], v.length, 0, bytes);
        }));
    registerFunction(registry, "__ZNSs6resizeEmc", "libstdc++-string-resize",
        RADEK_CXX_STRING_ENTRY({
            View v;
            if (!view(g, r.r[0], v))
                return false;
            const std::uint64_t target = r.r[1];
            if (target > v.length) {
                const std::vector<std::uint8_t> fill(
                    static_cast<std::size_t>(target - v.length), static_cast<std::uint8_t>(r.r[2] & 0xff));
                return replaceWith(g, r.r[0], v.length, 0, fill);
            }
            return target == v.length || mutate(g, r.r[0], target, v.length - target, 0);
        }));
    registerFunction(registry, "__ZNSs7reserveEm", "libstdc++-string-reserve",
        RADEK_CXX_STRING_ENTRY({
            View v;
            if (!view(g, r.r[0], v))
                return false;
            const std::uint64_t requested = std::max<std::uint64_t>(r.r[1], v.length);
            if (requested == v.capacity && !v.shared())
                return true;
            GuestAddress rep = 0;
            if (!createRep(g, requested, v.capacity, rep))
                return false;
            const GuestAddress fresh = rep + kRepHeaderBytes;
            std::vector<std::uint8_t> bytes;
            if (!g.bytesAt(v.data, v.length, bytes) || !g.writeBytes(fresh, bytes))
                return false;
            if (!setLength(g, fresh, v.length) || !dispose(g, v.data))
                return false;
            return g.setWord(r.r[0], fresh) || g.fail("string object is not writable guest memory");
        }));
    registerFunction(registry, "__ZNSs9_M_mutateEmmm", "libstdc++-string-mutate",
        RADEK_CXX_STRING_ENTRY({
            return mutate(g, r.r[0], r.r[1], r.r[2], r.r[3]);
        }));
    // _M_replace_safe(pos, len1, s, len2): the fifth word, len2, is the first stack argument.
    registerFunction(registry, "__ZNSs15_M_replace_safeEmmPKcm", "libstdc++-string-replace-safe",
        RADEK_CXX_STRING_ENTRY({
            std::uint32_t len2 = 0;
            std::vector<std::uint8_t> bytes;
            if (!g.word(r.r[13], len2) || !g.bytesAt(r.r[3], len2, bytes))
                return g.fail("replace source is not readable guest memory");
            return replaceWith(g, r.r[0], r.r[1], r.r[2], bytes);
        }));
    registerFunction(registry, "__ZNSs7replaceEmmPKc", "libstdc++-string-replace-cstr",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> bytes;
            return readCString(g, r.r[3], bytes) && replaceWith(g, r.r[0], r.r[1], r.r[2], bytes);
        }));
    registerFunction(registry, "__ZNSs12_M_leak_hardEv", "libstdc++-string-leak-hard",
        RADEK_CXX_STRING_ENTRY({
            return leak(g, r.r[0]);
        }));

    // Static representation factory: returns the representation header, not the characters.
    registerFunction(registry, "__ZNSs4_Rep9_S_createEmmRKSaIcE", "libstdc++-string-rep-create",
        RADEK_CXX_STRING_ENTRY({
            GuestAddress rep = 0;
            if (!createRep(g, r.r[0], r.r[1], rep))
                return false;
            r.r[0] = rep;
            return true;
        }));

    // Searches and comparisons.
    registerFunction(registry, "__ZNKSs4findEPKcmm", "libstdc++-string-find-n",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> haystack;
            std::vector<std::uint8_t> needle;
            // find(const char *s, pos, n): the needle is the first n characters of s.
            if (!contents(g, r.r[0], haystack) || !g.bytesAt(r.r[1], r.r[3], needle))
                return false;
            r.r[0] = static_cast<std::uint32_t>(findBytes(haystack, needle, r.r[2]));
            return true;
        }));
    registerFunction(registry, "__ZNKSs4findEPKcm", "libstdc++-string-find-cstr",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> haystack;
            std::vector<std::uint8_t> needle;
            if (!contents(g, r.r[0], haystack) || !readCString(g, r.r[1], needle))
                return false;
            r.r[0] = static_cast<std::uint32_t>(findBytes(haystack, needle, r.r[2]));
            return true;
        }));
    registerFunction(registry, "__ZNKSs13find_first_ofEPKcm", "libstdc++-string-find-first-of",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> haystack;
            std::vector<std::uint8_t> set;
            if (!contents(g, r.r[0], haystack) || !readCString(g, r.r[1], set))
                return false;
            r.r[0] = static_cast<std::uint32_t>(findFirstOf(haystack, set, r.r[2]));
            return true;
        }));
    // substr returns the string by value: the hidden return slot is r0, `this` is r1.
    registerFunction(registry, "__ZNKSs6substrEmm", "libstdc++-string-substr",
        RADEK_CXX_STRING_ENTRY({
            View v;
            if (!view(g, r.r[1], v))
                return false;
            if (r.r[2] > v.length)
                return g.fail("substr position is past the end of the string");
            const std::uint64_t count = std::min<std::uint64_t>(r.r[3], v.length - r.r[2]);
            std::vector<std::uint8_t> bytes;
            if (!g.bytesAt(v.data + static_cast<GuestAddress>(r.r[2]), count, bytes))
                return false;
            const GuestAddress slot = r.r[0];
            return initialize(g, slot, bytes);
        }));
    registerFunction(registry, "__ZNKSs7compareEPKc", "libstdc++-string-compare-cstr",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> self;
            std::vector<std::uint8_t> other;
            if (!contents(g, r.r[0], self) || !readCString(g, r.r[1], other))
                return false;
            r.r[0] = static_cast<std::uint32_t>(compareBytes(self, other));
            return true;
        }));
    registerFunction(registry, "__ZNKSs7compareERKSs", "libstdc++-string-compare-copy",
        RADEK_CXX_STRING_ENTRY({
            std::vector<std::uint8_t> self;
            std::vector<std::uint8_t> other;
            if (!contents(g, r.r[0], self) || !contents(g, r.r[1], other))
                return false;
            r.r[0] = static_cast<std::uint32_t>(compareBytes(self, other));
            return true;
        }));

#undef RADEK_CXX_STRING_ENTRY
}

} // namespace radek::compat_runtime::cxx_string
