package dev.radek.conventor

/** Exact ARM32 compatibility providers for Darwin-only imports.
 *
 * This catalog is intentionally separate from the same-name Android NDK catalog:
 * each entry names a typed compat-runtime-v1 adapter or guest-data binding.
 */
internal object CompatImportProviders {
    const val EXPECTED_DARWIN_ONLY_IMPORT_COUNT = 73

    val bySymbol: Map<String, String> = mapOf(
        "_AudioSessionInitialize" to "audio-session.initialize",
        "_AudioSessionSetActive" to "audio-session.set-active",
        "_NSSearchPathForDirectoriesInDomains" to "foundation.search-paths",
        "_OBJC_CLASS_\$_CAEAGLLayer" to "objc.class.CAEAGLLayer",
        "_OBJC_CLASS_\$_EAGLContext" to "objc.class.EAGLContext",
        "_OBJC_CLASS_\$_NSAutoreleasePool" to "objc.class.NSAutoreleasePool",
        "_OBJC_CLASS_\$_NSBundle" to "objc.class.NSBundle",
        "_OBJC_CLASS_\$_NSDictionary" to "objc.class.NSDictionary",
        "_OBJC_CLASS_\$_NSNumber" to "objc.class.NSNumber",
        "_OBJC_CLASS_\$_NSObject" to "objc.class.NSObject",
        "_OBJC_CLASS_\$_NSString" to "objc.class.NSString",
        "_OBJC_CLASS_\$_NSThread" to "objc.class.NSThread",
        "_OBJC_CLASS_\$_NSURL" to "objc.class.NSURL",
        "_OBJC_CLASS_\$_UIAccelerometer" to "objc.class.UIAccelerometer",
        "_OBJC_CLASS_\$_UIApplication" to "objc.class.UIApplication",
        "_OBJC_CLASS_\$_UIScreen" to "objc.class.UIScreen",
        "_OBJC_CLASS_\$_UIView" to "objc.class.UIView",
        "_OBJC_CLASS_\$_UIWindow" to "objc.class.UIWindow",
        "_OBJC_METACLASS_\$_NSObject" to "objc.metaclass.NSObject",
        "_OBJC_METACLASS_\$_UIView" to "objc.metaclass.UIView",
        "_UIApplicationMain" to "uikit.application-main",
        "__DefaultRuneLocale" to "darwin.rune-locale",
        "__Unwind_SjLj_Register" to "sjlj.register-context",
        "__Unwind_SjLj_Resume" to "sjlj.resume-boundary",
        "__Unwind_SjLj_Unregister" to "sjlj.unregister-context",
        "___CFConstantStringClassReference" to "corefoundation.constant-string-class",
        "___divdi3" to "compiler-runtime.divdi3",
        "___divsi3" to "compiler-runtime.divsi3",
        "___error" to "darwin.errno-cell",
        "___fixdfdi" to "compiler-runtime.fixdfdi",
        "___floatdidf" to "compiler-runtime.floatdidf",
        "___floatdisf" to "compiler-runtime.floatdisf",
        "___gxx_personality_sj0" to "cxxabi.gxx-personality-sj0",
        "___maskrune" to "darwin.ctype.maskrune",
        "___moddi3" to "compiler-runtime.moddi3",
        "___modsi3" to "compiler-runtime.modsi3",
        "___stderrp" to "darwin.stream.stderr",
        "___stdinp" to "darwin.stream.stdin",
        "___stdoutp" to "darwin.stream.stdout",
        "___tolower" to "darwin.ctype.tolower",
        "___toupper" to "darwin.ctype.toupper",
        "___udivsi3" to "compiler-runtime.udivsi3",
        "___umodsi3" to "compiler-runtime.umodsi3",
        "__objc_empty_cache" to "objc.data.empty-cache",
        "__objc_empty_vtable" to "objc.data.empty-vtable",
        "_alBufferData" to "openal.buffer-data",
        "_alDeleteBuffers" to "openal.delete-buffers",
        "_alDeleteSources" to "openal.delete-sources",
        "_alGenBuffers" to "openal.gen-buffers",
        "_alGenSources" to "openal.gen-sources",
        "_alGetSourcef" to "openal.get-source-float",
        "_alGetSourcei" to "openal.get-source-int",
        "_alSource3f" to "openal.source-3-float",
        "_alSourcePlay" to "openal.source-play",
        "_alSourceQueueBuffers" to "openal.source-queue",
        "_alSourceStop" to "openal.source-stop",
        "_alSourceUnqueueBuffers" to "openal.source-unqueue",
        "_alSourcef" to "openal.source-float",
        "_alSourcei" to "openal.source-int",
        "_alcCloseDevice" to "openal.close-device",
        "_alcCreateContext" to "openal.create-context",
        "_alcDestroyContext" to "openal.destroy-context",
        "_alcMakeContextCurrent" to "openal.make-context-current",
        "_alcOpenDevice" to "openal.open-device",
        "_kEAGLColorFormatRGB565" to "eagl.constant.RGB565",
        "_kEAGLColorFormatRGBA8" to "eagl.constant.RGBA8",
        "_kEAGLDrawablePropertyColorFormat" to "eagl.constant.color-format",
        "_kEAGLDrawablePropertyRetainedBacking" to "eagl.constant.retained-backing",
        "_objc_enumerationMutation" to "objc.enumeration-mutation",
        "_objc_msgSend" to "objc.msgSend",
        "_objc_msgSendSuper2" to "objc.msgSendSuper2",
        "_objc_msgSend_stret" to "objc.msgSend.stret",
        "_objc_setProperty" to "objc.setProperty",
    )

    init {
        check(bySymbol.size == EXPECTED_DARWIN_ONLY_IMPORT_COUNT) {
            "Darwin compatibility provider catalog drifted: ${bySymbol.size}"
        }
    }

    fun providerFor(symbol: String): String? = bySymbol[symbol]
}
