#include "compat_runtime/cxx_string_shims.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/guest_state.hpp"
#include "compat_runtime/libsystem_shims.hpp"
#include "compat_runtime/shim_registry.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace radek::compat_runtime;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

/** Bump arena in one guest mapping, used for C strings, string objects, and stack slots. */
struct Arena {
    GuestAddressSpace &memory;
    GuestAddress base;
    GuestAddress cursor;

    explicit Arena(GuestAddressSpace &space)
        : memory(space), base(space.mapAny(1U << 16, MemoryPermission::Read | MemoryPermission::Write,
                                           "cxx-string-test-arena")),
          cursor(base) {}

    GuestAddress take(std::size_t bytes) {
        const GuestAddress at = cursor;
        cursor += static_cast<GuestAddress>((bytes + 7) & ~std::size_t{7});
        return at;
    }
    GuestAddress object() { return put32(0); }
    GuestAddress put32(std::uint32_t value) {
        const GuestAddress at = take(4);
        require(memory.write(at, &value, 4), "arena write");
        return at;
    }
    GuestAddress cstring(const std::string &text) {
        const GuestAddress at = take(text.size() + 1);
        require(memory.write(at, text.c_str(), text.size() + 1), "arena cstring write");
        return at;
    }
};

std::uint32_t word(GuestAddressSpace &memory, GuestAddress address) {
    std::uint32_t value = 0;
    require(memory.read(address, &value, 4), "guest word read");
    return value;
}

std::string text(GuestAddressSpace &memory, GuestAddress object) {
    const GuestAddress data = word(memory, object);
    const std::uint32_t length = word(memory, data - 12);
    std::string out(length, '\0');
    if (length != 0)
        require(memory.read(data, out.data(), length), "guest string read");
    return out;
}

GuestAddress dataOf(GuestAddressSpace &memory, GuestAddress object) { return word(memory, object); }

struct Harness {
    libsystem::ShimAdapter heap;
    cxx_string::ShimAdapter adapter{&heap};
    ShimRegistry registry;
    GuestAddressSpace memory;
    Arena arena{memory};
    GuestAddress emptyData = 0;

    Harness() {
        adapter.registerBindings(registry);
        const auto data = registry.resolve("__ZNSs4_Rep20_S_empty_rep_storageE");
        require(data.has_value() && data->resolveGuestAddress, "empty rep data symbol is bound");
        GuestAddress resolved = 0;
        std::string reason;
        require(data->resolveGuestAddress(memory, resolved, reason), "empty rep resolves: " + reason);
        emptyData = resolved + 12;
    }

    /** Calls a bound string symbol with up to four register arguments. */
    bool call(const std::string &symbol, std::initializer_list<std::uint32_t> args,
              std::string &reason, CpuRegisterState *out = nullptr, std::uint32_t stackWord = 0,
              bool hasStack = false) {
        const auto binding = registry.resolve(symbol);
        require(binding.has_value() && binding->invoke, "bound symbol " + symbol);
        CpuRegisterState regs;
        std::size_t index = 0;
        for (const std::uint32_t value : args)
            regs.r[index++] = value;
        if (hasStack) {
            const GuestAddress slot = arena.put32(stackWord);
            regs.r[13] = slot;
        }
        const bool ok = binding->invoke(regs, memory, reason);
        if (out)
            *out = regs;
        return ok;
    }

    void expectCall(const std::string &symbol, std::initializer_list<std::uint32_t> args,
                    CpuRegisterState *out = nullptr) {
        std::string reason;
        require(call(symbol, args, reason, out), symbol + " failed: " + reason);
    }

    GuestAddress makeFromCString(const std::string &value) {
        const GuestAddress object = arena.object();
        const GuestAddress src = arena.cstring(value);
        expectCall("__ZNSsC1EPKcRKSaIcE", {object, src, 0});
        return object;
    }
};

void testConstructFromCString() {
    Harness h;
    const GuestAddress s = h.makeFromCString("hello");
    require(text(h.memory, s) == "hello", "ctor copies characters");
    require(word(h.memory, dataOf(h.memory, s) - 12) == 5, "length in rep header");
    const GuestAddress empty = h.makeFromCString("");
    require(dataOf(h.memory, empty) == h.emptyData, "empty string shares the static empty representation");
}

void testCopyShareAndDestroy() {
    Harness h;
    const GuestAddress a = h.makeFromCString("shared");
    const GuestAddress b = h.arena.object();
    h.expectCall("__ZNSsC1ERKSs", {b, a});
    require(dataOf(h.memory, a) == dataOf(h.memory, b), "copy shares the representation");
    require(word(h.memory, dataOf(h.memory, a) - 12 + 8) == 1, "reference count after one copy");
    h.expectCall("__ZNSsD2Ev", {b});
    require(word(h.memory, dataOf(h.memory, a) - 12 + 8) == 0, "reference count back to sole owner");
    h.expectCall("__ZNSsD2Ev", {a});
}

void testAssignAndAppend() {
    Harness h;
    const GuestAddress s = h.makeFromCString("ab");
    h.expectCall("__ZNSs6appendEPKcm", {s, h.arena.cstring("cdef"), 4});
    require(text(h.memory, s) == "abcdef", "append with count");
    const GuestAddress other = h.makeFromCString("XYZ");
    h.expectCall("__ZNSs6appendERKSs", {s, other});
    require(text(h.memory, s) == "abcdefXYZ", "append string");
    h.expectCall("__ZNSs6assignEPKcm", {s, h.arena.cstring("qq"), 2});
    require(text(h.memory, s) == "qq", "assign with count");
    h.expectCall("__ZNSs6assignERKSs", {s, other});
    require(text(h.memory, s) == "XYZ", "assign from string");
}

