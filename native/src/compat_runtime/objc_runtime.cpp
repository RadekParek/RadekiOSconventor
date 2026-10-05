#include "compat_runtime/objc_runtime.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace radek::compat_runtime::objc {

Object::Object(Class *objectClass, std::size_t instanceSlots)
    : isa(objectClass), ivars(instanceSlots, 0) {
    if (!objectClass || objectClass->isMetaclass)
        throw std::invalid_argument("Objective-C instances require a registered class");
}

void Runtime::requireKnownClassLocked(const Class *klass) const {
    if (!klass || knownClasses_.find(klass) == knownClasses_.end())
        throw std::invalid_argument("Objective-C class is not registered with this runtime");
}

Selector Runtime::selector(const std::string &name) {
    if (name.empty())
        throw std::invalid_argument("Objective-C selector name must not be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = selectors_.find(name);
    if (found != selectors_.end())
        return found->second;
    if (nextSelector_ == 0)
        throw std::overflow_error("Objective-C selector table is exhausted");
    const auto selectorValue = nextSelector_++;
    selectors_.emplace(name, selectorValue);
    return selectorValue;
}

Class *Runtime::registerClass(const std::string &name, Class *superclass,
                              std::vector<std::string> ivars) {
    if (name.empty())
        throw std::invalid_argument("Objective-C class name must not be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    if (classes_.find(name) != classes_.end() || classes_.find(name + "$metaclass") != classes_.end())
        throw std::runtime_error("duplicate or reserved Objective-C class name: " + name);
    if (superclass)
        requireKnownClassLocked(superclass);

    auto classObject = std::make_unique<Class>();
    auto metaclass = std::make_unique<Class>();
    classObject->name = name;
    classObject->superclass = superclass;
    classObject->ivarNames = std::move(ivars);
    classObject->instanceSlots = (superclass ? superclass->instanceSlots : 0) +
                                 classObject->ivarNames.size();
    metaclass->name = name + "$metaclass";
    metaclass->isMetaclass = true;
    metaclass->superclass = superclass ? superclass->metaclass : nullptr;

    auto *classPointer = classObject.get();
    auto *metaclassPointer = metaclass.get();
    classObject->metaclass = metaclassPointer;
    // The root metaclass is its own metaclass. Subclass metaclasses inherit
    // the root metaclass's identity, matching the core dispatch relationship.
    metaclass->metaclass = superclass ? superclass->metaclass->metaclass : metaclassPointer;

    classes_.emplace(name, std::move(classObject));
    classes_.emplace(name + "$metaclass", std::move(metaclass));
    knownClasses_.insert(classPointer);
    knownClasses_.insert(metaclassPointer);
    return classPointer;
}

Class *Runtime::findClass(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = classes_.find(name);
    if (found == classes_.end() || found->second->isMetaclass)
        return nullptr;
    return found->second.get();
}

void Runtime::addMethod(Class *klass, Selector selectorValue, IMP implementation) {
    if (selectorValue == 0 || !implementation)
        throw std::invalid_argument("Objective-C method requires a selector and implementation");
    std::lock_guard<std::mutex> lock(mutex_);
    requireKnownClassLocked(klass);
    klass->methods[selectorValue] = std::move(implementation);
    cache_.clear();
}

Object *Runtime::allocate(Class *klass) const {
    std::lock_guard<std::mutex> lock(mutex_);
    requireKnownClassLocked(klass);
    if (klass->isMetaclass)
        throw std::invalid_argument("cannot allocate an Objective-C metaclass instance");
    return new Object(klass, klass->instanceSlots);
}

IMP Runtime::lookupLocked(const Class *klass, Selector selectorValue) {
    if (!klass || selectorValue == 0)
        throw std::runtime_error("invalid Objective-C dispatch target");
    const auto key = std::make_pair(klass, selectorValue);
    const auto cached = cache_.find(key);
    if (cached != cache_.end())
        return cached->second;
    for (auto *current = klass; current; current = current->superclass) {
        const auto method = current->methods.find(selectorValue);
        if (method != current->methods.end()) {
            cache_.emplace(key, method->second);
            return method->second;
        }
    }
    throw std::runtime_error("unrecognized Objective-C selector " +
                             std::to_string(selectorValue) + " for " + klass->name);
}

Value Runtime::send(Object *object, Selector selectorValue, const Arguments &arguments) {
    if (!object)
        return 0;
    IMP implementation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requireKnownClassLocked(object->isa);
        implementation = lookupLocked(object->isa, selectorValue);
    }
    return implementation(Receiver{object, nullptr, false}, arguments);
}

Value Runtime::send(Class *classObject, Selector selectorValue, const Arguments &arguments) {
    if (!classObject)
        return 0;
    IMP implementation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requireKnownClassLocked(classObject);
        if (classObject->isMetaclass)
            throw std::invalid_argument("class dispatch requires an Objective-C class object");
        implementation = lookupLocked(classObject->metaclass, selectorValue);
    }
    return implementation(Receiver{nullptr, classObject, true}, arguments);
}

Object *retain(Object *object) {
    if (!object)
        return nullptr;
    auto current = object->references.load(std::memory_order_relaxed);
    for (;;) {
        if (current == 0 || current == std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Objective-C retain count is invalid or exhausted");
        if (object->references.compare_exchange_weak(current, current + 1,
                                                      std::memory_order_relaxed,
                                                      std::memory_order_relaxed))
            return object;
    }
}

void release(Object *object) {
    if (!object)
        return;
    auto current = object->references.load(std::memory_order_acquire);
    for (;;) {
        if (current == 0)
            throw std::runtime_error("Objective-C object was released more than once");
        if (object->references.compare_exchange_weak(current, current - 1,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
            if (current == 1)
                delete object;
            return;
        }
    }
}

thread_local AutoreleasePool *AutoreleasePool::current_ = nullptr;

AutoreleasePool::AutoreleasePool() : previous_(current_) { current_ = this; }

AutoreleasePool::~AutoreleasePool() {
    current_ = previous_;
    for (auto iterator = objects_.rbegin(); iterator != objects_.rend(); ++iterator)
        release(*iterator);
}

Object *AutoreleasePool::add(Object *object) {
    if (object && !current_)
        throw std::runtime_error("Objective-C autorelease requires an active pool");
    if (object)
        current_->objects_.push_back(object);
    return object;
}

Object *autorelease(Object *object) { return AutoreleasePool::add(object); }

} // namespace radek::compat_runtime::objc
