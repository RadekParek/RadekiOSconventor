package dev.radek.generated;

import android.view.Choreographer;

/**
 * Lifecycle-scoped bridge from Android's UI frame clock to libioscompat.so.
 * The native side dispatches only registered C callback links. This does not
 * implement Objective-C CADisplayLink or Objective-C message dispatch.
 */
final class FrameClockBridge {
    private static boolean libraryLoaded;
    private static boolean running;
    private static long previousFrameTimeNanos;

    static {
        try {
            System.loadLibrary("ioscompat");
            libraryLoaded = true;
        } catch (LinkageError unavailable) {
            libraryLoaded = false;
        }
    }

    private static native void nativeDispatchFrame(long frameTimeNanos, long frameIntervalNanos);

    private static final Choreographer.FrameCallback CALLBACK = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            if (!running) return;
            long intervalNanos = previousFrameTimeNanos == 0L
                    ? 0L
                    : Math.max(0L, frameTimeNanos - previousFrameTimeNanos);
            previousFrameTimeNanos = frameTimeNanos;
            if (libraryLoaded) {
                try {
                    nativeDispatchFrame(frameTimeNanos, intervalNanos);
                } catch (LinkageError unavailable) {
                    libraryLoaded = false;
                    running = false;
                    return;
                }
            }
            if (running) Choreographer.getInstance().postFrameCallback(this);
        }
    };

    private FrameClockBridge() { }

    static void resume() {
        if (!libraryLoaded || running) return;
        running = true;
        previousFrameTimeNanos = 0L;
        Choreographer.getInstance().postFrameCallback(CALLBACK);
    }

    static void pause() {
        if (running) Choreographer.getInstance().removeFrameCallback(CALLBACK);
        running = false;
        previousFrameTimeNanos = 0L;
    }

    static boolean isAvailable() {
        return libraryLoaded;
    }
}