void testSubscriptLeaksSharedString() {
    Harness h;
    const GuestAddress a = h.makeFromCString("own");
    const GuestAddress b = h.arena.object();
    h.expectCall("__ZNSsC1ERKSs", {b, a});
    CpuRegisterState out;
    h.expectCall("__ZNSsixEm", {a, 1}, &out);
    require(text(h.memory, a) == "own", "subscript keeps contents");
    require(out.r[0] == dataOf(h.memory, a) + 1, "subscript returns the element address");
    require(word(h.memory, dataOf(h.memory, a) - 12 + 8) == 0xffffffffu, "subscript leaks the string");
    require(text(h.memory, b) == "own", "other copy is unaffected");
}

void testResizeReserveAndReplace() {
    Harness h;
    const GuestAddress s = h.makeFromCString("abc");
    h.expectCall("__ZNSs6resizeEmc", {s, 6, 'x'});
    require(text(h.memory, s) == "abcxxx", "resize grows with fill");
    h.expectCall("__ZNSs6resizeEmc", {s, 2, 'x'});
    require(text(h.memory, s) == "ab", "resize shrinks");
    h.expectCall("__ZNSs7reserveEm", {s, 64});
    require(text(h.memory, s) == "ab", "reserve keeps contents");
}

void testReplaceSafeReadsStackLength() {
    Harness h;
    const GuestAddress s = h.makeFromCString("hello world");
    std::string reason;
    // _M_replace_safe(pos=6, len1=5, src="there", len2=5): len2 is the first stack word.
    require(h.call("__ZNSs15_M_replace_safeEmmPKcm", {s, 6, 5, h.arena.cstring("there")}, reason,
                   nullptr, 5, true),
            "replace_safe: " + reason);
    require(text(h.memory, s) == "hello there", "replace_safe splices the source");
}

void testReplaceCString() {
    Harness h;
    const GuestAddress s = h.makeFromCString("abcdef");
    h.expectCall("__ZNSs7replaceEmmPKc", {s, 1, 3, h.arena.cstring("XY")});
    require(text(h.memory, s) == "aXYef", "replace with C string");
}

void testFindAndCompare() {
    Harness h;
    const GuestAddress s = h.makeFromCString("the quick brown fox");
    CpuRegisterState out;
    h.expectCall("__ZNKSs4findEPKcm", {s, h.arena.cstring("quick"), 0}, &out);
    require(out.r[0] == 4, "find cstring");
    h.expectCall("__ZNKSs4findEPKcmm", {s, h.arena.cstring("fox"), 0, 3}, &out);
    require(out.r[0] == 16, "find with count");
    h.expectCall("__ZNKSs4findEPKcm", {s, h.arena.cstring("cat"), 0}, &out);
    require(out.r[0] == 0xffffffffu, "find miss returns npos");
    h.expectCall("__ZNKSs13find_first_ofEPKcm", {s, h.arena.cstring("qb"), 0}, &out);
    require(out.r[0] == 4, "find_first_of picks the first member");
    h.expectCall("__ZNKSs7compareEPKc", {s, h.arena.cstring("the quick brown fox")}, &out);
    require(out.r[0] == 0, "compare equal");
    h.expectCall("__ZNKSs7compareEPKc", {s, h.arena.cstring("the a")}, &out);
    require(static_cast<std::int32_t>(out.r[0]) > 0, "compare greater");
    const GuestAddress t = h.makeFromCString("apple");
    const GuestAddress u = h.makeFromCString("apricot");
    h.expectCall("__ZNKSs7compareERKSs", {t, u}, &out);
    require(static_cast<std::int32_t>(out.r[0]) < 0, "compare less");
}

void testSubstrReturnsBySret() {
    Harness h;
    const GuestAddress s = h.makeFromCString("abcdefgh");
    const GuestAddress slot = h.arena.object();
    h.expectCall("__ZNKSs6substrEmm", {slot, s, 2, 3});
    require(text(h.memory, slot) == "cde", "substr copies the window");
    const GuestAddress tail = h.arena.object();
    h.expectCall("__ZNKSs6substrEmm", {tail, s, 6, 100});
    require(text(h.memory, tail) == "gh", "substr clamps the count");
    std::string reason;
    const GuestAddress bad = h.arena.object();
    require(!h.call("__ZNKSs6substrEmm", {bad, s, 9, 1}, reason), "substr past end fails");
    require(!reason.empty(), "substr failure carries a reason");
}

void testSymbolSetIsPinned() {
    Harness h;
    require(h.adapter.boundSymbolCount() == 22, "string symbol count is pinned at 22");
}

} // namespace

int main() {
    const std::vector<std::pair<const char *, void (*)()>> tests = {
        {"construct from C string", testConstructFromCString},
        {"copy shares and destroy", testCopyShareAndDestroy},
        {"assign and append", testAssignAndAppend},
        {"subscript leaks shared string", testSubscriptLeaksSharedString},
        {"resize reserve replace", testResizeReserveAndReplace},
        {"replace_safe reads stack length", testReplaceSafeReadsStackLength},
        {"replace with C string", testReplaceCString},
        {"find and compare", testFindAndCompare},
        {"substr returns by sret", testSubstrReturnsBySret},
        {"symbol set is pinned", testSymbolSetIsPinned},
    };
    int failures = 0;
    for (const auto &[name, body] : tests) {
        try {
            body();
        } catch (const std::exception &error) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
        }
    }
    if (failures == 0)
        std::printf("cxx-string shims: %zu tests passed\n", tests.size());
    return failures == 0 ? 0 : 1;
}
