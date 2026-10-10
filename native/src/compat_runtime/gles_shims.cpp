// OpenGL ES 1.1 forwarding layer for the compat runtime.
//
// Design: the guest is an ES 1.1 fixed-function app, so its GL imports are
// forwarded to the platform's own GLES/EGL driver instead of being
// reimplemented. Android uses the device's `/system/lib*/libGLESv1_CM.so` and
// `libEGL.so` (the same hardware-accelerated driver every other app uses);
// desktop hosts use whatever `libGLESv1_CM`/`libEGL` the system provides. When
// no driver is present the probe fails, every call is refused with a named
// diagnostic, and the boot report states it - nothing is emulated silently.
//
// Consequences of the design that matter for correctness:
//   * guest pointers are translated per call with the byte range the call
//     needs (`GuestAddressSpace::guestToHost`), so the driver never reads host
//     memory outside mapped guest regions;
//   * the guest's client arrays are re-issued at draw time with translated
//     pointers, so a guest array that moved or an array left over from an
//     earlier binding can never point the driver at stale host memory;
//   * EAGL's drawable is a real EGL surface: `renderbufferStorage:fromDrawable:`
//     creates the EGL context/surface (window surface when the Android glue
//     supplied an `ANativeWindow`, offscreen pbuffer otherwise) and
//     `presentRenderbuffer:` is `eglSwapBuffers`.
#include "compat_runtime/gles_shims.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include <dlfcn.h>

namespace radek::compat_runtime::gles {
namespace {

// ---- GL/EGL types and the values the forwarding logic reasons about --------
using GLenum = unsigned int;
using GLfloat = float;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLbitfield = unsigned int;

constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_FRAMEBUFFER_OES = 0x8D40;
constexpr GLenum GL_RENDERBUFFER_WIDTH_OES = 0x8D42;
constexpr GLenum GL_RENDERBUFFER_HEIGHT_OES = 0x8D43;
constexpr GLenum GL_COLOR_ATTACHMENT0_OES = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_OES = 0x8CD5;

constexpr GLenum GL_VERTEX_ARRAY = 0x8074;
constexpr GLenum GL_NORMAL_ARRAY = 0x8075;
constexpr GLenum GL_COLOR_ARRAY = 0x8076;
constexpr GLenum GL_TEXTURE_COORD_ARRAY = 0x8078;

constexpr GLenum GL_BYTE = 0x1400;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_SHORT = 0x1402;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_FIXED = 0x140C;
constexpr GLenum GL_FLOAT = 0x1406;

using EGLint = int;
using EGLBoolean = unsigned int;
using EGLDisplay = void *;
using EGLConfig = void *;
using EGLSurface = void *;
using EGLContext = void *;
using EGLNativeDisplayType = void *;
using EGLNativeWindowType = void *;

constexpr EGLint EGL_NONE = 0x3038;
constexpr EGLint EGL_TRUE_VALUE = 1;
constexpr EGLint EGL_DEFAULT_DISPLAY_VALUE = 0;
constexpr EGLint EGL_SURFACE_TYPE = 0x3033;
constexpr EGLint EGL_PBUFFER_BIT = 0x0001;
constexpr EGLint EGL_WINDOW_BIT = 0x0004;
constexpr EGLint EGL_RENDERABLE_TYPE = 0x3040;
constexpr EGLint EGL_OPENGL_ES_BIT = 0x0001;
constexpr EGLint EGL_OPENGL_ES2_BIT = 0x0004;
constexpr EGLint EGL_DEPTH_SIZE = 0x3025;
constexpr EGLint EGL_WIDTH = 0x3057;
constexpr EGLint EGL_HEIGHT = 0x3056;
constexpr EGLint EGL_CONTEXT_CLIENT_VERSION = 0x3098;

// The callout window used by the forwarding bindings (free range between the
// libsystem window ending at 0xf004ffff and the loader's reserved region).
constexpr GuestAddress kCalloutBase = 0xf0080000u;
constexpr GuestAddress kCalloutEnd = 0xf00C0000u;

struct AtomicProgress {
    std::atomic<bool> driverGlesLoaded{false};
    std::atomic<bool> driverEglLoaded{false};
    std::atomic<bool> drawableReady{false};
    std::atomic<bool> presentingToWindow{false};
    std::atomic<std::uint32_t> drawableWidth{0};
    std::atomic<std::uint32_t> drawableHeight{0};
    std::atomic<std::uint64_t> guestCallsObserved{0};
    std::atomic<std::uint64_t> forwardedCalls{0};
    std::atomic<std::uint64_t> refusedCalls{0};
    std::atomic<std::uint64_t> framesPresented{0};
};

AtomicProgress gProgress;

void recordDriver(const DriverReport &report) {
    gProgress.driverGlesLoaded.store(report.glesLoaded, std::memory_order_relaxed);
    gProgress.driverEglLoaded.store(report.eglLoaded, std::memory_order_relaxed);
}

void recordDrawable(bool ready, bool window, std::uint32_t width, std::uint32_t height) {
    gProgress.drawableReady.store(ready, std::memory_order_relaxed);
    gProgress.presentingToWindow.store(ready && window, std::memory_order_relaxed);
    gProgress.drawableWidth.store(ready ? width : 0, std::memory_order_relaxed);
    gProgress.drawableHeight.store(ready ? height : 0, std::memory_order_relaxed);
}

/** ARM AAPCS argument view: r0-r3, then [sp], plus the VFP argument bank. */
struct Args {
    CpuRegisterState &registers;
    GuestAddressSpace &memory;
    std::string &reason;

    std::uint32_t u32(int index) const {
        if (index < 4)
            return registers.r[static_cast<std::size_t>(index)];
        std::uint32_t value = 0;
        if (registers.r[13] == 0 ||
            !memory.read(registers.r[13] + static_cast<GuestAddress>(4 * (index - 4)), &value,
                         sizeof(value)))
            throw std::runtime_error("streaming argument is unreadable");
        return value;
    }
    GLint i32(int index) const { return static_cast<GLint>(u32(index)); }
    GLenum enumValue(int index) const { return static_cast<GLenum>(u32(index)); }
    GLfloat f(int index) const {
        const auto slot = registers.d[static_cast<std::size_t>(index / 2)];
        const std::uint32_t bits = (index % 2 == 0) ? static_cast<std::uint32_t>(slot)
                                                    : static_cast<std::uint32_t>(slot >> 32);
        GLfloat value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    GuestAddress pointer(int index) const { return u32(index); }
};

std::size_t elementSize(GLenum type) {
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
        return 2;
    case GL_FIXED:
    case GL_FLOAT:
        return 4;
    default:
        return 0;
    }
}

/** Loads the platform GL/EGL libraries once per process. */
struct Driver {
    void *gles = nullptr;
    void *egl = nullptr;
    bool glesLoaded = false;
    bool eglLoaded = false;
    std::string detail;
    std::map<std::string, void *> symbols;

