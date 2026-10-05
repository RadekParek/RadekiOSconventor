#include "compat_runtime/objc_runtime.hpp"

#include <cstdint>
#include <stdexcept>

#define CHECK(expression)                                                                             \
    do {                                                                                              \
        if (!(expression))                                                                            \
            throw std::runtime_error("CHECK failed: " #expression);                                   \
    } while (false)

namespace {
using namespace radek::compat_runtime::objc;

struct TrackedObject final : Object {
    explicit TrackedObject(Class *klass, int &destroyed) : Object(klass), destroyed_(destroyed) {}
    ~TrackedObject() override { ++destroyed_; }
    int &destroyed_;
};

void testClassesSelectorsAndDispatch() {
    Runtime runtime;
    auto *root = runtime.registerClass("NSObject", nullptr, {"rootValue"});
    auto *child = runtime.registerClass("GameObject", root, {"childValue"});
    CHECK(runtime.findClass("GameObject") == child);
    CHECK(child->superclass == root);
    CHECK(child->metaclass->superclass == root->metaclass);
    CHECK(root->metaclass->metaclass == root->metaclass);
    CHECK(child->metaclass->metaclass == root->metaclass);

    const auto value = runtime.selector("value");
    CHECK(value != 0);
    CHECK(value == runtime.selector("value"));
    runtime.addMethod(root, value, [](const Receiver &receiver, const Arguments &) {
        CHECK(receiver.object != nullptr);
        return receiver.object->ivars[0];
    });
    auto *object = runtime.allocate(child);
    CHECK(object->ivars.size() == 2);
    object->ivars[0] = 17;
    CHECK(runtime.send(object, value) == 17);

    // A child override invalidates the cached inherited implementation.
    runtime.addMethod(child, value, [](const Receiver &, const Arguments &arguments) {
        return arguments.empty() ? Value{23} : arguments.front();
    });
    CHECK(runtime.send(object, value) == 23);
    CHECK(runtime.send(object, value, {41}) == 41);

    const auto className = runtime.selector("className");
    runtime.addMethod(root->metaclass, className, [](const Receiver &receiver, const Arguments &) {
        CHECK(receiver.isClassMethod);
        CHECK(receiver.classObject != nullptr);
        return reinterpret_cast<Value>(receiver.classObject);
    });
    CHECK(runtime.send(child, className) == reinterpret_cast<Value>(child));

    bool missingThrew = false;
    try {
        runtime.send(object, runtime.selector("unimplementedSelector"));
    } catch (const std::runtime_error &) {
        missingThrew = true;
    }
    CHECK(missingThrew);
    radek::compat_runtime::objc::release(object);
}

void testRetainReleaseAndAutoreleasePools() {
    Runtime runtime;
    auto *klass = runtime.registerClass("Tracked");
    int destroyed = 0;
    auto *object = new TrackedObject(klass, destroyed);
    CHECK(retain(object) == object);
    CHECK(object->references.load() == 2);
    release(object);
    CHECK(destroyed == 0);
    release(object);
    CHECK(destroyed == 1);

    {
        AutoreleasePool outer;
        autorelease(new TrackedObject(klass, destroyed));
        {
            AutoreleasePool inner;
            AutoreleasePool::add(new TrackedObject(klass, destroyed));
        }
        CHECK(destroyed == 2);
    }
    CHECK(destroyed == 3);

    bool missingPoolFailedClosed = false;
    auto *unpooled = new TrackedObject(klass, destroyed);
    try {
        autorelease(unpooled);
    } catch (const std::runtime_error &) {
        missingPoolFailedClosed = true;
    }
    CHECK(missingPoolFailedClosed);
    // The caller still owns the object because it could not be added to a pool.
    release(unpooled);
    CHECK(destroyed == 4);
}
} // namespace

int main() {
    testClassesSelectorsAndDispatch();
    testRetainReleaseAndAutoreleasePools();
}
