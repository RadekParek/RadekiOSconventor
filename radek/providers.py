"""iOS install names and imported symbols mapped to real Android providers.

This module is the host-side twin of ``app/src/main/java/dev/radek/conventor/
Providers.kt``; ``tests/test_providers.py`` asserts the two tables agree, so the
CLI report and the on-device report never diverge.

This inventory separates known Android libraries, platform targets, compatibility plans, and
four exact compiled time shims. A provider/candidate entry is not proof that a converted game has
been rewritten or linked to it. ``STATUS_PROVIDED`` denotes an Android system library; broad
framework targets remain compatibility candidates until their ABI and behavior are implemented.

* ``native-library`` - Android system libraries with reviewed C APIs.
* ``platform-api`` - Android platform targets; semantic adaptation may still be required.
* ``runtime`` - compatibility-runtime targets, not an Apple ABI claim.
"""

from __future__ import annotations

KIND_LIBRARY = "native-library"
KIND_PLATFORM = "platform-api"
KIND_RUNTIME = "runtime"

STATUS_PROVIDED = "provided"
STATUS_COMPATIBILITY = "compatibility"
STATUS_BLOCKED = "blocked"

#: Native libraries a converted image may legitimately depend on. This extends
#: the whitelist ``radek/apk.py:validate_apk`` checks against.
NATIVE_LIBRARIES = (
    "libc.so",
    "libm.so",
    "libdl.so",
    "liblog.so",
    "libandroid.so",
    "libz.so",
    "libGLESv1_CM.so",
    "libGLESv2.so",
    "libGLESv3.so",
    "libEGL.so",
    "libaaudio.so",
    "libmediandk.so",
    "libsqlite.so",
    "libc++_shared.so",
    "libioscompat.so",
)


class Provider:
    __slots__ = ("install_name", "framework", "android", "kind", "status", "detail")

    def __init__(self, install_name, framework, android, kind, status, detail):
        self.install_name = install_name
        self.framework = framework
        self.android = android
        self.kind = kind
        self.status = status
        self.detail = detail

    def as_dict(self) -> dict:
        return {
            "framework": self.framework,
            "provider": self.android,
            "providerKind": self.kind,
            "status": self.status,
            "reason": self.detail,
        }


