#include "ioscompat_registry.hpp"
#include "apple_time_compat.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define CHECK(expression)                                                                                   \
    do {                                                                                                     \
        if (!(expression))                                                                                  \
            throw std::runtime_error("CHECK failed: " #expression);                                         \
    } while (false)

int main() {
    using radek_compat::Kind;
    using radek_compat::lookup;
    using radek_compat::registerStub;

    // The four tested time shims are present and classified as verified.
    for (const char *name :
         {"_CFAbsoluteTimeGetCurrent", "_CACurrentMediaTime", "_mach_absolute_time", "_mach_timebase_info"}) {
        const auto *record = lookup(name);
        CHECK(record != nullptr);
        CHECK(record->kind == Kind::Verified);
        CHECK(record->handler != nullptr);
        CHECK(std::strcmp(radek_compat_classify(name), "verified") == 0);
    }

    // The verified handler addresses are the real implementation bodies.
    const auto *timeRecord = lookup("_CFAbsoluteTimeGetCurrent");
    double (*typed)();
    std::memcpy(&typed, &timeRecord->handler, sizeof(typed));
    const double now = typed();
    CHECK(now > 700000000.0 && now < 1100000000.0);

    // Unknown symbols are not classified until explicitly registered.
    CHECK(lookup("_glDrawArrays") == nullptr);
    CHECK(radek_compat_classify("_glDrawArrays") == nullptr);

    // Dynamic runtime hook registration for an unmapped Darwin import.
    CHECK(registerStub("_glDrawArrays"));
    const auto *stub = lookup("_glDrawArrays");
    CHECK(stub != nullptr);
    CHECK(stub->kind == Kind::Stub);
    CHECK(stub->handler != nullptr);
    CHECK(std::strcmp(radek_compat_classify("_glDrawArrays"), "stubbed") == 0);
    CHECK(std::string(stub->androidSymbol).rfind("radek_compat_stub_", 0) == 0);

    // Idempotent registration: same symbol keeps its first record.
    void (*firstHandler)() = stub->handler;
    CHECK(!registerStub("_glDrawArrays"));
    CHECK(lookup("_glDrawArrays")->handler == firstHandler);

    // Stubbed classification never satisfies verified lookups.
    CHECK(radek_compat_resolve("_glDrawArrays") != nullptr);
    CHECK(radek_compat_stub_call_count("_glDrawArrays") == 0);
    CHECK(radek_compat_stub_call_count("_CFAbsoluteTimeGetCurrent") == 0);

    // Invoking the stub trampoline is safe, observable, and returns zero.
    std::int64_t (*invocation)() = reinterpret_cast<std::int64_t (*)()>(stub->handler);
    CHECK(invocation() == 0);
    CHECK(invocation() == 0);
    CHECK(radek_compat_stub_call_count("_glDrawArrays") == 2);
    CHECK(radek_compat_stub_call_total() >= 2);

    // Invalid names are rejected by both registration and lookup.
    CHECK(!registerStub(""));
    CHECK(!registerStub(nullptr));
    CHECK(!registerStub("bad name with spaces"));
    CHECK(lookup("bad name with spaces") == nullptr);
    CHECK(lookup(nullptr) == nullptr);

    // Bulk registration and enumeration stay consistent.
    const unsigned long before = radek_compat_entry_count();
    std::vector<std::string> batch;
    for (int index = 0; index < 64; ++index) {
        batch.push_back("_radek_bulk_symbol_" + std::to_string(index));
        CHECK(registerStub(batch.back().c_str()));
    }
    CHECK(radek_compat_entry_count() == before + 64);
    bool sawBulk = false;
    for (unsigned long index = 0; index < radek_compat_entry_count(); ++index) {
        const char *darwin = nullptr;
        const char *android = nullptr;
        int kind = 0;
        void (*handler)() = nullptr;
        CHECK(radek_compat_entry_at(index, &darwin, &android, &kind, &handler) == 0);
        CHECK(darwin && android && handler);
        CHECK(kind == static_cast<int>(Kind::Verified) || kind == static_cast<int>(Kind::Stub));
        if (std::strcmp(darwin, "_radek_bulk_symbol_3") == 0) {
            sawBulk = true;
            CHECK(kind == static_cast<int>(Kind::Stub));
        }
    }
    CHECK(sawBulk);
    CHECK(radek_compat_entry_at(radek_compat_entry_count(), nullptr, nullptr, nullptr, nullptr) == -1);
    CHECK(radek_compat::verifiedCount() == 4);
    CHECK(radek_compat::stubCount() == radek_compat_entry_count() - 4);

    // Concurrent registration and lookup must not corrupt the registry.
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([worker] {
            for (int index = 0; index < 256; ++index) {
                std::string name =
                    "_radek_thread_symbol_" + std::to_string(worker) + "_" + std::to_string(index);
                registerStub(name.c_str());
                CHECK(lookup(name.c_str()) != nullptr);
            }
        });
    }
    for (auto &thread : workers)
        thread.join();
    CHECK(lookup("_radek_thread_symbol_3_255") != nullptr);

    return 0;
}
