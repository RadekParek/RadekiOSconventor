package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/**
 * iOS install names and external symbols mapped to the Android implementation
 * that actually provides them.
 *
 * Entries are provider candidates, not universal support claims. A [KIND_LIBRARY]
 * entry can name an Android system ABI; [KIND_PLATFORM] and [KIND_RUNTIME] entries
 * are triage hints until an ABI-safe adapter is compiled and wired into generated
 * output. Only individually tested compatibility exports are verified; unimplemented
 * imports remain explicit blockers or stubs.
 */
object Providers {
    const val KIND_LIBRARY = "native-library"
    const val KIND_PLATFORM = "platform-api"
    const val KIND_RUNTIME = "runtime"

    const val STATUS_PROVIDED = "provided"
    const val STATUS_COMPATIBILITY = "compatibility"
    const val STATUS_BLOCKED = "blocked"

    /** Native libraries the converted image may legitimately depend on. */
    val NATIVE_LIBRARIES = listOf(
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so",
        "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libEGL.so", "libaaudio.so",
        "libmediandk.so", "libsqlite.so", "libc++_shared.so", "libioscompat.so"
    )

    data class Provider(
        val installName: String,
        val framework: String,
        val android: String,
        val kind: String,
        val status: String,
        val detail: String
    )

    private val TABLE = listOf(
        Provider("OpenGLES.framework/OpenGLES", "OpenGLES", "libGLESv2.so · libGLESv3.so · libEGL.so · libGLESv1_CM.so",
            KIND_LIBRARY, STATUS_PROVIDED,
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
        Provider("libgcc_s.1.dylib", "libgcc_s", "NDK compiler-rt builtins · NDK libunwind/libc++abi",
            KIND_RUNTIME, STATUS_COMPATIBILITY,
            "Android NDK does not ship a drop-in libgcc_s.so. Compiler helper symbols need toolchain compiler-rt builtins; unwind/personality symbols need per-symbol NDK libunwind/libc++abi validation. The install-name mapping is a candidate, not a loadable-library alias or completed link."),
        Provider("libz.1.dylib", "libz", "libz.so deflate · inflate · crc32",
            KIND_LIBRARY, STATUS_PROVIDED, "zlib is part of the Android platform."),
        Provider("libresolv.9.dylib", "libresolv", "bionic getaddrinfo · res_*",
            KIND_LIBRARY, STATUS_PROVIDED, "Resolver entry points are in bionic libc."),
        Provider("libcompression.dylib", "libcompression", "libz.so · java.util.zip.Inflater/Deflater",
            KIND_PLATFORM, STATUS_COMPATIBILITY, "zlib streams map directly; LZFSE/LZMA payloads are decoded by the runtime."),

        Provider("Foundation.framework/Foundation", "Foundation",
            "dev.radek.runtime.Foundation · libioscompat.so", KIND_RUNTIME, STATUS_COMPATIBILITY,
            "NSObject/NSString/NSData/NSArray/NSDictionary/NSNotificationCenter/NSUserDefaults over JVM objects, SharedPreferences and java.time."),
        Provider("CoreFoundation.framework/CoreFoundation", "CoreFoundation",
            "libioscompat.so (CFString/CFData/CFArray/CFDictionary/CFNumber/CFDate + CFRunLoop subset)", KIND_RUNTIME, STATUS_COMPATIBILITY,
            "A small host-tested opaque CF object/collection model and queued C-callback run-loop subset are implemented. It is not the complete CoreFoundation ABI: Apple callbacks/Blocks, run-loop sources/timers, toll-free bridging and the Objective-C runtime remain unsupported."),
        Provider("libobjc.A.dylib", "libobjc", "libioscompat.so message dispatch",
            KIND_RUNTIME, STATUS_COMPATIBILITY, "Class registration, selector interning, IMP lookup, inheritance and autorelease pools."),
        Provider("UIKit.framework/UIKit", "UIKit",
            "android.app.Activity · android.view.View/ViewGroup · TextView · ImageView · Bitmap",
            KIND_PLATFORM, STATUS_COMPATIBILITY,
            "UIView/UILabel/UIImageView/UIWindow/UIScreen/UIApplication are backed by the real Android view hierarchy."),
        Provider("CoreGraphics.framework/CoreGraphics", "CoreGraphics",
            "android.graphics.Canvas/Paint/Bitmap · NDK ACanvas/APaint · libioscompat.so affine math",
            KIND_PLATFORM, STATUS_COMPATIBILITY,
            "CGAffineTransform/CGPoint/CGRect math is native; drawing goes to a real Android Canvas."),
        Provider("QuartzCore.framework/QuartzCore", "QuartzCore",
            "libioscompat.so C frame-link callback API · android.view.Choreographer", KIND_PLATFORM, STATUS_COMPATIBILITY,
            "libioscompat.so implements a native C frame-link callback service driven by Choreographer in the bounded converted launcher. It is not the Objective-C CADisplayLink class/selector ABI; CAAnimation, CALayer and callsite rewriting remain unsupported."),
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
            "libmediandk.so AMediaFormat/AMediaCodec · android.media.MediaFormat", KIND_PLATFORM, STATUS_COMPATIBILITY,
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
            "UTI to MIME type static recompilation."),
        Provider("Accelerate.framework/Accelerate", "Accelerate",
            "NEON intrinsics in libioscompat.so", KIND_RUNTIME, STATUS_COMPATIBILITY,
            "vDSP/vImage hot paths are implemented with ARM NEON."),
        Provider("Metal.framework/Metal", "Metal", "libvulkan.so · OpenGL ES",
            KIND_PLATFORM, STATUS_COMPATIBILITY, "Metal render pipelines fall back to Vulkan/GLES command static recompilation."),
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
            KIND_PLATFORM, STATUS_BLOCKED, "Location requires a runtime permission flow the converted app has not requested."),
        Provider("CoreBluetooth.framework/CoreBluetooth", "CoreBluetooth", "none",
            KIND_PLATFORM, STATUS_BLOCKED, "Bluetooth LE needs its own permission and GATT stack binding.")
    )

    /**
     * Broad symbol-family triage hints only; these are not proof of generated or linked code.
     * Concrete compiled compatibility exports are tracked separately by AndroidApiMapper; broad
     * framework-family labels remain candidates.
     */
    private val SYMBOL_PROVIDERS = listOf(
        "gl" to "OpenGL ES (libGLESv2.so/libGLESv3.so)",
        "egl" to "EGL (libEGL.so)",
        "al" to "OpenAL over AAudio",
        "alc" to "OpenAL context over AAudio",
        "Audio" to "AudioToolbox/CoreAudio over AAudio",
        "ExtAudio" to "ExtAudioFile over MediaExtractor",
        "CG" to "CoreGraphics over android.graphics",
        "CA" to "QuartzCore; partial C frame-clock bridge only",
        "CM" to "CoreMedia over MediaCodec",
        "CV" to "CoreVideo over AHardwareBuffer",
        "CF" to "CoreFoundation over the runtime",
        "NS" to "Foundation over the runtime",
        "UI" to "UIKit over android.view",
        "AV" to "AVFoundation over android.media",
        "MP" to "MediaPlayer over android.media",
        "iconv" to "bionic iconv",
        "sqlite3" to "libsqlite.so",
        "objc_" to "libioscompat.so message dispatch",
        "dispatch_" to "java.util.concurrent + pthreads",
        "pthread_" to "bionic pthreads",
        "mach_absolute_time" to "clock_gettime(CLOCK_MONOTONIC)",
        "Sec" to "java.security/AndroidKeyStore",
        "inflate" to "libz.so",
        "deflate" to "libz.so",
        "uncompress" to "libz.so"
    )

    private fun compilerRuntimeCandidate(symbol: String): String? {
        val name = symbol.trimStart('_')
        val unwind = listOf("Unwind_", "gcc_personality_v0", "gxx_personality_v0", "aeabi_unwind_", "gnu_unwind_")
        if (unwind.any { prefix -> name.startsWith(prefix) }) {
            return "NDK libunwind/libc++abi (unwind ABI candidate; not linked)"
        }
        val builtins = listOf(
            "aeabi_", "divdi3", "udivdi3", "moddi3", "umoddi3", "muldi3", "ashldi3", "ashrdi3",
            "lshrdi3", "udivmoddi4", "divti3", "udivti3", "modti3", "umodti3", "multi3", "muloti4",
            "ashlti3", "ashrti3", "lshrti3", "addvti3", "subvti3", "absvti2", "cmpdi2", "ucmpdi2",
            "clear_cache", "register_frame", "deregister_frame", "fix", "float",
        )
        return if (builtins.any { prefix -> name.startsWith(prefix) }) {
            "NDK compiler-rt builtins (toolchain link candidate; not linked)"
        } else null
    }

    fun forInstallName(path: String): Provider? {
        val trimmed = path.trim()
        var best: Provider? = null
        for (provider in TABLE) {
            if (trimmed.endsWith(provider.installName) || trimmed.contains("/" + provider.installName)) {
                if (best == null || provider.installName.length > best.installName.length) best = provider
            }
        }
        return best
    }

    /** Provider for an imported symbol name, or null when nothing maps it. */
    fun forSymbol(symbol: String): String? {
        AndroidApiMapper.compiledCompatibilityProvider(symbol)?.let { return it }
        compilerRuntimeCandidate(symbol)?.let { return it }
        val candidate = symbol.trimStart('_')
        for ((prefix, provider) in SYMBOL_PROVIDERS) if (candidate.startsWith(prefix)) return provider
        // libc/libm/pthread entry points are provided by bionic directly.
        if (candidate.isNotEmpty() && candidate[0].isLowerCase()) return "bionic libc/libm"
        return null
    }

    /**
     * Classify one dependency edge exactly like `radek/analysis.py` does on the
     * host, so the app and the CLI report the same thing.
     */
    fun classify(installName: String): JSONObject {
        val provider = forInstallName(installName)
        return if (provider == null) {
            JSONObject().put("installName", installName).put("classification", "unsupported")
                .put("status", STATUS_BLOCKED).put("provider", "")
                .put("reason", "No Android provider: this Darwin image is not on the verified mapping table")
        } else {
            JSONObject().put("installName", installName)
                .put("framework", provider.framework)
                .put("classification", if (provider.status == STATUS_BLOCKED) "unsupported" else "provided")
                .put("status", provider.status).put("kind", provider.kind)
                .put("provider", provider.android)
                .put("reason", provider.detail)
        }
    }

    /**
     * Candidate-catalog coverage only. A non-null candidate does not prove ABI
     * compatibility or that its code is linked; callers must keep this separate
     * from verified exports and generated/linked implementation counts.
     */
    fun coverage(dependencies: JSONArray, imports: JSONArray): Int {
        val total = dependencies.length() + imports.length()
        if (total == 0) return 100
        var mapped = 0
        for (i in 0 until dependencies.length()) {
            val name = dependencies.getJSONObject(i).optString("path")
            val provider = forInstallName(name)
            if (provider != null && provider.status != STATUS_BLOCKED) mapped++
        }
        for (i in 0 until imports.length()) {
            val name = imports.getJSONObject(i).optString("name")
            if (forSymbol(name) != null) mapped++
        }
        return mapped * 100 / total
    }

    /** Human-readable table for the detail screen. */
    fun describe(installName: String): String {
        val provider = forInstallName(installName) ?: return "BLOCKED · no Android provider"
        val marker = when (provider.status) {
            STATUS_PROVIDED -> "PROVIDED"
            STATUS_COMPATIBILITY -> "COMPAT"
            else -> "BLOCKED"
        }
        return "$marker · ${provider.framework} → ${provider.android}"
    }
}