TABLE: tuple[Provider, ...] = (
    Provider("OpenGLES.framework/OpenGLES", "OpenGLES",
             "libGLESv2.so · libGLESv3.so · libEGL.so · libGLESv1_CM.so", KIND_LIBRARY, STATUS_PROVIDED,
             "Android ships the identical Khronos OpenGL ES 1.1/2.0/3.x and EGL C ABI; gl*/egl* symbols bind directly."),
    Provider("libiconv.2.dylib", "libiconv", "bionic libc.so iconv · iconv_open · iconv_close",
             KIND_LIBRARY, STATUS_PROVIDED,
             "Android's libc exports the POSIX iconv API from API 28; the same entry points resolve at load time."),
    Provider("libsqlite3.dylib", "libsqlite3", "libsqlite.so sqlite3_*",
             KIND_LIBRARY, STATUS_PROVIDED, "Android ships the SQLite C API as libsqlite.so."),
    Provider("libSystem.B.dylib", "libSystem", "bionic libc.so · libm.so · libdl.so · liblog.so · pthreads",
             KIND_LIBRARY, STATUS_PROVIDED, "POSIX/libc/pthread surface is provided natively by bionic."),
    Provider("libc++.1.dylib", "libc++", "libc++_shared.so (NDK)",
             KIND_LIBRARY, STATUS_PROVIDED, "The same LLVM C++ runtime, packaged with the app."),
    Provider("libc++abi.dylib", "libc++abi", "libc++_shared.so (NDK)",
             KIND_LIBRARY, STATUS_PROVIDED, "C++ ABI support is part of the NDK runtime."),
    Provider("libstdc++.6.dylib", "libstdc++", "libc++_shared.so (NDK)",
             KIND_LIBRARY, STATUS_PROVIDED, "C++ standard library calls are served by the NDK runtime."),
    Provider("libz.1.dylib", "libz", "libz.so deflate · inflate · crc32",
             KIND_LIBRARY, STATUS_PROVIDED, "zlib is part of the Android platform."),
    Provider("libresolv.9.dylib", "libresolv", "bionic getaddrinfo · res_*",
             KIND_LIBRARY, STATUS_PROVIDED, "Resolver entry points are in bionic libc."),
    Provider("libcompression.dylib", "libcompression", "libz.so · java.util.zip.Inflater/Deflater",
             KIND_PLATFORM, STATUS_COMPATIBILITY,
             "zlib streams map directly; LZFSE/LZMA payloads are decoded by the runtime."),

    Provider("Foundation.framework/Foundation", "Foundation",
             "dev.radek.runtime.Foundation · libioscompat.so", KIND_RUNTIME, STATUS_COMPATIBILITY,
             "NSObject/NSString/NSData/NSArray/NSDictionary/NSNotificationCenter/NSUserDefaults over JVM objects, "
             "SharedPreferences and java.time."),
    Provider("CoreFoundation.framework/CoreFoundation", "CoreFoundation",
             "libioscompat.so (CFAbsoluteTimeGetCurrent, mach_absolute_time, mach_timebase_info)", KIND_RUNTIME, STATUS_COMPATIBILITY,
             "Concrete Bionic-backed clock shims only; CoreFoundation object, collection, and run-loop ABI is not implemented."),
    Provider("libobjc.A.dylib", "libobjc", "libioscompat.so message dispatch",
             KIND_RUNTIME, STATUS_COMPATIBILITY,
             "Class registration, selector interning, IMP lookup, inheritance and autorelease pools."),
    Provider("UIKit.framework/UIKit", "UIKit",
             "android.app.Activity · android.view.View/ViewGroup · TextView · ImageView · Bitmap",
             KIND_PLATFORM, STATUS_COMPATIBILITY,
             "UIView/UILabel/UIImageView/UIWindow/UIScreen/UIApplication are backed by the real Android view hierarchy."),
    Provider("CoreGraphics.framework/CoreGraphics", "CoreGraphics",
             "android.graphics.Canvas/Paint/Bitmap · NDK ACanvas/APaint · libioscompat.so affine math",
             KIND_PLATFORM, STATUS_COMPATIBILITY,
             "CGAffineTransform/CGPoint/CGRect math is native; drawing goes to a real Android Canvas."),
    Provider("QuartzCore.framework/QuartzCore", "QuartzCore",
             "libioscompat.so CACurrentMediaTime · android.view.Choreographer (candidate)", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "CACurrentMediaTime has a concrete CLOCK_MONOTONIC shim; CADisplayLink and the QuartzCore object ABI are not linked or implemented."),
    Provider("OpenAL.framework/OpenAL", "OpenAL", "libaaudio.so software mixer",
             KIND_PLATFORM, STATUS_COMPATIBILITY,
             "al*/alc* buffers and sources are mixed into a real AAudio low-latency output stream."),
    Provider("AudioToolbox.framework/AudioToolbox", "AudioToolbox",
             "libaaudio.so · android.media.AudioTrack · SoundPool", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "AudioQueue*/AudioServices*/ExtAudioFile over AAudio streams and platform players."),
    Provider("CoreAudio.framework/CoreAudio", "CoreAudio",
             "libaaudio.so · android.media.AudioManager", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "AudioStreamBasicDescription and AudioComponent rendering over AAudio."),
    Provider("AVFoundation.framework/AVFoundation", "AVFoundation",
             "android.media.MediaPlayer · MediaExtractor · MediaCodec", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "AVAudioPlayer/AVPlayer/AVAudioSession map to the platform media stack and AudioManager focus."),
    Provider("MediaPlayer.framework/MediaPlayer", "MediaPlayer",
             "android.media.MediaPlayer · SoundPool · AudioManager", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "MPMoviePlayer/MPMusicPlayer playback over the platform player and audio focus."),
    Provider("CoreMedia.framework/CoreMedia", "CoreMedia",
             "libmediandk.so AMediaFormat/AMediaCodec · android.media.MediaFormat", KIND_PLATFORM,
             STATUS_COMPATIBILITY,
             "CMTime arithmetic is native; sample buffers ride on MediaCodec buffers."),
    Provider("CoreVideo.framework/CoreVideo", "CoreVideo",
             "libandroid.so AHardwareBuffer · ANativeWindow · Surface", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "CVPixelBuffer storage is an AHardwareBuffer; buffer pools are Surface-backed."),
    Provider("CFNetwork.framework/CFNetwork", "CFNetwork",
             "bionic sockets · java.net.HttpURLConnection", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "CFReadStream/CFHTTPMessage over real sockets and the platform HTTP client."),
    Provider("CoreText.framework/CoreText", "CoreText",
             "android.graphics.Typeface · StaticLayout", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "Glyph runs and line breaking use the platform text stack."),
    Provider("ImageIO.framework/ImageIO", "ImageIO",
             "android.graphics.BitmapFactory · ImageDecoder", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "Image decoding, including Apple CgBI PNG payloads, is handled by the runtime decoder."),
    Provider("Security.framework/Security", "Security",
             "java.security · AndroidKeyStore", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "Keychain items map to EncryptedSharedPreferences/Keystore-backed keys."),
    Provider("SystemConfiguration.framework/SystemConfiguration", "SystemConfiguration",
             "android.net.ConnectivityManager", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "Reachability flags come from the platform connectivity callbacks."),
    Provider("MobileCoreServices.framework/MobileCoreServices", "MobileCoreServices",
             "android.webkit.MimeTypeMap", KIND_PLATFORM, STATUS_COMPATIBILITY,
             "UTI to MIME type translation."),
    Provider("Accelerate.framework/Accelerate", "Accelerate",
             "NEON intrinsics in libioscompat.so", KIND_RUNTIME, STATUS_COMPATIBILITY,
             "vDSP/vImage hot paths are implemented with ARM NEON."),
    Provider("Metal.framework/Metal", "Metal", "libvulkan.so · OpenGL ES",
             KIND_PLATFORM, STATUS_COMPATIBILITY,
             "Metal render pipelines fall back to Vulkan/GLES command translation."),
    Provider("WebKit.framework/WebKit", "WebKit", "android.webkit.WebView",
             KIND_PLATFORM, STATUS_COMPATIBILITY, "WKWebView content is rendered by the platform WebView."),
    Provider("AdSupport.framework/AdSupport", "AdSupport", "none",
             KIND_PLATFORM, STATUS_BLOCKED,
             "IDFA has no Android contract; the advertising id requires Play Services and explicit consent."),
    Provider("StoreKit.framework/StoreKit", "StoreKit", "none",
             KIND_PLATFORM, STATUS_BLOCKED,
             "In-app purchase receipts are an App Store contract; Play Billing is a different flow and is not faked."),
    Provider("GameKit.framework/GameKit", "GameKit", "none",
             KIND_PLATFORM, STATUS_BLOCKED,
             "Game Center multiplayer/leaderboards have no Android counterpart."),
    Provider("Social.framework/Social", "Social", "none",
             KIND_PLATFORM, STATUS_BLOCKED, "Share sheets are app-specific intents and are not impersonated."),
    Provider("MessageUI.framework/MessageUI", "MessageUI", "none",
             KIND_PLATFORM, STATUS_BLOCKED, "In-app mail/SMS composition is not provided."),
    Provider("AddressBook.framework/AddressBook", "AddressBook", "none",
             KIND_PLATFORM, STATUS_BLOCKED, "Contacts access requires its own runtime permission flow."),
    Provider("MapKit.framework/MapKit", "MapKit", "none",
             KIND_PLATFORM, STATUS_BLOCKED, "Apple Maps tiles cannot be served on Android."),
    Provider("iAd.framework/iAd", "iAd", "none", KIND_PLATFORM, STATUS_BLOCKED, "The iAd network no longer exists."),
    Provider("EventKit.framework/EventKit", "EventKit", "none",
             KIND_PLATFORM, STATUS_BLOCKED, "Calendar access requires its own permission flow."),
    Provider("CoreLocation.framework/CoreLocation", "CoreLocation", "none",
             KIND_PLATFORM, STATUS_BLOCKED,
             "Location requires a runtime permission flow the converted app has not requested."),
    Provider("CoreBluetooth.framework/CoreBluetooth", "CoreBluetooth", "none",
             KIND_PLATFORM, STATUS_BLOCKED, "Bluetooth LE needs its own permission and GATT stack binding."),
)