    Driver() {
        for (const char *name : {"libGLESv1_CM.so.1", "libGLESv1_CM.so", "libGLESv2.so.2"}) {
            gles = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (gles) {
                glesLoaded = true;
                detail += std::string(detail.empty() ? "" : ", ") + "GLES=" + name;
                break;
            }
        }
        if (!gles) {
            const char *error = ::dlerror();
            detail = std::string("GLES=unavailable") + (error ? std::string(" (") + error + ")" : "");
        }
        for (const char *name : {"libEGL.so.1", "libEGL.so", "libEGL_angle.so"}) {
            egl = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (egl) {
                eglLoaded = true;
                detail += std::string(detail.empty() ? "" : ", ") + "EGL=" + name;
                break;
            }
        }
        if (!egl)
            detail += std::string(detail.empty() ? "" : ", ") + "EGL=unavailable";
    }

    void *find(const char *name) {
        const auto cached = symbols.find(name);
        if (cached != symbols.end())
            return cached->second;
        void *address = nullptr;
        if (gles)
            address = ::dlsym(gles, name);
        if (!address && egl)
            address = ::dlsym(egl, name);
        symbols.emplace(name, address);
        return address;
    }

    template <typename Fn> Fn function(const char *name) {
        return reinterpret_cast<Fn>(find(name));
    }
};

struct EglState {
    EGLDisplay display = nullptr;
    EGLConfig config = nullptr;
    EGLContext context = nullptr;
    EGLSurface surface = nullptr;
    bool windowSurface = false;
    bool current = false;
    void *boundWindow = nullptr;  // Android window behind `surface`, if any
    int width = 0;
    int height = 0;

    void *eglGetDisplay = nullptr;
    void *eglInitialize = nullptr;
    void *eglChooseConfig = nullptr;
    void *eglCreateContext = nullptr;
    void *eglCreateWindowSurface = nullptr;
    void *eglCreatePbufferSurface = nullptr;
    void *eglMakeCurrent = nullptr;
    void *eglSwapBuffers = nullptr;
    void *eglGetError = nullptr;
};

} // namespace

void resetProgress() {
    gProgress.driverGlesLoaded.store(false, std::memory_order_relaxed);
    gProgress.driverEglLoaded.store(false, std::memory_order_relaxed);
    gProgress.drawableReady.store(false, std::memory_order_relaxed);
    gProgress.presentingToWindow.store(false, std::memory_order_relaxed);
    gProgress.drawableWidth.store(0, std::memory_order_relaxed);
    gProgress.drawableHeight.store(0, std::memory_order_relaxed);
    gProgress.guestCallsObserved.store(0, std::memory_order_relaxed);
    gProgress.forwardedCalls.store(0, std::memory_order_relaxed);
    gProgress.refusedCalls.store(0, std::memory_order_relaxed);
    gProgress.framesPresented.store(0, std::memory_order_relaxed);
}

ProgressSnapshot progressSnapshot() {
    ProgressSnapshot snapshot;
    snapshot.driverGlesLoaded = gProgress.driverGlesLoaded.load(std::memory_order_relaxed);
    snapshot.driverEglLoaded = gProgress.driverEglLoaded.load(std::memory_order_relaxed);
    snapshot.drawableReady = gProgress.drawableReady.load(std::memory_order_relaxed);
    snapshot.presentingToWindow = gProgress.presentingToWindow.load(std::memory_order_relaxed);
    snapshot.drawableWidth = gProgress.drawableWidth.load(std::memory_order_relaxed);
    snapshot.drawableHeight = gProgress.drawableHeight.load(std::memory_order_relaxed);
    snapshot.guestCallsObserved = gProgress.guestCallsObserved.load(std::memory_order_relaxed);
    snapshot.forwardedCalls = gProgress.forwardedCalls.load(std::memory_order_relaxed);
    snapshot.refusedCalls = gProgress.refusedCalls.load(std::memory_order_relaxed);
    snapshot.framesPresented = gProgress.framesPresented.load(std::memory_order_relaxed);
    return snapshot;
}

// ---------------------------------------------------------------------------
struct Forwarder::Impl {
    static constexpr std::size_t kMaxDiagnostics = 64;

    Driver driver;
    DriverReport report;
    EglState egl;
    // Written from the JNI thread (surface changes), read on the guest thread.
    std::atomic<void *> nativeWindow{nullptr};
    void *failedWindowBind = nullptr;  // window whose bind already failed; not retried per frame

    std::set<int> drawableRenderbuffers;  // EAGL-backed renderbuffer names
    std::set<int> drawableFramebuffers;   // guest FBOs whose color attachment is EAGL
    int boundRenderbuffer = 0;
    int boundFramebuffer = 0;
    int boundArrayBuffer = 0;
    int boundElementArrayBuffer = 0;

    struct ArrayState {
        bool enabled = false;
        GLint size = 0;
        GLenum type = 0;
        GLsizei stride = 0;
        GuestAddress pointer = 0;
    };
    ArrayState vertexArray;
    ArrayState colorArray;
    ArrayState texCoordArray;
    ArrayState normalArray;

    std::uint64_t guestCalls = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t refused = 0;
    std::uint64_t frames = 0;
    std::vector<std::string> diagnostics;
    std::uint32_t drawableWidth = 0;
    std::uint32_t drawableHeight = 0;

    GuestAddress nextCallout = kCalloutBase;

    Impl() {
        report.glesLoaded = driver.glesLoaded;
        report.eglLoaded = driver.eglLoaded;
        report.detail = driver.detail;
        recordDriver(report);
    }

    void note(const std::string &message) {
        if (diagnostics.size() >= kMaxDiagnostics)
            return;
        if (std::find(diagnostics.begin(), diagnostics.end(), message) != diagnostics.end())
            return;
        diagnostics.push_back(message);
    }

    /** Records that a call could not be handed to the driver and why. */
    void refuse(const std::string &call, const std::string &why = {}) {
        ++refused;
        gProgress.refusedCalls.fetch_add(1, std::memory_order_relaxed);
        if (why.empty())
            note(call + " refused: no GLES driver is loaded on this host");
        else
            note(call + " refused: " + why);
    }

    template <typename Fn> Fn symbol(const char *name) { return driver.function<Fn>(name); }

