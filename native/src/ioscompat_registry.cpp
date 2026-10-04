#include "ioscompat_registry.hpp"
#include "apple_time_compat.h"

#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using radek_compat::Kind;
using radek_compat::Record;

struct OwnedRecord {
    std::string darwin;
    std::string android;
    Kind kind;
    void (*handler)();
    std::size_t slot; // stub counter slot; SIZE_MAX for verified/overflow
};

std::mutex g_mutex;
std::deque<OwnedRecord> g_records; // deque: element addresses stay stable
std::unordered_map<std::string, std::size_t> g_index;
std::size_t g_nextPoolSlot = 0;

std::array<std::atomic<std::uint64_t>, radek_compat::kStubPoolSize> g_stubCalls{};
std::atomic<std::uint64_t> g_overflowCalls{0};

std::int64_t noteStubCall(std::size_t slot) {
    if (slot < radek_compat::kStubPoolSize)
        g_stubCalls[slot].fetch_add(1, std::memory_order_relaxed);
    else
        g_overflowCalls.fetch_add(1, std::memory_order_relaxed);
    // Documented safe default. Stubs never implement the Darwin API; they make
    // accidental invocations observable instead of undefined.
    return 0;
}

template <std::size_t Slot> std::int64_t stubTrampoline() { return noteStubCall(Slot); }

std::int64_t overflowTrampoline() { return noteStubCall(radek_compat::kStubPoolSize); }

// Fold expressions are seeded in 128-wide chunks: clang rejects a single fold
// wider than its expression-nesting limit (256), while g++ accepts it.
constexpr std::size_t kSeedChunkWidth = 128;
static_assert(radek_compat::kStubPoolSize % kSeedChunkWidth == 0, "pool must split into chunks");

template <std::size_t Base, std::size_t... Off>
void seedChunk(std::array<void (*)(), radek_compat::kStubPoolSize> &table,
               std::index_sequence<Off...>) {
    ((table[Base + Off] = reinterpret_cast<void (*)()>(&stubTrampoline<Base + Off>)), ...);
}

template <std::size_t... Chunk>
void seedAll(std::array<void (*)(), radek_compat::kStubPoolSize> &table,
             std::index_sequence<Chunk...>) {
    (seedChunk<Chunk * kSeedChunkWidth>(table, std::make_index_sequence<kSeedChunkWidth>{}), ...);
}

const std::array<void (*)(), radek_compat::kStubPoolSize> &trampolines() {
    static const std::array<void (*)(), radek_compat::kStubPoolSize> table = [] {
        std::array<void (*)(), radek_compat::kStubPoolSize> filled{};
        seedAll(filled,
                std::make_index_sequence<radek_compat::kStubPoolSize / kSeedChunkWidth>{});
        return filled;
    }();
    return table;
}

template <typename Function> void (*toGeneric(Function function))() {
    void (*generic)();
    static_assert(sizeof(generic) == sizeof(function), "function pointer size mismatch");
    std::memcpy(&generic, &function, sizeof(generic));
    return generic;
}

struct VerifiedSeed {
    const char *darwin;
    const char *android;
    void (*handler)();
};

const VerifiedSeed kVerifiedSeeds[] = {
    {"_CFAbsoluteTimeGetCurrent", "CFAbsoluteTimeGetCurrent", toGeneric(&CFAbsoluteTimeGetCurrent)},
    {"_CACurrentMediaTime", "CACurrentMediaTime", toGeneric(&CACurrentMediaTime)},
    {"_mach_absolute_time", "mach_absolute_time", toGeneric(&mach_absolute_time)},
    {"_mach_timebase_info", "mach_timebase_info", toGeneric(&mach_timebase_info)},
};

bool validSymbolName(const char *name) {
    if (!name || !*name || std::strlen(name) > 512)
        return false;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(name); *cursor; ++cursor) {
        const bool alpha = (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z');
        const bool digit = *cursor >= '0' && *cursor <= '9';
        if (!alpha && !digit && *cursor != '_' && *cursor != '$' && *cursor != '.')
            return false;
    }
    return true;
}

void seedOnce() {
    static const bool seeded = [] {
        for (const auto &seed : kVerifiedSeeds) {
            g_records.push_back({seed.darwin, seed.android, Kind::Verified, seed.handler, SIZE_MAX});
            g_index.emplace(seed.darwin, g_records.size() - 1);
        }
        return true;
    }();
    (void)seeded;
}