#: Exact time-API shims with compiled Android implementations. These functions
#: are available in libioscompat.so; they are not yet linked into converted games.
IMPLEMENTED_C_API_SHIMS = {
    "_CFAbsoluteTimeGetCurrent": "libioscompat.so:CFAbsoluteTimeGetCurrent",
    "_CACurrentMediaTime": "libioscompat.so:CACurrentMediaTime",
    "_mach_absolute_time": "libioscompat.so:mach_absolute_time",
    "_mach_timebase_info": "libioscompat.so:mach_timebase_info",
}

#: Exact reviewed libc/libm/libdl name candidates. A same-name candidate is not
#: proof that the Darwin ABI or its behavior can be linked safely.
BIONIC_SYMBOL_CANDIDATES = frozenset(
    """
    abort abs atoi atof calloc clock_gettime close exit fclose feof ferror fflush fgetc fgets fopen fprintf
    fputc fputs fread free fseek ftell fwrite getenv gettimeofday malloc memcmp memcpy memmove memset mkdir
    open perror printf puts read realloc remove rename rmdir scanf snprintf sprintf strcmp strcpy strdup strerror
    strlen strncat strncmp strncpy strnlen strrchr strchr strstr strtol strtoll strtoul strtoull tolower toupper
    unlink vsnprintf write __stack_chk_fail pthread_create pthread_join pthread_mutex_init pthread_mutex_lock
    pthread_mutex_unlock pthread_cond_init pthread_cond_wait pthread_cond_signal pthread_once socket connect send
    recv bind listen accept shutdown dlopen dlsym dlclose dlerror acos asin atan atan2 ceil cos exp fabs floor log
    pow sin sqrt tan acosf asinf atanf atan2f ceilf cosf expf fabsf floorf logf powf sinf sqrtf tanf
    """.split()
)