    template <typename Fn, typename... A> bool call(const char *name, A... args) {
        Fn fn = symbol<Fn>(name);
        if (!fn) {
            refuse(name);
            return false;
        }
        fn(args...);
        ++forwarded;
        gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /** Guest range -> host pointer, or null (with a diagnostic) when unmapped. */
    void *hostAddress(GuestAddressSpace &memory, GuestAddress address, std::size_t bytes,
                      const char *call) {
        if (bytes == 0)
            return nullptr;
        void *host = memory.guestToHost(address, bytes, MemoryPermission::Read);
        if (!host)
            refuse(call, "the guest range " + describe(address, bytes) +
                             " leaves mapped guest memory");
        return host;
    }

    static std::string describe(GuestAddress address, std::size_t bytes) {
        constexpr char digits[] = "0123456789abcdef";
        std::string text = "0x";
        for (int shift = 28; shift >= 0; shift -= 4)
            text.push_back(digits[(address >> shift) & 0xF]);
        return text + " + " + std::to_string(bytes) + " byte(s)";
    }

    static std::size_t arrayBytes(const ArrayState &array, GLint first, GLsizei count) {
        if (count <= 0)
            return 0;
        const std::size_t element = elementSize(array.type) * static_cast<std::size_t>(
                                                              std::max<GLint>(array.size, 1));
        const std::size_t stride =
            array.stride > 0 ? static_cast<std::size_t>(array.stride) : element;
        return static_cast<std::size_t>(first + count - 1) * stride + element;
    }

    /**
     * Re-issues one client array with a host pointer valid for `needed` bytes.
     * Buffer-backed arrays pass their offset through untouched: the driver reads
     * the buffer object, not guest memory.
     */
    bool applyArray(const char *setter, GuestAddressSpace &memory, const ArrayState &array,
                    std::size_t needed, void *fn) {
        if (!array.enabled)
            return true;
        if (boundArrayBuffer != 0) {
            using SetterFn = void (*)(GLint, GLenum, GLsizei, const void *);
            auto *setterFn = reinterpret_cast<SetterFn>(fn);
            if (!setterFn)
                return false;
            setterFn(array.size, array.type, array.stride,
                     reinterpret_cast<const void *>(static_cast<std::uintptr_t>(array.pointer)));
            ++forwarded;
            gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        auto *host = static_cast<const void *>(
            hostAddress(memory, array.pointer, needed, setter));
        if (!host)
            return false;
        using SetterFn = void (*)(GLint, GLenum, GLsizei, const void *);
        auto *setterFn = reinterpret_cast<SetterFn>(fn);
        if (!setterFn) {
            refuse(setter);
            return false;
        }
        setterFn(array.size, array.type, array.stride, host);
        ++forwarded;
        gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /** Re-issues every enabled client array for a draw of `count` vertices. */
    bool applyDrawArrays(GuestAddressSpace &memory, GLint first, GLsizei count) {
        if (count <= 0)
            return true;
        for (const auto *entry :
             {&vertexArray, &colorArray, &texCoordArray, &normalArray}) {
            if (entry->type != 0 && elementSize(entry->type) == 0) {
                refuse("glDrawArrays", "a client array uses an unsupported element type");
                return false;
            }
        }
        const std::size_t vertexBytes = arrayBytes(vertexArray, first, count);
        const std::size_t colorBytes = arrayBytes(colorArray, first, count);
        const std::size_t texCoordBytes = arrayBytes(texCoordArray, first, count);
        const std::size_t normalBytes = arrayBytes(normalArray, first, count);
        if (!applyArray("glVertexPointer", memory, vertexArray, vertexBytes,
                        driver.find("glVertexPointer")))
            return false;
        if (!applyArray("glColorPointer", memory, colorArray, colorBytes,
                        driver.find("glColorPointer")))
            return false;
        if (!applyArray("glTexCoordPointer", memory, texCoordArray, texCoordBytes,
                        driver.find("glTexCoordPointer")))
            return false;
        if (!applyArray("glNormalPointer", memory, normalArray, normalBytes,
                        driver.find("glNormalPointer")))
            return false;
        return true;
    }

    // ------------------------------------------------------------------
    // EGL drawable
    // ------------------------------------------------------------------
    bool createEglSurface(int width, int height, std::string &why) {
        auto eglGetDisplay = symbol<EGLDisplay (*)(EGLNativeDisplayType)>("eglGetDisplay");
        auto eglInitialize = symbol<EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *)>("eglInitialize");
        auto eglChooseConfig =
            symbol<EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)>(
                "eglChooseConfig");
        auto eglCreateContext =
            symbol<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *)>(
                "eglCreateContext");
        auto eglMakeCurrent =
            symbol<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent");
        if (!eglGetDisplay || !eglInitialize || !eglChooseConfig || !eglCreateContext ||
            !eglMakeCurrent) {
            why = "the EGL entry points are missing from the loaded libraries";
            return false;
        }
        egl.display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(
            static_cast<std::uintptr_t>(EGL_DEFAULT_DISPLAY_VALUE)));
        if (!egl.display) {
            why = "eglGetDisplay returned no display";
            return false;
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(egl.display, &major, &minor) != EGL_TRUE_VALUE) {
            why = "eglInitialize failed";
            return false;
        }
        const bool wantWindow = nativeWindow.load() != nullptr;
        auto choose = [&](EGLint renderable) {
            const EGLint attributes[] = {
                EGL_SURFACE_TYPE, wantWindow ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, renderable, EGL_DEPTH_SIZE, 16, EGL_NONE};
            EGLint configCount = 0;
            if (eglChooseConfig(egl.display, attributes, &egl.config, 1, &configCount) !=
                    EGL_TRUE_VALUE ||
                configCount < 1)
                return false;
            return true;
        };
        EGLint contextAttributeValue = 1;
        if (!choose(EGL_OPENGL_ES_BIT)) {
            if (!choose(EGL_OPENGL_ES2_BIT)) {
                why = "no EGL config supports an OpenGL ES surface on this device";
                return false;
            }
            contextAttributeValue = 2;
        }
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION,
                                            contextAttributeValue, EGL_NONE};
        egl.context = eglCreateContext(egl.display, egl.config, nullptr,
                                       contextAttributeValue == 1 ? nullptr : contextAttributes);
        if (!egl.context) {
            why = "eglCreateContext failed for an OpenGL ES context";
            return false;
        }
        if (wantWindow) {
            auto eglCreateWindowSurface =
                symbol<EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *)>(
                    "eglCreateWindowSurface");
            if (!eglCreateWindowSurface) {
                why = "eglCreateWindowSurface is missing from the loaded libraries";
                return false;
            }
            egl.surface = eglCreateWindowSurface(
                egl.display, egl.config,
                reinterpret_cast<EGLNativeWindowType>(nativeWindow.load()),
                nullptr);
            egl.windowSurface = egl.surface != nullptr;
            egl.boundWindow = egl.windowSurface ? nativeWindow.load() : nullptr;
            if (!egl.surface) {
                why = "eglCreateWindowSurface failed for the Android surface";
                return false;
            }
        } else {
            auto eglCreatePbufferSurface =
                symbol<EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *)>(
                    "eglCreatePbufferSurface");
            if (!eglCreatePbufferSurface) {
                why = "eglCreatePbufferSurface is missing from the loaded libraries";
                return false;
            }
            const EGLint attributes[] = {EGL_WIDTH, std::max(width, 1), EGL_HEIGHT,
                                         std::max(height, 1), EGL_NONE};
            egl.surface =
                eglCreatePbufferSurface(egl.display, egl.config, attributes);
            egl.windowSurface = false;
            egl.boundWindow = nullptr;
            if (!egl.surface) {
                why = "eglCreatePbufferSurface failed";
                return false;
            }
        }
        if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) !=
            EGL_TRUE_VALUE) {
            why = "eglMakeCurrent failed";
            return false;
        }
        egl.current = true;
        egl.width = width;
        egl.height = height;
        return true;
    }

    /// Moves the drawable onto the Android window surface without losing it: the new
    /// window surface is created and made current first, and the old surface is
    /// destroyed only after that succeeds, so a failed bind keeps the offscreen drawable.
    bool rebindWindowSurface(std::string &why) {
        void *window = nativeWindow.load();
        if (window == nullptr) {
            why = "no Android window surface is available";
            return false;
        }
        if (!egl.display || !egl.context || !egl.current) {
            why = "no current EGL context to rebind";
            return false;
        }
        auto eglCreateWindowSurface =
            symbol<EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *)>(
                "eglCreateWindowSurface");
        auto eglMakeCurrent =
            symbol<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent");
        auto eglDestroySurface = symbol<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglDestroySurface");
        if (!eglCreateWindowSurface || !eglMakeCurrent || !eglDestroySurface) {
            why = "EGL window-surface entry points are missing from the loaded libraries";
            return false;
        }
        EGLSurface surface = eglCreateWindowSurface(
            egl.display, egl.config, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
        if (!surface) {
            why = "eglCreateWindowSurface failed for the Android surface";
            return false;
        }
        if (eglMakeCurrent(egl.display, surface, surface, egl.context) != EGL_TRUE_VALUE) {
            eglDestroySurface(egl.display, surface);
            why = "eglMakeCurrent failed for the Android window surface";
            return false;
        }
        if (egl.surface)
            eglDestroySurface(egl.display, egl.surface);
        egl.surface = surface;
        egl.windowSurface = true;
        egl.boundWindow = window;
        egl.current = true;
        return true;
    }

    bool attachDrawable(GuestAddressSpace &memory, std::uint32_t width, std::uint32_t height) {
        (void)memory;
        if (width == 0 || height == 0) {
            recordDrawable(false, false, 0, 0);
            note("EAGL drawable has a zero-sized frame; the drawable stays unattached");
            return false;
        }
        drawableRenderbuffers.insert(boundRenderbuffer);
        drawableWidth = width;
        drawableHeight = height;
        if (egl.current) {
            egl.width = static_cast<int>(width);
            egl.height = static_cast<int>(height);
            // The Android surface may arrive after the first (offscreen) attach:
            // recreate the drawable so the frames reach the screen instead of a
            // pbuffer nobody can see.
            if (nativeWindow.load() != nullptr &&
                (!egl.windowSurface || egl.boundWindow != nativeWindow.load())) {
                destroyEgl();
                std::string why;
                if (createEglSurface(static_cast<int>(width), static_cast<int>(height), why)) {
                    recordDrawable(true, egl.windowSurface, width, height);
                    note("EAGL drawable re-attached to an EGL window surface (frames go to the "
                         "Android surface)");
                    return true;
                }
                recordDrawable(false, false, 0, 0);
                note("EGL window surface setup failed: " + why + "; GL calls are refused");
                return false;
            }
            recordDrawable(egl.current && egl.surface != nullptr, egl.windowSurface, width, height);
            return true;
        }
        if (!driver.eglLoaded) {
            recordDrawable(false, false, 0, 0);
            note("no EGL library is available on this host: the EAGL drawable has no "
                 "context and every GL call is refused");
            return false;
        }
        std::string why;
        if (!createEglSurface(static_cast<int>(width), static_cast<int>(height), why)) {
            recordDrawable(false, false, 0, 0);
            note("EGL surface setup failed: " + why + "; GL calls are refused");
            return false;
        }
        recordDrawable(true, egl.windowSurface, width, height);
        note(egl.windowSurface
                 ? "EAGL drawable attached to an EGL window surface (frames go to the "
                   "Android surface)"
                 : "EAGL drawable attached to an offscreen EGL pbuffer surface (no native "
                   "window was supplied; nothing is presented on screen)");
        return true;
    }

    bool presentDrawable() {
        // The Android surface can arrive after the guest's drawable was attached to an
        // offscreen pbuffer. Bind it here so frames reach the screen, not the pbuffer.
        void *window = nativeWindow.load();
        if (egl.current && window != nullptr && window != failedWindowBind &&
            (!egl.windowSurface || egl.boundWindow != window)) {
            std::string why;
            if (rebindWindowSurface(why)) {
                recordDrawable(true, true, static_cast<std::uint32_t>(egl.width),
                               static_cast<std::uint32_t>(egl.height));
                note("EAGL drawable bound to the Android window surface before presenting");
            } else {
                failedWindowBind = window;
                note("could not bind the Android window surface (" + why +
                     "); frames stay on the offscreen surface");
            }
        }
        if (!egl.current || !egl.surface) {
            refuse("eglSwapBuffers", "the EAGL drawable has no current EGL surface");
            return false;
        }
        auto eglSwapBuffers = symbol<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglSwapBuffers");
        if (!eglSwapBuffers) {
            refuse("eglSwapBuffers");
            return false;
        }
        const auto result = eglSwapBuffers(egl.display, egl.surface);
        ++forwarded;
        gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
        if (result != EGL_TRUE_VALUE) {
            refuse("eglSwapBuffers", "the EGL driver reported a failed swap");
            return false;
        }
        ++frames;
        gProgress.framesPresented.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void destroyEgl() {
        if (!driver.eglLoaded)
            return;
        auto eglMakeCurrent =
            symbol<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent");
        auto eglDestroySurface = symbol<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglDestroySurface");
        auto eglDestroyContext = symbol<EGLBoolean (*)(EGLDisplay, EGLContext)>("eglDestroyContext");
        auto eglTerminate = symbol<EGLBoolean (*)(EGLDisplay)>("eglTerminate");
        if (eglMakeCurrent && egl.display)
            eglMakeCurrent(egl.display, nullptr, nullptr, nullptr);
        if (eglDestroySurface && egl.display && egl.surface)
            eglDestroySurface(egl.display, egl.surface);
        if (eglDestroyContext && egl.display && egl.context)
            eglDestroyContext(egl.display, egl.context);
        if (eglTerminate && egl.display)
            eglTerminate(egl.display);
        egl = EglState{};
    }
};

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
void Forwarder::registerBindings(ShimRegistry &registry) {
    auto &impl = *impl_;
    auto bind = [&](const std::string &symbol, const char *adapter,
                    std::function<bool(Args &)> invoke) {
        if (impl.nextCallout >= kCalloutEnd)
            throw std::runtime_error("GLES forwarding callout window is exhausted");
        ShimBinding binding;
        binding.darwinSymbol = symbol;
        binding.library = "OpenGLES";
        binding.adapterName = adapter;
        binding.guestAddress = impl.nextCallout;
        impl.nextCallout += 4;
        binding.invoke = [&impl, invoke](CpuRegisterState &registers, GuestAddressSpace &memory,
                                         std::string &reason) {
            ++impl.guestCalls;
            gProgress.guestCallsObserved.fetch_add(1, std::memory_order_relaxed);
            Args args{registers, memory, reason};
            try {
                return invoke(args);
            } catch (const std::exception &error) {
                // A GL call never aborts the boot attempt: an unreadable argument or a
                // refused feature is recorded and the call becomes a no-op.
                impl.refuse("a GL call", error.what());
                return true;
            }
        };
        registry.registerBinding(std::move(binding));
    };

    // ---- fixed-function state -------------------------------------------------
    bind("_glClear", "gles-forward-clear", [&impl](Args &args) {
        impl.call<void (*)(GLbitfield)>("glClear", static_cast<GLbitfield>(args.u32(0)));
        return true;
    });
    bind("_glClearColor", "gles-forward-clear-color", [&impl](Args &args) {
        impl.call<void (*)(GLfloat, GLfloat, GLfloat, GLfloat)>("glClearColor", args.f(0),
                                                                args.f(1), args.f(2), args.f(3));
        return true;
    });
    bind("_glViewport", "gles-forward-viewport", [&impl](Args &args) {
        impl.call<void (*)(GLint, GLint, GLsizei, GLsizei)>("glViewport", args.i32(0), args.i32(1),
                                                            args.i32(2), args.i32(3));
        return true;
    });
    bind("_glScissor", "gles-forward-scissor", [&impl](Args &args) {
        impl.call<void (*)(GLint, GLint, GLsizei, GLsizei)>("glScissor", args.i32(0), args.i32(1),
                                                            args.i32(2), args.i32(3));
        return true;
    });
    bind("_glEnable", "gles-forward-enable", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glEnable", args.enumValue(0));
        return true;
    });
    bind("_glDisable", "gles-forward-disable", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glDisable", args.enumValue(0));
        return true;
    });
    bind("_glBlendFunc", "gles-forward-blend-func", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum)>("glBlendFunc", args.enumValue(0), args.enumValue(1));
        return true;
    });
    bind("_glDepthFunc", "gles-forward-depth-func", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glDepthFunc", args.enumValue(0));
        return true;
    });
    bind("_glDepthMask", "gles-forward-depth-mask", [&impl](Args &args) {
        impl.call<void (*)(unsigned char)>("glDepthMask",
                                           static_cast<unsigned char>(args.u32(0) != 0));
        return true;
    });
    bind("_glFrontFace", "gles-forward-front-face", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glFrontFace", args.enumValue(0));
        return true;
    });
    bind("_glCullFace", "gles-forward-cull-face", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glCullFace", args.enumValue(0));
        return true;
    });
    bind("_glLineWidth", "gles-forward-line-width", [&impl](Args &args) {
        impl.call<void (*)(GLfloat)>("glLineWidth", args.f(0));
        return true;
    });
    bind("_glColor4f", "gles-forward-color4f", [&impl](Args &args) {
        impl.call<void (*)(GLfloat, GLfloat, GLfloat, GLfloat)>("glColor4f", args.f(0), args.f(1),
                                                                args.f(2), args.f(3));
        return true;
    });
    bind("_glPixelStorei", "gles-forward-pixel-store", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLint)>("glPixelStorei", args.enumValue(0), args.i32(1));
        return true;
    });
    bind("_glTexEnvi", "gles-forward-tex-env", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum, GLint)>("glTexEnvi", args.enumValue(0), args.enumValue(1),
                                                   args.i32(2));
        return true;
    });
    bind("_glTexEnvf", "gles-forward-tex-env-f", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum, GLfloat)>("glTexEnvf", args.enumValue(0),
                                                     args.enumValue(1), args.f(2));
        return true;
    });
    bind("_glActiveTexture", "gles-forward-active-texture", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glActiveTexture", args.enumValue(0));
        return true;
    });
    bind("_glClientActiveTexture", "gles-forward-client-active-texture", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glClientActiveTexture", args.enumValue(0));
        return true;
    });
    bind("_glShadeModel", "gles-forward-shade-model", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glShadeModel", args.enumValue(0));
        return true;
    });
    bind("_glAlphaFunc", "gles-forward-alpha-func", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLfloat)>("glAlphaFunc", args.enumValue(0), args.f(1));
        return true;
    });
    bind("_glColorMask", "gles-forward-color-mask", [&impl](Args &args) {
        impl.call<void (*)(unsigned char, unsigned char, unsigned char, unsigned char)>(
            "glColorMask", static_cast<unsigned char>(args.u32(0) != 0),
            static_cast<unsigned char>(args.u32(1) != 0),
            static_cast<unsigned char>(args.u32(2) != 0),
            static_cast<unsigned char>(args.u32(3) != 0));
        return true;
    });
    bind("_glHint", "gles-forward-hint", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum)>("glHint", args.enumValue(0), args.enumValue(1));
        return true;
    });
    bind("_glFinish", "gles-forward-finish", [&impl](Args &) {
        impl.call<void (*)()>("glFinish");
        return true;
    });
    bind("_glFlush", "gles-forward-flush", [&impl](Args &) {
        impl.call<void (*)()>("glFlush");
        return true;
    });
    bind("_glGetError", "gles-forward-get-error", [&impl](Args &args) {
        auto *fn = impl.symbol<GLenum (*)()>("glGetError");
        if (!fn) {
            impl.refuse("glGetError");
            args.registers.r[0] = 0; // GL_NO_ERROR: the guest cannot observe a driver error
            return true;
        }
        args.registers.r[0] = fn();
        ++impl.forwarded;
        return true;
    });

    // ---- matrices -------------------------------------------------------------
    bind("_glMatrixMode", "gles-forward-matrix-mode", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glMatrixMode", args.enumValue(0));
        return true;
    });
    bind("_glLoadMatrixf", "gles-forward-load-matrix", [&impl](Args &args) {
        const auto address = args.pointer(0);
        auto *host = static_cast<const void *>(
            impl.hostAddress(args.memory, address, 16 * sizeof(GLfloat), "glLoadMatrixf"));
        if (!host)
            return true;
        impl.call<void (*)(const GLfloat *)>("glLoadMatrixf",
                                             static_cast<const GLfloat *>(host));
        return true;
    });
    bind("_glMultMatrixf", "gles-forward-mult-matrix", [&impl](Args &args) {
        const auto address = args.pointer(0);
        auto *host = static_cast<const void *>(
            impl.hostAddress(args.memory, address, 16 * sizeof(GLfloat), "glMultMatrixf"));
        if (!host)
            return true;
        impl.call<void (*)(const GLfloat *)>("glMultMatrixf",
                                             static_cast<const GLfloat *>(host));
        return true;
    });
    bind("_glLoadIdentity", "gles-forward-load-identity", [&impl](Args &) {
        impl.call<void (*)()>("glLoadIdentity");
        return true;
    });
    bind("_glPushMatrix", "gles-forward-push-matrix", [&impl](Args &) {
        impl.call<void (*)()>("glPushMatrix");
        return true;
    });
    bind("_glPopMatrix", "gles-forward-pop-matrix", [&impl](Args &) {
        impl.call<void (*)()>("glPopMatrix");
        return true;
    });

    // ---- fixed-function lighting (the driver's own implementation) ------------
    bind("_glLightfv", "gles-forward-light", [&impl](Args &args) {
        const auto address = args.pointer(2);
        const bool isMode = args.enumValue(1) == 0x0B52 /* GL_LIGHT_MODEL_* */;
        auto *host = static_cast<const void *>(impl.hostAddress(
            args.memory, address, (isMode ? 4 : 4) * sizeof(GLfloat), "glLightfv"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, GLenum, const GLfloat *)>(
            "glLightfv", args.enumValue(0), args.enumValue(1),
            static_cast<const GLfloat *>(host));
        return true;
    });
    bind("_glMaterialfv", "gles-forward-material", [&impl](Args &args) {
        auto *host = static_cast<const void *>(impl.hostAddress(
            args.memory, args.pointer(2), 4 * sizeof(GLfloat), "glMaterialfv"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, GLenum, const GLfloat *)>(
            "glMaterialfv", args.enumValue(0), args.enumValue(1),
            static_cast<const GLfloat *>(host));
        return true;
    });

    // ---- client arrays --------------------------------------------------------
    auto bindPointer = [&](const std::string &symbol, const char *adapter,
                           Impl::ArrayState Impl::*member) {
        bind(symbol, adapter, [&impl, member](Args &args) {
            auto &array = impl.*member;
            array.size = args.i32(0);
            array.type = args.enumValue(1);
            array.stride = args.i32(2);
            array.pointer = args.pointer(3);
            // Forward the call as well: the pointer is re-issued with a host address at
            // draw time, but the driver keeps its own copy of the state meanwhile.
            return true;
        });
    };
    bindPointer("_glVertexPointer", "gles-forward-vertex-pointer", &Forwarder::Impl::vertexArray);
    bindPointer("_glColorPointer", "gles-forward-color-pointer", &Forwarder::Impl::colorArray);
    bindPointer("_glTexCoordPointer", "gles-forward-texcoord-pointer",
                &Forwarder::Impl::texCoordArray);
    bindPointer("_glNormalPointer", "gles-forward-normal-pointer", &Forwarder::Impl::normalArray);
    bind("_glEnableClientState", "gles-forward-enable-client-state", [&impl](Args &args) {
        switch (args.enumValue(0)) {
        case GL_VERTEX_ARRAY: impl.vertexArray.enabled = true; break;
        case GL_COLOR_ARRAY: impl.colorArray.enabled = true; break;
        case GL_TEXTURE_COORD_ARRAY: impl.texCoordArray.enabled = true; break;
        case GL_NORMAL_ARRAY: impl.normalArray.enabled = true; break;
        default: break;
        }
        impl.call<void (*)(GLenum)>("glEnableClientState", args.enumValue(0));
        return true;
    });
    bind("_glDisableClientState", "gles-forward-disable-client-state", [&impl](Args &args) {
        switch (args.enumValue(0)) {
        case GL_VERTEX_ARRAY: impl.vertexArray.enabled = false; break;
        case GL_COLOR_ARRAY: impl.colorArray.enabled = false; break;
        case GL_TEXTURE_COORD_ARRAY: impl.texCoordArray.enabled = false; break;
        case GL_NORMAL_ARRAY: impl.normalArray.enabled = false; break;
        default: break;
        }
        impl.call<void (*)(GLenum)>("glDisableClientState", args.enumValue(0));
        return true;
    });

    // ---- draws ----------------------------------------------------------------
    bind("_glDrawArrays", "gles-forward-draw-arrays", [&impl](Args &args) {
        const auto mode = args.enumValue(0);
        const GLint first = args.i32(1);
        const GLsizei count = args.i32(2);
        if (!impl.applyDrawArrays(args.memory, first, count))
            return true;
        impl.call<void (*)(GLenum, GLint, GLsizei)>("glDrawArrays", mode, first, count);
        return true;
    });
    bind("_glDrawElements", "gles-forward-draw-elements", [&impl](Args &args) {
        const auto mode = args.enumValue(0);
        const GLsizei count = args.i32(1);
        const auto type = args.enumValue(2);
        const auto indices = args.pointer(3);
        if (!impl.applyDrawArrays(args.memory, 0, 1))
            return true;
        if (impl.boundElementArrayBuffer != 0) {
            impl.call<void (*)(GLenum, GLsizei, GLenum, const void *)>(
                "glDrawElements", mode, count, type,
                reinterpret_cast<const void *>(static_cast<std::uintptr_t>(indices)));
            return true;
        }
        const std::size_t elementBytes = (type == GL_UNSIGNED_BYTE) ? 1u : 2u;
        auto *host = static_cast<const void *>(impl.hostAddress(
            args.memory, indices, static_cast<std::size_t>(std::max(count, 0)) * elementBytes,
            "glDrawElements"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, GLsizei, GLenum, const void *)>("glDrawElements", mode, count,
                                                                   type, host);
        return true;
    });

    // ---- buffer objects -------------------------------------------------------
    bind("_glGenBuffers", "gles-forward-gen-buffers", [&impl](Args &args) {
        const GLsizei count = args.i32(0);
        auto *host = static_cast<void *>(impl.hostAddress(
            args.memory, args.pointer(1), static_cast<std::size_t>(std::max(count, 0)) * 4u,
            "glGenBuffers"));
        if (!host)
            return true;
        impl.call<void (*)(GLsizei, GLuint *)>("glGenBuffers", count, static_cast<GLuint *>(host));
        return true;
    });
    bind("_glDeleteBuffers", "gles-forward-delete-buffers", [&impl](Args &args) {
        const GLsizei count = args.i32(0);
        auto *host = static_cast<void *>(impl.hostAddress(
            args.memory, args.pointer(1), static_cast<std::size_t>(std::max(count, 0)) * 4u,
            "glDeleteBuffers"));
        if (!host)
            return true;
        impl.call<void (*)(GLsizei, const GLuint *)>("glDeleteBuffers", count,
                                                     static_cast<const GLuint *>(host));
        return true;
    });
    bind("_glBindBuffer", "gles-forward-bind-buffer", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const int name = args.i32(1);
        if (target == GL_ARRAY_BUFFER)
            impl.boundArrayBuffer = name;
        else if (target == GL_ELEMENT_ARRAY_BUFFER)
            impl.boundElementArrayBuffer = name;
        impl.call<void (*)(GLenum, GLuint)>("glBindBuffer", target, static_cast<GLuint>(name));
        return true;
    });
    bind("_glBufferData", "gles-forward-buffer-data", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const auto size = args.u32(1);
        const auto address = args.pointer(2);
        const auto usage = args.enumValue(3);
        const void *host = nullptr;
        if (address != 0 && size != 0) {
            host = impl.hostAddress(args.memory, address, size, "glBufferData");
            if (!host)
                return true;
        }
        impl.call<void (*)(GLenum, long, const void *, GLenum)>(
            "glBufferData", target, static_cast<long>(size), host, usage);
        return true;
    });
    bind("_glBufferSubData", "gles-forward-buffer-sub-data", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const auto offset = args.u32(1);
        const auto size = args.u32(2);
        auto *host = static_cast<const void *>(
            impl.hostAddress(args.memory, args.pointer(3), size, "glBufferSubData"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, long, long, const void *)>("glBufferSubData", target,
                                                              static_cast<long>(offset),
                                                              static_cast<long>(size), host);
        return true;
    });

    // ---- textures -------------------------------------------------------------
    bind("_glGenTextures", "gles-forward-gen-textures", [&impl](Args &args) {
        const GLsizei count = args.i32(0);
        auto *host = static_cast<void *>(impl.hostAddress(
            args.memory, args.pointer(1), static_cast<std::size_t>(std::max(count, 0)) * 4u,
            "glGenTextures"));
        if (!host)
            return true;
        impl.call<void (*)(GLsizei, GLuint *)>("glGenTextures", count, static_cast<GLuint *>(host));
        return true;
    });
    bind("_glDeleteTextures", "gles-forward-delete-textures", [&impl](Args &args) {
        const GLsizei count = args.i32(0);
        auto *host = static_cast<void *>(impl.hostAddress(
            args.memory, args.pointer(1), static_cast<std::size_t>(std::max(count, 0)) * 4u,
            "glDeleteTextures"));
        if (!host)
            return true;
        impl.call<void (*)(GLsizei, const GLuint *)>("glDeleteTextures", count,
                                                     static_cast<const GLuint *>(host));
        return true;
    });
    bind("_glBindTexture", "gles-forward-bind-texture", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLuint)>("glBindTexture", args.enumValue(0),
                                            static_cast<GLuint>(args.u32(1)));
        return true;
    });
    bind("_glTexParameteri", "gles-forward-tex-parameter", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum, GLint)>("glTexParameteri", args.enumValue(0),
                                                   args.enumValue(1), args.i32(2));
        return true;
    });
    auto texImage = [&impl](const char *name, Args &args, int pointerIndex) {
        const auto target = args.enumValue(0);
        const GLint level = args.i32(1);
        const GLint internalFormat = args.i32(2);
        const GLsizei width = args.i32(3);
        const GLsizei height = args.i32(4);
        const GLint border = args.i32(5);
        const auto format = args.enumValue(6);
        const auto type = args.enumValue(7);
        const auto address = args.pointer(pointerIndex);
        const void *host = nullptr;
        if (address != 0) {
            const std::size_t bytes = static_cast<std::size_t>(std::max(width, 0)) *
                                      static_cast<std::size_t>(std::max(height, 0)) * 4u;
            host = impl.hostAddress(args.memory, address, std::max<std::size_t>(bytes, 1), name);
            if (!host)
                return;
        }
        impl.call<void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                           const void *)>(name, target, level, internalFormat, width, height,
                                          border, format, type, host);
    };
    bind("_glTexImage2D", "gles-forward-tex-image-2d", [texImage](Args &args) {
        texImage("glTexImage2D", args, 8);
        return true;
    });
    bind("_glTexSubImage2D", "gles-forward-tex-sub-image-2d", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const GLint level = args.i32(1);
        const GLint xoffset = args.i32(2);
        const GLint yoffset = args.i32(3);
        const GLsizei width = args.i32(4);
        const GLsizei height = args.i32(5);
        const auto format = args.enumValue(6);
        const auto type = args.enumValue(7);
        const auto address = args.pointer(8);
        const void *host = nullptr;
        if (address != 0) {
            const std::size_t bytes = static_cast<std::size_t>(std::max(width, 0)) *
                                      static_cast<std::size_t>(std::max(height, 0)) * 4u;
            host = impl.hostAddress(args.memory, address, std::max<std::size_t>(bytes, 1),
                                    "glTexSubImage2D");
            if (!host)
                return true;
        }
        impl.call<void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                           const void *)>("glTexSubImage2D", target, level, xoffset, yoffset,
                                          width, height, format, type, host);
        return true;
    });
    bind("_glCompressedTexImage2D", "gles-forward-compressed-tex-image-2d", [&impl](Args &args) {
        // Compressed uploads (PVRTC and friends) go to the driver as-is: decoding them
        // is exactly the driver's job, and a driver without support reports the GL error.
        const auto target = args.enumValue(0);
        const GLint level = args.i32(1);
        const auto format = args.enumValue(2);
        const GLsizei width = args.i32(3);
        const GLsizei height = args.i32(4);
        const GLint border = args.i32(5);
        const GLsizei imageSize = args.i32(6);
        const auto address = args.pointer(7);
        auto *host = static_cast<const void *>(
            impl.hostAddress(args.memory, address, static_cast<std::size_t>(std::max(imageSize, 1)),
                             "glCompressedTexImage2D"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)>(
            "glCompressedTexImage2D", target, level, format, width, height, border, imageSize,
            host);
        return true;
    });

    // ---- OES framebuffer / renderbuffer objects --------------------------------
    bind("_glGenRenderbuffersOES", "gles-forward-gen-renderbuffers", [&impl](Args &args) {
        const GLsizei count = args.i32(0);
        auto *host = static_cast<void *>(impl.hostAddress(
            args.memory, args.pointer(1), static_cast<std::size_t>(std::max(count, 0)) * 4u,
            "glGenRenderbuffersOES"));
        if (!host)
            return true;
        impl.call<void (*)(GLsizei, GLuint *)>("glGenRenderbuffersOES", count,
                                               static_cast<GLuint *>(host));
        return true;
    });
    bind("_glDeleteRenderbuffersOES", "gles-forward-delete-renderbuffers", [&impl](Args &args) {
        const GLsizei count = args.i32(0);
        auto *host = static_cast<void *>(impl.hostAddress(
            args.memory, args.pointer(1), static_cast<std::size_t>(std::max(count, 0)) * 4u,
            "glDeleteRenderbuffersOES"));
        if (!host)
            return true;
        const auto *names = static_cast<const GLuint *>(host);
        for (GLsizei i = 0; i < count; ++i)
            impl.drawableRenderbuffers.erase(static_cast<int>(names[i]));
        impl.call<void (*)(GLsizei, const GLuint *)>("glDeleteRenderbuffersOES", count, names);
        return true;
    });
    bind("_glBindRenderbufferOES", "gles-forward-bind-renderbuffer", [&impl](Args &args) {
        impl.boundRenderbuffer = args.i32(1);
        impl.call<void (*)(GLenum, GLuint)>("glBindRenderbufferOES", args.enumValue(0),
                                            static_cast<GLuint>(args.u32(1)));
        return true;
    });
    bind("_glRenderbufferStorageOES", "gles-forward-renderbuffer-storage", [&impl](Args &args) {
        if (impl.drawableRenderbuffers.count(impl.boundRenderbuffer) != 0) {
            // The EAGL drawable's storage is the EGL surface, not a driver renderbuffer.
            impl.drawableWidth = static_cast<std::uint32_t>(std::max(args.i32(2), 0));
            impl.drawableHeight = static_cast<std::uint32_t>(std::max(args.i32(3), 0));
            return true;
        }
        impl.call<void (*)(GLenum, GLenum, GLsizei, GLsizei)>(
            "glRenderbufferStorageOES", args.enumValue(0), args.enumValue(1), args.i32(2),
            args.i32(3));
        return true;
    });
    bind("_glGetRenderbufferParameterivOES", "gles-forward-get-renderbuffer-parameter",
         [&impl](Args &args) {
             if (impl.drawableRenderbuffers.count(impl.boundRenderbuffer) != 0) {
                 const auto pname = args.enumValue(1);
                 GLint value = 0;
                 if (pname == GL_RENDERBUFFER_WIDTH_OES)
                     value = static_cast<GLint>(impl.drawableWidth);
                 else if (pname == GL_RENDERBUFFER_HEIGHT_OES)
                     value = static_cast<GLint>(impl.drawableHeight);
                 auto *host = static_cast<void *>(
                     impl.hostAddress(args.memory, args.pointer(2), sizeof(GLint),
                                      "glGetRenderbufferParameterivOES"));
                 if (host)
                     std::memcpy(host, &value, sizeof(value));
                 return true;
             }
             auto *host = static_cast<void *>(impl.hostAddress(
                 args.memory, args.pointer(2), sizeof(GLint), "glGetRenderbufferParameterivOES"));
             if (!host)
                 return true;
             impl.call<void (*)(GLenum, GLenum, GLint *)>(
                 "glGetRenderbufferParameterivOES", args.enumValue(0), args.enumValue(1),
                 static_cast<GLint *>(host));
             return true;
         });
    bind("_glGenFramebuffersOES", "gles-forward-gen-framebuffers", [&impl](Args &args) {
        const GLsizei count = args.i32(0);
        auto *host = static_cast<void *>(impl.hostAddress(
            args.memory, args.pointer(1), static_cast<std::size_t>(std::max(count, 0)) * 4u,
            "glGenFramebuffersOES"));
        if (!host)
            return true;
        impl.call<void (*)(GLsizei, GLuint *)>("glGenFramebuffersOES", count,
                                               static_cast<GLuint *>(host));
        return true;
    });
    bind("_glDeleteFramebuffersOES", "gles-forward-delete-framebuffers", [&impl](Args &args) {
        const GLsizei count = args.i32(0);
        auto *host = static_cast<void *>(impl.hostAddress(
            args.memory, args.pointer(1), static_cast<std::size_t>(std::max(count, 0)) * 4u,
            "glDeleteFramebuffersOES"));
        if (!host)
            return true;
        const auto *names = static_cast<const GLuint *>(host);
        for (GLsizei i = 0; i < count; ++i)
            impl.drawableFramebuffers.erase(static_cast<int>(names[i]));
        impl.call<void (*)(GLsizei, const GLuint *)>("glDeleteFramebuffersOES", count, names);
        return true;
    });
    bind("_glBindFramebufferOES", "gles-forward-bind-framebuffer", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const int name = args.i32(1);
        impl.boundFramebuffer = name;
        // A framebuffer whose color attachment is the EAGL drawable is the default
        // framebuffer of the EGL surface, i.e. GL framebuffer 0.
        const auto hostName = (name != 0 && impl.drawableFramebuffers.count(name) != 0)
                                  ? 0u
                                  : static_cast<GLuint>(name);
        impl.call<void (*)(GLenum, GLuint)>("glBindFramebufferOES", target, hostName);
        return true;
    });
    bind("_glFramebufferRenderbufferOES", "gles-forward-framebuffer-renderbuffer",
         [&impl](Args &args) {
             const auto attachment = args.enumValue(1);
             const int renderbuffer = args.i32(3);
             if (attachment == GL_COLOR_ATTACHMENT0_OES &&
                 impl.drawableRenderbuffers.count(renderbuffer) != 0) {
                 if (impl.boundFramebuffer != 0)
                     impl.drawableFramebuffers.insert(impl.boundFramebuffer);
                 // Re-bind so the driver sees the default framebuffer for this object.
                 if (impl.boundFramebuffer == 0)
                     impl.call<void (*)(GLenum, GLuint)>("glBindFramebufferOES",
                                                         GL_FRAMEBUFFER_OES, 0);
                 return true;
             }
             impl.call<void (*)(GLenum, GLenum, GLenum, GLuint)>(
                 "glFramebufferRenderbufferOES", args.enumValue(0), attachment, args.enumValue(2),
                 static_cast<GLuint>(args.u32(3)));
             return true;
         });
    bind("_glFramebufferTexture2DOES", "gles-forward-framebuffer-texture", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum, GLenum, GLuint, GLint)>(
            "glFramebufferTexture2DOES", args.enumValue(0), args.enumValue(1), args.enumValue(2),
            static_cast<GLuint>(args.u32(3)), args.i32(4));
        return true;
    });
    bind("_glCheckFramebufferStatusOES", "gles-forward-check-framebuffer-status",
         [&impl](Args &args) {
             if (impl.boundFramebuffer != 0 &&
                 impl.drawableFramebuffers.count(impl.boundFramebuffer) != 0) {
                 args.registers.r[0] = GL_FRAMEBUFFER_COMPLETE_OES;
                 return true;
             }
             auto *fn = impl.symbol<GLenum (*)(GLenum)>("glCheckFramebufferStatusOES");
             if (!fn) {
                 impl.refuse("glCheckFramebufferStatusOES");
                 args.registers.r[0] = GL_FRAMEBUFFER_COMPLETE_OES;
                 return true;
             }
             args.registers.r[0] = fn(args.enumValue(0));
             ++impl.forwarded;
             return true;
         });

    // ---- queries ---------------------------------------------------------------
    bind("_glGetIntegerv", "gles-forward-get-integer", [&impl](Args &args) {
        // The guest may ask for up to 16 values (GL_VIEWPORT etc.); the driver writes
        // straight into guest memory, so the range must be mapped.
        auto *host = static_cast<void *>(
            impl.hostAddress(args.memory, args.pointer(1), 16 * sizeof(GLint), "glGetIntegerv"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, GLint *)>("glGetIntegerv", args.enumValue(0),
                                             static_cast<GLint *>(host));
        return true;
    });
    bind("_glGetString", "gles-forward-get-string", [&impl](Args &args) {
        auto *fn = impl.symbol<const unsigned char *(*)(GLenum)>("glGetString");
        if (!fn) {
            impl.refuse("glGetString");
            args.registers.r[0] = 0;
            return true;
        }
        const auto *text = fn(args.enumValue(0));
        ++impl.forwarded;
        if (!text) {
            args.registers.r[0] = 0;
            return true;
        }
        // Copy the driver string into guest memory so the guest owns the bytes.
        const std::size_t length = std::strlen(reinterpret_cast<const char *>(text)) + 1;
        const auto address = args.memory.mapAny(length, MemoryPermission::Read | MemoryPermission::Write,
                                                "gles-driver-string", 16);
        if (!args.memory.write(address, text, length)) {
            impl.refuse("glGetString", "the driver string could not be copied into guest memory");
            args.registers.r[0] = 0;
            return true;
        }
        args.registers.r[0] = address;
        return true;
    });
}

