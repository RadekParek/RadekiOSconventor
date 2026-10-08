#pragma once

#include "compat_runtime/shim_registry.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace radek::compat_runtime::gles {

/** What the runtime found when it probed the host for a GL/EGL driver. */
struct DriverReport {
    bool glesLoaded = false;   // libGLESv1_CM (the ES 1.1 import surface)
    bool eglLoaded = false;    // libEGL (drawable surfaces and contexts)
    std::string detail;        // library names, or why the probe failed
};

/**
 * OpenGL ES 1.1 forwarding layer for the compat runtime.
 *
 * The guest is an OpenGL ES 1.1 app: its GL imports are fixed-function
 * (`glMatrixMode`, `glLoadMatrixf`, the client arrays, `glDrawArrays`,
 * `glTexImage2D`, the `GL_OES_framebuffer_object` calls). Instead of
 * reimplementing GL, every imported call is forwarded to the real driver:
 *
 *   * on Android that is the device's own `libGLESv1_CM.so` + `libEGL.so`
 *     (hardware accelerated, and the same driver every other app uses);
 *   * on a host without GL libraries the driver probe fails, every call is
 *     refused with a named diagnostic, and the boot report says exactly that.
 *
 * Guest pointers (client arrays, texture uploads, query outputs) are translated
 * through `GuestAddressSpace::guestToHost` for the duration of one call, with
 * the byte range the call actually needs; a range that leaves mapped guest
 * memory refuses the call instead of reading host memory out of bounds.
 *
 * The EAGL drawable is a real EGL surface: `renderbufferStorage:fromDrawable:`
 * creates/binds it (window surface when the Android glue supplied a native
 * window, otherwise an offscreen pbuffer) and `presentRenderbuffer:` maps to
 * `eglSwapBuffers`. Frames are therefore the guest's own command stream on the
 * host GPU; the boot report still refuses to call that gameplay.
 */
class Forwarder {
  public:
    Forwarder();
    ~Forwarder();
    Forwarder(const Forwarder &) = delete;
    Forwarder &operator=(const Forwarder &) = delete;

    /** Registers the GLES symbols the runtime implements by forwarding. */
    void registerBindings(ShimRegistry &registry);

    /** `-[EAGLContext renderbufferStorage:fromDrawable:]`: EGL surface + context. */
    bool attachDrawable(GuestAddressSpace &memory, std::uint32_t width, std::uint32_t height);
    /** `-[EAGLContext presentRenderbuffer:]`: `eglSwapBuffers`. */
    bool presentDrawable();

    /** Android glue: `ANativeWindow_fromSurface` result for the window surface. */
    void setNativeWindow(void *window);

    const DriverReport &driver() const;
    bool drawableReady() const;
    bool presentingToWindow() const;
    std::uint32_t drawableWidth() const;
    std::uint32_t drawableHeight() const;
    /** Number of guest GLES imports entered, including calls refused by the host driver. */
    std::uint64_t guestCallsObserved() const;
    std::uint64_t forwardedCalls() const;
    std::uint64_t refusedCalls() const;
    std::uint64_t framesPresented() const;
    const std::vector<std::string> &diagnostics() const;

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

/** The forwarding GL context that belongs to one guest address space. */
Forwarder &forwarderFor(GuestAddressSpace &memory);

/** Native window used by contexts created afterwards (Android JNI glue). */
void setDefaultNativeWindow(void *window);

/** Context the Android glue should use for the window it just handed over. */
Forwarder *lastForwarder();

} // namespace radek::compat_runtime::gles