#: Broad symbol-family triage hints only; these are not proof of replacement code.
SYMBOL_PROVIDERS: tuple[tuple[str, str], ...] = (
    ("gl", "OpenGL ES (libGLESv2.so/libGLESv3.so)"),
    ("egl", "EGL (libEGL.so)"),
    ("al", "OpenAL over AAudio"),
    ("alc", "OpenAL context over AAudio"),
    ("Audio", "AudioToolbox/CoreAudio over AAudio"),
    ("ExtAudio", "ExtAudioFile over MediaExtractor"),
    ("CG", "CoreGraphics over android.graphics"),
    ("CA", "QuartzCore over Choreographer"),
    ("CM", "CoreMedia over MediaCodec"),
    ("CV", "CoreVideo over AHardwareBuffer"),
    ("CF", "CoreFoundation over the runtime"),
    ("NS", "Foundation over the runtime"),
    ("UI", "UIKit over android.view"),
    ("AV", "AVFoundation over android.media"),
    ("MP", "MediaPlayer over android.media"),
    ("iconv", "bionic iconv"),
    ("sqlite3", "libsqlite.so"),
    ("objc_", "libioscompat.so message dispatch"),
    ("dispatch_", "java.util.concurrent + pthreads"),
    ("pthread_", "bionic pthreads"),
    ("mach_absolute_time", "clock_gettime(CLOCK_MONOTONIC)"),
    ("Sec", "java.security/AndroidKeyStore"),
    ("inflate", "libz.so"),
    ("deflate", "libz.so"),
    ("uncompress", "libz.so"),
)


def for_install_name(path: str) -> Provider | None:
    """Longest-suffix match, so ``UIKit.framework/UIKit`` wins over a prefix."""
    best: Provider | None = None
    for provider in TABLE:
        if path.endswith(provider.install_name) or ("/" + provider.install_name) in path:
            if best is None or len(provider.install_name) > len(best.install_name):
                best = provider
    return best


def for_symbol(symbol: str) -> str | None:
    """Return an exact shim or triage hint; unknown lower-case names stay unknown."""
    if symbol in IMPLEMENTED_C_API_SHIMS:
        return IMPLEMENTED_C_API_SHIMS[symbol]
    name = symbol.lstrip("_")
    for prefix, provider in SYMBOL_PROVIDERS:
        if name.startswith(prefix):
            return provider
    if name in BIONIC_SYMBOL_CANDIDATES:
        return "bionic libc/libm/libdl (same-name candidate)"
    return None


def classify(install_name: str) -> dict:
    """Fields added to every dependency edge of a host report."""
    provider = for_install_name(install_name)
    if provider is None:
        return {
            "status": STATUS_BLOCKED,
            "provider": "",
            "providerKind": "",
            "reason": "No Android provider: this Darwin image is not on the verified mapping table",
        }
    return provider.as_dict()


def coverage(dependencies: list, imports: list) -> int:
    """Static provider/candidate triage share (0-100), not linked implementation coverage."""
    total = len(dependencies) + len(imports)
    if not total:
        return 100
    mapped = sum(
        1
        for name in dependencies
        if (provider := for_install_name(name)) is not None and provider.status != STATUS_BLOCKED
    )
    mapped += sum(1 for symbol in imports if for_symbol(symbol) is not None)
    return mapped * 100 // total