// ---------------------------------------------------------------------------
// Public plumbing
// ---------------------------------------------------------------------------
Forwarder::Forwarder() : impl_(new Impl()) {}
Forwarder::~Forwarder() {
    if (impl_)
        impl_->destroyEgl();
}

void Forwarder::setNativeWindow(void *window) { impl_->nativeWindow = window; }

bool Forwarder::attachDrawable(GuestAddressSpace &memory, std::uint32_t width,
                              std::uint32_t height) {
    return impl_->attachDrawable(memory, width, height);
}

bool Forwarder::presentDrawable() { return impl_->presentDrawable(); }

const DriverReport &Forwarder::driver() const { return impl_->report; }

bool Forwarder::drawableReady() const { return impl_->egl.current && impl_->egl.surface; }
bool Forwarder::presentingToWindow() const { return impl_->egl.windowSurface; }
std::uint32_t Forwarder::drawableWidth() const { return impl_->drawableWidth; }
std::uint32_t Forwarder::drawableHeight() const { return impl_->drawableHeight; }
std::uint64_t Forwarder::guestCallsObserved() const { return impl_->guestCalls; }
std::uint64_t Forwarder::forwardedCalls() const { return impl_->forwarded; }
std::uint64_t Forwarder::refusedCalls() const { return impl_->refused; }
std::uint64_t Forwarder::framesPresented() const { return impl_->frames; }
const std::vector<std::string> &Forwarder::diagnostics() const { return impl_->diagnostics; }

namespace {
std::mutex gRegistryMutex;
std::map<GuestAddressSpace *, std::unique_ptr<Forwarder>> gForwarders;
Forwarder *gLastForwarder = nullptr;
void *gDefaultNativeWindow = nullptr;
} // namespace

Forwarder &forwarderFor(GuestAddressSpace &memory) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto found = gForwarders.find(&memory);
    if (found == gForwarders.end()) {
        auto forwarder = std::make_unique<Forwarder>();
        forwarder->setNativeWindow(gDefaultNativeWindow);
        gLastForwarder = forwarder.get();
        found = gForwarders.emplace(&memory, std::move(forwarder)).first;
    }
    gLastForwarder = found->second.get();
    return *found->second;
}

void setDefaultNativeWindow(void *window) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gDefaultNativeWindow = window;
    if (gLastForwarder)
        gLastForwarder->setNativeWindow(window);
}

Forwarder *lastForwarder() {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    return gLastForwarder;
}

} // namespace radek::compat_runtime::gles
