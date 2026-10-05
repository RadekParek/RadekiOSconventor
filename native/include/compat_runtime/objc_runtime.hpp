#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace radek::compat_runtime::objc {

using Selector = std::uint32_t;
using Value = std::uintptr_t;
using Arguments = std::vector<Value>;

struct Class;

struct Object {
    Class *isa = nullptr;
    std::atomic<std::uint32_t> references{1};
    std::vector<Value> ivars;

    explicit Object(Class *objectClass, std::size_t instanceSlots = 0);
    virtual ~Object() = default;
    Object(const Object &) = delete;
    Object &operator=(const Object &) = delete;
};

struct Receiver {
    Object *object = nullptr;
    Class *classObject = nullptr;
    bool isClassMethod = false;
};

using IMP = std::function<Value(const Receiver &, const Arguments &)>;

struct Class {
    std::string name;
    Class *superclass = nullptr;
    Class *metaclass = nullptr;
    bool isMetaclass = false;
    std::map<Selector, IMP> methods;
    std::vector<std::string> ivarNames;
    std::size_t instanceSlots = 0;
};

/** Host-testable Objective-C object model; it does not expose a platform ABI. */
class Runtime {
    mutable std::mutex mutex_;
    Selector nextSelector_ = 1;
    std::map<std::string, Selector> selectors_;
    std::map<std::string, std::unique_ptr<Class>> classes_;
    std::set<const Class *> knownClasses_;
    std::map<std::pair<const Class *, Selector>, IMP> cache_;

    IMP lookupLocked(const Class *klass, Selector selector);
    void requireKnownClassLocked(const Class *klass) const;

  public:
    Selector selector(const std::string &name);
    Class *registerClass(const std::string &name, Class *superclass = nullptr,
                         std::vector<std::string> ivars = {});
    Class *findClass(const std::string &name) const;
    void addMethod(Class *klass, Selector selector, IMP implementation);
    Object *allocate(Class *klass) const;
    Value send(Object *object, Selector selector, const Arguments &arguments = {});
    Value send(Class *classObject, Selector selector, const Arguments &arguments = {});
};

Object *retain(Object *object);
void release(Object *object);
Object *autorelease(Object *object);

class AutoreleasePool {
    static thread_local AutoreleasePool *current_;
    AutoreleasePool *previous_ = nullptr;
    std::vector<Object *> objects_;

  public:
    AutoreleasePool();
    ~AutoreleasePool();
    AutoreleasePool(const AutoreleasePool &) = delete;
    AutoreleasePool &operator=(const AutoreleasePool &) = delete;

    static Object *add(Object *object);
};

} // namespace radek::compat_runtime::objc