Record publishRecord(const OwnedRecord &owned) {
    return Record{owned.darwin.c_str(), owned.android.c_str(), owned.kind, owned.handler};
}

} // namespace

namespace radek_compat {

const Record *lookup(const char *darwinSymbol) {
    if (!validSymbolName(darwinSymbol))
        return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    seedOnce();
    auto found = g_index.find(darwinSymbol);
    if (found == g_index.end())
        return nullptr;
    static thread_local Record record{};
    record = publishRecord(g_records[found->second]);
    return &record;
}

bool registerStub(const char *darwinSymbol) {
    if (!validSymbolName(darwinSymbol))
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    seedOnce();
    if (g_index.count(darwinSymbol))
        return false;
    const std::size_t slot = g_nextPoolSlot < kStubPoolSize ? g_nextPoolSlot : kStubPoolSize;
    if (g_nextPoolSlot < kStubPoolSize)
        ++g_nextPoolSlot;
    void (*handler)() = slot < kStubPoolSize ? trampolines()[slot]
                                             : reinterpret_cast<void (*)()>(&overflowTrampoline);
    std::string android = "radek_compat_stub_";
    android += std::to_string(slot);
    g_records.push_back({darwinSymbol, std::move(android), Kind::Stub, handler, slot});
    g_index.emplace(darwinSymbol, g_records.size() - 1);
    return true;
}

std::size_t size() {
    std::lock_guard<std::mutex> lock(g_mutex);
    seedOnce();
    return g_records.size();
}

const Record *at(std::size_t index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    seedOnce();
    if (index >= g_records.size())
        return nullptr;
    static thread_local Record record{};
    record = publishRecord(g_records[index]);
    return &record;
}

std::size_t verifiedCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    seedOnce();
    std::size_t count = 0;
    for (const auto &record : g_records)
        count += record.kind == Kind::Verified;
    return count;
}

std::size_t stubCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    seedOnce();
    std::size_t count = 0;
    for (const auto &record : g_records)
        count += record.kind == Kind::Stub;
    return count;
}

std::uint64_t stubCallTotal() {
    std::uint64_t total = g_overflowCalls.load(std::memory_order_relaxed);
    for (const auto &counter : g_stubCalls)
        total += counter.load(std::memory_order_relaxed);
    return total;
}

std::uint64_t stubCallCount(const char *darwinSymbol) {
    std::lock_guard<std::mutex> lock(g_mutex);
    seedOnce();
    auto found = g_index.find(darwinSymbol ? darwinSymbol : "");
    if (found == g_index.end())
        return 0;
    const OwnedRecord &record = g_records[found->second];
    if (record.kind != Kind::Stub)
        return 0;
    if (record.slot >= kStubPoolSize)
        return g_overflowCalls.load(std::memory_order_relaxed);
    return g_stubCalls[record.slot].load(std::memory_order_relaxed);
}

} // namespace radek_compat

// dlsym-visible C ABI for dynamic runtime hook registration and triage.
extern "C" {

const char *radek_compat_classify(const char *darwin_symbol) {
    const Record *record = radek_compat::lookup(darwin_symbol);
    if (!record)
        return nullptr;
    return record->kind == Kind::Verified ? "verified" : "stubbed";
}

// Resolution address for a registered symbol, or nullptr.
void (*radek_compat_resolve(const char *darwin_symbol))(void) {
    const Record *record = radek_compat::lookup(darwin_symbol);
    return record ? record->handler : nullptr;
}

int radek_compat_register_stub(const char *darwin_symbol) {
    return radek_compat::registerStub(darwin_symbol) ? 1 : 0;
}

unsigned long radek_compat_entry_count(void) { return radek_compat::size(); }

// Copies one entry into out parameters; returns 0 on success, -1 out of range.
int radek_compat_entry_at(unsigned long index, const char **darwin_symbol, const char **android_symbol,
                          int *kind, void (**handler)(void)) {
    const Record *record = radek_compat::at(index);
    if (!record)
        return -1;
    if (darwin_symbol)
        *darwin_symbol = record->darwinSymbol;
    if (android_symbol)
        *android_symbol = record->androidSymbol;
    if (kind)
        *kind = static_cast<int>(record->kind);
    if (handler)
        *handler = record->handler;
    return 0;
}

unsigned long long radek_compat_stub_call_total(void) { return radek_compat::stubCallTotal(); }

unsigned long long radek_compat_stub_call_count(const char *darwin_symbol) {
    return radek_compat::stubCallCount(darwin_symbol);
}

} // extern "C"
