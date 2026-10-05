#pragma once

/*
 * C callback service for frame-synchronised work in libioscompat.so.
 *
 * Android launchers drive radek_compat_CADisplayLinkDispatchFrame from
 * android.view.Choreographer. This is a usable native frame scheduler for a
 * future static recompilation backend/runtime; it is not an Objective-C CADisplayLink class or
 * objc_msgSend bridge. The callback/context pair must outlive the active link.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct radek_CADisplayLink radek_CADisplayLink;
typedef radek_CADisplayLink *radek_CADisplayLinkRef;

typedef void (*radek_CADisplayLinkCallback)(void *context,
                                             radek_CADisplayLinkRef displayLink,
                                             double timestamp,
                                             double duration);

radek_CADisplayLinkRef radek_compat_CADisplayLinkCreate(radek_CADisplayLinkCallback callback,
                                                         void *context);
void radek_compat_CADisplayLinkInvalidate(radek_CADisplayLinkRef displayLink);
void radek_compat_CADisplayLinkRelease(radek_CADisplayLinkRef displayLink);
void radek_compat_CADisplayLinkSetPaused(radek_CADisplayLinkRef displayLink, uint8_t paused);
uint8_t radek_compat_CADisplayLinkIsPaused(radek_CADisplayLinkRef displayLink);

/* 0 follows the display refresh; 1..240 requests an interval no faster than that rate. */
int radek_compat_CADisplayLinkSetPreferredFramesPerSecond(radek_CADisplayLinkRef displayLink,
                                                            int framesPerSecond);
int radek_compat_CADisplayLinkGetPreferredFramesPerSecond(radek_CADisplayLinkRef displayLink);
double radek_compat_CADisplayLinkGetTimestamp(radek_CADisplayLinkRef displayLink);
double radek_compat_CADisplayLinkGetDuration(radek_CADisplayLinkRef displayLink);
size_t radek_compat_CADisplayLinkActiveCount(void);

/* Called by the launcher once per Choreographer frame; timestamp/duration are ns. */
void radek_compat_CADisplayLinkDispatchFrame(int64_t frameTimeNanos,
                                              int64_t frameIntervalNanos);

#ifdef __cplusplus
} /* extern "C" */
#endif
