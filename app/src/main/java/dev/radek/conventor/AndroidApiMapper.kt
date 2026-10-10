package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/**
 * Conservative native-symbol and semantic API equivalence inventory.
 * A candidate is a static recompilation plan only: this module does not rewrite Mach-O
 * code, bridge Objective-C objects, link a Bionic library, or generate Java.
 */
internal object AndroidApiMapper {
    // Keep high-volume games analyzable while bounding mapper/report growth.
    private const val MAX_SYMBOLS = 100_000

    /**
     * Classifications that describe a *reviewed Android mapping* of some kind:
     * a same-name NDK/system candidate, an NDK compiler-runtime toolchain
     * candidate, a guest-runtime adapter catalog entry, a compiled
     * compatibility implementation, or a reviewed semantic target.
     * `COMPAT_STUB_HANDLER_REGISTERED` (an explicitly unimplemented
     * handler) and `UNMAPPED` are deliberately not members.
     */
    private val REVIEWED_MAPPING_CLASSIFICATIONS = setOf(
        "BIONIC_SYMBOL_CANDIDATE",
        "COMPILER_RUNTIME_CANDIDATE",
        "GUEST_RUNTIME_ADAPTER_CATALOGUED",
        "IMPLEMENTED_API_REPLACEMENT_AVAILABLE",
        "COMPAT_VERIFIED_HANDLER_RESOLVED",
        "SEMANTIC_REWRITE_CANDIDATE",
    )

    private val ndkRuntimeLibraries = setOf(
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so", "libEGL.so",
        "libGLESv1_CM.so", "libGLESv2.so", "libaaudio.so", "libmediandk.so", "libvulkan.so",
        "libOpenSLES.so", "libOpenMAXAL.so", "libjnigraphics.so", "libbinder_ndk.so", "libamidi.so",
        "libcamera2ndk.so", "libc++_shared.so", "libnativewindow.so", "libneuralnetworks.so",
        "libsync.so",
    )

    /**
     * Reviewed same-name candidates in Android's public NDK/system libraries.
     *
     * Every entry is a *name* match in a library Android actually ships; a
     * candidate still needs caller-ABI, struct-layout and relocation
     * verification before anything can be linked. The catalog is the
     * device-side twin of `BIONIC_SYMBOL_CANDIDATES` in `radek/providers.py`;
     * `tests/test_providers.py` asserts the two tables stay equal.
     */
    private val bionicLibraries = mapOf(
        "libc.so" to setOf(
            "__assert", "__assert2", "__cxa_atexit", "__cxa_finalize", "__errno", "__libc_current_sigrtmax",
            "__libc_current_sigrtmin", "__memcpy_chk", "__memmove_chk", "__memset_chk", "__sprintf_chk",
            "__stack_chk_fail", "__strcat_chk", "__strcpy_chk", "__strlen_chk", "__vsnprintf_chk",
            "__vsprintf_chk", "_exit", "_longjmp", "_setjmp", "abort", "abs", "accept", "access", "alarm",
            "asctime", "asctime_r", "atexit", "atof", "atoi", "atol", "atoll", "basename", "bcmp", "bcopy", "bind",
            "brk", "bsearch", "btowc", "bzero", "calloc", "chdir", "chmod", "clearerr", "clock", "clock_gettime",
            "clock_settime", "close", "closedir", "closelog", "connect", "creat", "ctime", "ctime_r", "difftime",
            "dirname", "dup", "dup2", "endgrent", "endpwent", "environ", "error", "error_at_line", "error_message_count",
            "error_one_per_line", "error_print_progname", "execl", "execle", "execlp", "execv", "execve",
            "execvp", "exit", "fchmod", "fchown", "fclose", "fcntl", "fdatasync", "fdopen", "feof", "ferror",
            "fflush", "ffs", "ffsl", "ffsll", "fgetc", "fgetpos", "fgets", "fileno", "flock", "fmemopen",
            "fnmatch", "fopen", "fprintf", "fputc", "fputs", "fread", "free", "freeaddrinfo", "freopen", "fscanf",
            "fseek", "fseeko", "fsetpos", "fstat", "fstatat", "fstatfs", "fstatvfs", "fsync", "ftell", "ftello",
            "ftruncate", "fwrite", "gai_strerror", "getc", "getcwd", "getdtablesize", "getegid", "getenv",
            "geteuid", "getgid", "getgrent", "getgrgid", "getgrnam", "getgroups", "gethostbyaddr", "gethostbyname",
            "gethostname", "getline", "getlogin", "getopt", "getopt_long", "getopt_long_only", "getpagesize",
            "getpeername", "getpgrp", "getpid", "getppid", "getpriority", "getprogname", "getpwent", "getpwnam",
            "getpwuid", "getservbyname", "getservbyport", "getsockname", "getsockopt", "gettid", "gettimeofday",
            "getuid", "gmtime", "gmtime_r", "hstrerror", "htonl", "htons", "iconv", "iconv_close", "iconv_open",
            "if_indextoname", "if_nametoindex", "inet_addr", "inet_aton", "inet_ntoa", "inet_ntop", "inet_pton",
            "ioctl", "isalnum", "isalpha", "isatty", "isblank", "iscntrl", "isdigit", "isgraph", "islower",
            "isprint", "ispunct", "isspace", "isupper", "isxdigit", "kill", "labs", "lchown", "ldexp", "link", "listen",
            "llabs", "localeconv", "localtime", "localtime_r", "longjmp", "lseek", "lstat", "madvise", "malloc",
            "malloc_usable_size", "mallopt", "mblen", "mbrlen", "mbrtowc", "mbsinit", "mbsrtowcs", "mbstowcs",
            "mbtowc", "memalign", "memccpy", "memchr", "memcmp", "memcpy", "memmem", "memmove", "mempcpy",
            "memrchr", "memset", "mincore", "mkdir", "mkdtemp", "mkstemp", "mktime", "mlock", "mlockall", "mmap",
            "mprotect", "mremap", "msync", "munlock", "munlockall", "munmap", "nanosleep", "newlocale",
            "nl_langinfo", "ntohl", "ntohs", "open", "opendir", "openlog", "pathconf", "pause", "pclose", "perror",
            "pipe", "poll", "popen", "printf", "pselect", "psignal", "pthread_atfork", "pthread_attr_destroy",
            "pthread_attr_init", "pthread_attr_setdetachstate", "pthread_attr_setschedparam",
            "pthread_attr_setstacksize", "pthread_cond_broadcast", "pthread_cond_destroy", "pthread_cond_init",
            "pthread_cond_signal", "pthread_cond_timedwait", "pthread_cond_wait", "pthread_create",
            "pthread_detach", "pthread_equal", "pthread_exit", "pthread_getschedparam", "pthread_getspecific",
            "pthread_join", "pthread_key_create", "pthread_key_delete", "pthread_kill", "pthread_mutex_destroy",
            "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_unlock",
            "pthread_mutexattr_destroy", "pthread_mutexattr_init", "pthread_mutexattr_settype", "pthread_once",
            "pthread_rwlock_destroy", "pthread_rwlock_rdlock", "pthread_rwlock_unlock", "pthread_rwlock_wrlock",
            "pthread_self", "pthread_setname_np", "pthread_setschedparam", "pthread_setspecific",
            "pthread_sigmask", "putc", "putchar", "putenv", "puts", "qsort", "raise", "rand", "random", "read",
            "readdir", "readdir_r", "readlink", "readv", "realloc", "realpath", "recv", "recvfrom", "recvmsg",
            "regcomp", "regerror", "regexec", "regfree", "remove", "rename", "rewind", "rewinddir", "rmdir",
            "sbrk", "scandir", "scanf", "sched_yield", "seekdir", "select", "sem_destroy", "sem_getvalue",
            "sem_init", "sem_post", "sem_timedwait", "sem_trywait", "sem_wait", "send", "sendmsg", "sendto",
            "setbuf", "setbuffer", "setenv", "setgrent", "setgroups", "setjmp", "setlinebuf", "setlocale",
            "setpriority", "setprogname", "setpwent", "setsockopt", "setvbuf", "sigaction", "sigaddset",
            "sigaltstack", "sigdelset", "sigemptyset", "sigfillset", "sigismember", "signal", "sigprocmask",
            "sigwait", "sleep", "snprintf", "socket", "socketpair", "sprintf", "srand", "srandom", "sscanf",
            "stat", "statfs", "statvfs", "strcasecmp", "strcasestr", "strcat", "strchr", "strchrnul", "strcmp",
            "strcoll", "strcpy", "strcspn", "strdup", "strerror", "strerror_r", "strftime", "strlcat", "strlcpy",
            "strlen", "strncasecmp", "strncat", "strncmp", "strncpy", "strndup", "strnlen", "strpbrk", "strptime",
            "strrchr", "strsep", "strsignal", "strspn", "strstr", "strtod", "strtof", "strtoimax", "strtok",
            "strtok_r", "strtol", "strtold", "strtoll", "strtoul", "strtoull", "strtoumax", "strxfrm", "swab",
            "symlink", "symlinkat", "sysconf", "syslog", "system", "tcdrain", "tcflow", "tcflush", "tcgetattr",
            "tcsendbreak", "tcsetattr", "telldir", "tempnam", "time", "timegm", "tmpfile", "tmpnam", "tolower",
            "toupper", "towlower", "towupper", "truncate", "ttyname", "tzset", "umask", "uname", "ungetc",
            "unlink", "unlinkat", "unsetenv", "uselocale", "usleep", "utime", "utimensat", "utimes", "vasprintf",
            "vdprintf", "vfork", "vfprintf", "vfscanf", "vprintf", "vscanf", "vsnprintf", "vsprintf", "vsscanf",
            "vsyslog", "wait", "wait3", "wait4", "waitpid", "wcrtomb", "wcschr", "wcscmp", "wcscoll", "wcscpy",
            "wcscspn", "wcslen", "wcsncat", "wcsncmp", "wcsncpy", "wcspbrk", "wcsrchr", "wcsrtombs", "wcsstr",
            "wcstod", "wcstol", "wcstombs", "wcstoul", "wctomb", "wcwidth", "wmemchr", "wmemcmp", "wmemcpy",
            "wmemmove", "wmemset", "write", "writev",
        ),
        "libdl.so" to setOf(
            "android_dlopen_ext", "dl_iterate_phdr", "dladdr", "dlclose", "dlerror", "dlinfo", "dlopen", "dlsym",
        ),
        "libm.so" to setOf(
            "acos", "acosf", "acosh", "acoshf", "asin", "asinf", "asinh", "asinhf", "atan", "atan2", "atan2f",
            "atanf", "atanh", "atanhf", "cabs", "cabsf", "cacos", "cacosf", "cacosh", "cacoshf", "carg", "cargf",
            "casin", "casinf", "casinh", "casinhf", "catan", "catanf", "catanh", "catanhf", "cbrt", "cbrtf", "ccos",
            "ccosf", "ccosh", "ccoshf", "ceil", "ceilf", "cexp", "cexpf", "cimag", "cimagf", "clog", "clogf", "conj",
            "conjf", "copysign", "copysignf", "cos", "cosf", "cosh", "coshf", "cpow", "cpowf", "cproj", "cprojf",
            "creal", "crealf", "csin", "csinf", "csinh", "csinhf", "csqrt", "csqrtf", "ctan", "ctanf", "ctanh",
            "ctanhf", "erf", "erfc", "erfcf", "erff", "exp", "exp2", "exp2f", "expf", "expm1", "expm1f", "fabs",
            "fabsf", "fdim", "fdimf", "floor", "floorf", "fma", "fmaf", "fmax", "fmaxf", "fmin", "fminf", "fmod",
            "fmodf", "frexp", "frexpf", "hypot", "hypotf", "ilogb", "ilogbf", "j0", "j1", "jn", "ldexpf",
            "lgamma", "lgamma_r", "lgammaf", "lgammaf_r", "llrint", "llrintf", "llround", "llroundf", "log", "log10",
            "log10f", "log1p", "log1pf", "log2", "log2f", "logb", "logbf", "logf", "lrint", "lrintf", "lround",
            "lroundf", "modf", "modff", "nan", "nanf", "nearbyint", "nearbyintf", "nextafter", "nextafterf",
            "nexttoward", "pow", "powf", "remainder", "remainderf", "remquo", "remquof", "rint", "rintf", "round",
            "roundf", "scalbn", "scalbnf", "significand", "sin", "sinf", "sinh", "sinhf", "sqrt", "sqrtf", "tan",
            "tanf", "tanh", "tanhf", "tgamma", "tgammaf", "trunc", "truncf", "y0", "y1", "yn",
        ),
        "libz.so" to setOf(
            "adler32", "adler32_combine", "adler32_z", "compress", "compress2", "compressBound", "crc32",
            "crc32_combine", "crc32_z", "deflate", "deflateBound", "deflateCopy", "deflateEnd",
            "deflateGetDictionary", "deflateInit2_", "deflateInit_", "deflateParams", "deflatePending",
            "deflatePrime", "deflateReset", "deflateResetKeep", "deflateSetDictionary", "deflateSetHeader",
            "deflateTune", "gzclearerr", "gzclose", "gzeof", "gzerror", "gzflush", "gzgetc", "gzgets", "gzoffset",
            "gzopen", "gzprintf", "gzputc", "gzputs", "gzread", "gzrewind", "gzseek", "gztell", "gzungetc", "gzwrite",
            "inflate", "inflateBack", "inflateBackEnd", "inflateBackInit_", "inflateCodesUsed", "inflateCopy",
            "inflateEnd", "inflateGetDictionary", "inflateGetHeader", "inflateInit2_", "inflateInit_", "inflatePrime",
            "inflateReset", "inflateReset2", "inflateResetKeep", "inflateSetDictionary", "inflateSync",
            "inflateSyncPoint", "inflateUndermine", "inflateValidate", "uncompress", "uncompress2", "unzClose",
            "unzOpen", "unzReadCurrentFile", "zlibCompileFlags", "zlibVersion",
        ),
        "libEGL.so" to setOf(
            "eglBindAPI", "eglBindTexImage", "eglChooseConfig", "eglClientWaitSyncKHR", "eglCopyBuffers",
            "eglCreateContext", "eglCreateImageKHR", "eglCreatePbufferFromClientBuffer", "eglCreatePbufferSurface",
            "eglCreatePixmapSurface", "eglCreatePlatformPixmapSurface", "eglCreatePlatformWindowSurface",
            "eglCreateSyncKHR", "eglCreateWindowSurface", "eglDestroyContext", "eglDestroyImageKHR",
            "eglDestroySurface", "eglDestroySyncKHR", "eglGetConfigAttrib", "eglGetConfigs", "eglGetCurrentContext",
            "eglGetCurrentDisplay", "eglGetCurrentSurface", "eglGetDisplay", "eglGetError", "eglGetPlatformDisplay",
            "eglGetProcAddress", "eglGetSyncAttribKHR", "eglInitialize", "eglMakeCurrent",
            "eglPresentationTimeANDROID", "eglQueryAPI", "eglQueryContext", "eglQueryString", "eglQuerySurface",
            "eglQuerySurfacePointerANGLE", "eglReleaseTexImage", "eglReleaseThread", "eglSetDamageRegionKHR",
            "eglSignalSyncKHR", "eglSurfaceAttrib", "eglSwapBuffers", "eglSwapBuffersWithDamageKHR",
            "eglSwapInterval", "eglTerminate", "eglWaitClient", "eglWaitGL", "eglWaitNative", "eglWaitSyncKHR",
        ),
        "libGLESv1_CM.so" to setOf(
            "glAlphaFunc", "glAlphaFuncx", "glBindFramebufferOES", "glBindRenderbufferOES", "glBlendEquationOES",
            "glBlendEquationSeparateOES", "glBlendFuncSeparateOES", "glCheckFramebufferStatusOES", "glClearColorx",
            "glClientActiveTexture", "glClipPlanef", "glClipPlanex", "glColor4f", "glColor4ub", "glColor4x",
            "glColorPointer", "glDeleteFramebuffersOES", "glDeleteRenderbuffersOES", "glDisableClientState",
            "glDrawTexfOES", "glDrawTexfvOES", "glDrawTexiOES", "glDrawTexivOES", "glDrawTexsOES",
            "glDrawTexsvOES", "glDrawTexxOES", "glDrawTexxvOES", "glEGLImageTargetRenderbufferStorageOES",
            "glEGLImageTargetTexture2DOES", "glEnableClientState", "glFogf", "glFogfv", "glFogi", "glFogiv",
            "glFogx", "glFogxv", "glFramebufferRenderbufferOES", "glFramebufferTexture2DOES", "glFrustumf",
            "glFrustumx", "glGenFramebuffersOES", "glGenRenderbuffersOES", "glGenerateMipmapOES", "glGetClipPlane",
            "glGetClipPlanef", "glGetClipPlanex", "glGetFixedv", "glGetLightfv", "glGetLightiv", "glGetLightxv",
            "glGetMaterialfv", "glGetMaterialiv", "glGetMaterialxv", "glGetPointerv",
            "glGetRenderbufferParameterivOES", "glGetTexEnviv", "glGetTexEnvxv", "glGetTexGenfvOES",
            "glGetTexGenivOES", "glGetTexGenxvOES", "glGetTexParameterxv", "glIsFramebufferOES",
            "glIsRenderbufferOES", "glLightModelf", "glLightModelfv", "glLightModelx", "glLightModelxv",
            "glLightf", "glLightfv", "glLighti", "glLightiv", "glLightx", "glLightxv", "glLineWidthx",
            "glLoadIdentity", "glLoadMatrixf", "glLoadMatrixx", "glLogicOp", "glMaterialf", "glMaterialfv",
            "glMaterialx", "glMaterialxv", "glMatrixMode", "glMultMatrixf", "glMultMatrixx", "glMultiTexCoord4f",
            "glMultiTexCoord4x", "glNormal3f", "glNormal3x", "glNormalPointer", "glOrthof", "glOrthox",
            "glPointParameterf", "glPointParameterfv", "glPointSize", "glPointSizePointerOES", "glPointSizex",
            "glPolygonOffsetx", "glPopMatrix", "glPushMatrix", "glQueryMatrixxOES", "glRenderbufferStorageOES",
            "glRotatef", "glRotatex", "glSampleCoveragex", "glScalef", "glScalex", "glShadeModel",
            "glTexCoordPointer", "glTexEnvf", "glTexEnvfv", "glTexEnvi", "glTexEnviv", "glTexEnvx", "glTexEnvxv",
            "glTexGenfOES", "glTexGenfvOES", "glTexGeniOES", "glTexGenivOES", "glTexGenxOES", "glTexGenxvOES",
            "glTexParameterx", "glTexParameterxv", "glTranslatef", "glTranslatex", "glVertexPointer",
        ),
        "libGLESv2.so" to setOf(
            "glActiveTexture", "glAttachShader", "glBeginQueryEXT", "glBindAttribLocation", "glBindBuffer",
            "glBindFramebuffer", "glBindRenderbuffer", "glBindTexture", "glBlendColor", "glBlendEquation",
            "glBlendEquationSeparate", "glBlendFunc", "glBlendFuncSeparate", "glBufferData", "glBufferSubData",
            "glCheckFramebufferStatus", "glClear", "glClearColor", "glClearDepthf", "glClearStencil", "glColorMask",
            "glCompileShader", "glCompressedTexImage2D", "glCompressedTexSubImage2D", "glCopyTexImage2D",
            "glCopyTexSubImage2D", "glCreateProgram", "glCreateShader", "glCullFace", "glDeleteBuffers",
            "glDeleteFramebuffers", "glDeleteProgram", "glDeleteQueriesEXT", "glDeleteRenderbuffers",
            "glDeleteShader", "glDeleteTextures", "glDepthFunc", "glDepthMask", "glDepthRangef", "glDetachShader",
            "glDisable", "glDisableVertexAttribArray", "glDiscardFramebufferEXT", "glDrawArrays", "glDrawElements",
            "glEnable", "glEnableVertexAttribArray", "glEndQueryEXT", "glFinish", "glFlush",
            "glFramebufferRenderbuffer", "glFramebufferTexture2D", "glFrontFace", "glGenBuffers", "glGenFramebuffers",
            "glGenQueriesEXT", "glGenRenderbuffers", "glGenTextures", "glGenerateMipmap", "glGetActiveAttrib",
            "glGetActiveUniform", "glGetAttribLocation", "glGetBooleanv", "glGetBufferParameteriv", "glGetError",
            "glGetFloatv", "glGetFramebufferAttachmentParameteriv", "glGetIntegerv", "glGetProgramBinaryOES",
            "glGetProgramInfoLog", "glGetProgramiv", "glGetQueryObjectuivEXT", "glGetQueryivEXT",
            "glGetRenderbufferParameteriv", "glGetShaderInfoLog", "glGetShaderPrecisionFormat", "glGetShaderiv",
            "glGetString", "glGetTexParameterfv", "glGetTexParameteriv", "glGetUniformLocation", "glGetUniformfv",
            "glGetUniformiv", "glGetVertexAttribPointerv", "glGetVertexAttribfv", "glGetVertexAttribiv", "glHint",
            "glIsBuffer", "glIsEnabled", "glIsFramebuffer", "glIsProgram", "glIsQueryEXT", "glIsRenderbuffer",
            "glIsShader", "glIsTexture", "glLineWidth", "glLinkProgram", "glPixelStorei", "glPolygonOffset",
            "glProgramBinaryOES", "glReadPixels", "glReleaseShaderCompiler", "glRenderbufferStorage",
            "glSampleCoverage", "glScissor", "glShaderBinary", "glShaderSource", "glStencilFunc",
            "glStencilFuncSeparate", "glStencilMask", "glStencilMaskSeparate", "glStencilOp", "glStencilOpSeparate",
            "glTexImage2D", "glTexParameterf", "glTexParameterfv", "glTexParameteri", "glTexParameteriv",
            "glTexSubImage2D", "glUniform1f", "glUniform1fv", "glUniform1i", "glUniform1iv", "glUniform2f",
            "glUniform2fv", "glUniform2i", "glUniform2iv", "glUniform3f", "glUniform3fv", "glUniform3i",
            "glUniform3iv", "glUniform4f", "glUniform4fv", "glUniform4i", "glUniform4iv", "glUniformMatrix2fv",
            "glUniformMatrix3fv", "glUniformMatrix4fv", "glUseProgram", "glValidateProgram", "glVertexAttrib1f",
            "glVertexAttrib1fv", "glVertexAttrib2f", "glVertexAttrib2fv", "glVertexAttrib3f", "glVertexAttrib3fv",
            "glVertexAttrib4f", "glVertexAttrib4fv", "glVertexAttribPointer", "glViewport",
        ),
        "liblog.so" to setOf(
            "__android_log_assert", "__android_log_buf_print", "__android_log_buf_write", "__android_log_is_loggable",
            "__android_log_print", "__android_log_vprint", "__android_log_write",
        ),
        "libandroid.so" to setOf(
            "AAssetManager_fromJava", "AAssetManager_open", "AAssetManager_openDir", "AAsset_close",
            "AAsset_getBuffer", "AAsset_getLength", "AAsset_getLength64", "AAsset_getRemainingLength",
            "AAsset_getRemainingLength64", "AAsset_isAllocated", "AAsset_openFileDescriptor",
            "AAsset_openFileDescriptor64", "AAsset_read", "AAsset_seek", "AAsset_seek64", "AConfiguration_delete",
            "AConfiguration_fromAssetManager", "AConfiguration_getCountry", "AConfiguration_getLanguage",
            "AConfiguration_new", "AInputQueue_attachLooper", "AInputQueue_detachLooper", "AInputQueue_finishEvent",
            "AInputQueue_getEvent", "AInputQueue_preDispatchEvent", "ALooper_addFd", "ALooper_forThread",
            "ALooper_pollAll", "ALooper_pollOnce", "ALooper_prepare", "ALooper_removeFd", "ALooper_wake",
            "ANativeActivity_finish", "ANativeActivity_setWindowFlags", "ANativeWindow_acquire",
            "ANativeWindow_fromSurface", "ANativeWindow_getFormat", "ANativeWindow_getHeight",
            "ANativeWindow_getWidth", "ANativeWindow_lock", "ANativeWindow_release",
            "ANativeWindow_setBuffersGeometry", "ANativeWindow_unlockAndPost", "ASensorEventQueue_disableSensor",
            "ASensorEventQueue_enableSensor", "ASensorEventQueue_getEvents", "ASensorEventQueue_hasEvents",
            "ASensorEventQueue_setEventRate", "ASensorManager_createEventQueue", "ASensorManager_destroyEventQueue",
            "ASensorManager_getDefaultSensor", "ASensorManager_getInstance", "ASensorManager_getSensorList",
        ),
        "libOpenSLES.so" to setOf(
            "slCreateEngine",
        ),
        "libmediandk.so" to setOf(
            "AMediaCodec_configure", "AMediaCodec_createDecoderByType", "AMediaCodec_createInputBuffer",
            "AMediaCodec_createOutputBuffer", "AMediaCodec_delete", "AMediaCodec_dequeueInputBuffer",
            "AMediaCodec_dequeueOutputBuffer", "AMediaCodec_flush", "AMediaCodec_getInputBuffer",
            "AMediaCodec_getOutputBuffer", "AMediaCodec_getOutputFormat", "AMediaCodec_queueInputBuffer",
            "AMediaCodec_releaseInputBuffer", "AMediaCodec_releaseOutputBuffer", "AMediaCodec_start",
            "AMediaCodec_stop", "AMediaExtractor_advance", "AMediaExtractor_create", "AMediaExtractor_getSampleFlags",
            "AMediaExtractor_getSampleTime", "AMediaExtractor_getSampleTrackIndex", "AMediaExtractor_getTrackCount",
            "AMediaExtractor_getTrackFormat", "AMediaExtractor_readSampleData", "AMediaExtractor_release",
            "AMediaExtractor_seekTo", "AMediaExtractor_setDataSource", "AMediaFormat_create", "AMediaFormat_delete",
            "AMediaFormat_getInt32", "AMediaFormat_getString", "AMediaFormat_setBuffer", "AMediaFormat_setInt32",
            "AMediaFormat_setString",
        ),
        "libjnigraphics.so" to setOf(
            "AndroidBitmap_getInfo", "AndroidBitmap_lockPixels", "AndroidBitmap_unlockPixels",
        ),
        "libvulkan.so" to setOf(
            "vkAcquireNextImageKHR", "vkAllocateCommandBuffers", "vkAllocateMemory", "vkBeginCommandBuffer",
            "vkCmdBindPipeline", "vkCmdDraw", "vkCmdDrawIndexed", "vkCreateBuffer", "vkCreateCommandPool",
            "vkCreateDevice", "vkCreateFramebuffer", "vkCreateGraphicsPipelines", "vkCreateImage",
            "vkCreateImageView", "vkCreateInstance", "vkCreateRenderPass", "vkCreateShaderModule",
            "vkCreateSwapchainKHR", "vkDestroyBuffer", "vkDestroyCommandPool", "vkDestroyDevice",
            "vkDestroyFramebuffer", "vkDestroyImage", "vkDestroyImageView", "vkDestroyInstance", "vkDestroyPipeline",
            "vkDestroyRenderPass", "vkDestroyShaderModule", "vkDestroySwapchainKHR", "vkDeviceWaitIdle",
            "vkEndCommandBuffer", "vkEnumeratePhysicalDevices", "vkFreeCommandBuffers", "vkFreeMemory",
            "vkGetDeviceQueue", "vkGetPhysicalDeviceProperties", "vkGetSwapchainImagesKHR", "vkQueuePresentKHR",
            "vkQueueSubmit", "vkQueueWaitIdle",
        ),
        "libaaudio.so" to setOf(
            "AAudioStreamBuilder_delete", "AAudioStreamBuilder_openStream",
            "AAudioStreamBuilder_setBufferCapacityInFrames", "AAudioStreamBuilder_setChannelCount",
            "AAudioStreamBuilder_setDataCallback", "AAudioStreamBuilder_setDirection",
            "AAudioStreamBuilder_setErrorCallback", "AAudioStreamBuilder_setFormat",
            "AAudioStreamBuilder_setPerformanceMode", "AAudioStreamBuilder_setSampleRate",
            "AAudioStreamBuilder_setSharingMode", "AAudioStream_close", "AAudioStream_getChannelCount",
            "AAudioStream_getFormat", "AAudioStream_getSampleRate", "AAudioStream_getState",
            "AAudioStream_getXRunCount", "AAudioStream_read", "AAudioStream_requestFlush",
            "AAudioStream_requestPause", "AAudioStream_requestStart", "AAudioStream_requestStop",
            "AAudioStream_write", "AAudio_createStreamBuilder",
        ),
        "libc++_shared.so" to setOf(
            "_ZSt9terminatev", "_ZTVN10__cxxabiv117__class_type_infoE",
            "_ZTVN10__cxxabiv119__pointer_type_infoE", "_ZTVN10__cxxabiv120__si_class_type_infoE",
            "_ZTVN10__cxxabiv121__vmi_class_type_infoE", "_ZdaPv", "_ZdlPv", "_Znam", "_Znwm",
            "__cxa_allocate_exception", "__cxa_begin_catch", "__cxa_demangle", "__cxa_end_catch",
            "__cxa_free_exception", "__cxa_guard_abort", "__cxa_guard_acquire", "__cxa_guard_release",
            "__cxa_pure_virtual", "__cxa_rethrow", "__cxa_throw", "__dynamic_cast",
        )
    )

    /** These need source-level/ABI rewrites; none are direct native symbol aliases. */
    private val semanticTargets = mapOf(
        "UIApplication" to "android.app.Application + Activity lifecycle",
        "UIViewController" to "android.app.Activity or androidx.fragment.app.Fragment",
        "UIView" to "android.view.View",
        "UIWindow" to "android.view.Window",
        "UILabel" to "android.widget.TextView",
        "UIButton" to "android.widget.Button",
        "UIImage" to "android.graphics.Bitmap or android.graphics.drawable.Drawable",
        "UIImageView" to "android.widget.ImageView",
        "UIScrollView" to "android.widget.ScrollView",
        "UITableView" to "RecyclerView + LayoutManager + Adapter",
        "UICollectionView" to "RecyclerView + LayoutManager + Adapter",
        "UITextField" to "android.widget.EditText",
        "UITextView" to "android.widget.EditText or android.widget.TextView",
        "UIScreen" to "android.util.DisplayMetrics + WindowManager",
        "UIColor" to "android.graphics.Color",
        "UIFont" to "android.graphics.Typeface",
        "NSString" to "java.lang.String",
        "NSArray" to "java.util.List",
        "NSDictionary" to "java.util.Map",
        "NSData" to "byte[]",
        "NSBundle" to "Android assets/resources + package metadata",
        "NSFileManager" to "java.io.File + Android scoped-storage APIs",
        "NSUserDefaults" to "android.content.SharedPreferences or DataStore",
        "NSNotificationCenter" to "Lifecycle-aware callbacks or Android broadcasts",
        "NSTimer" to "Handler, ScheduledExecutorService, or Choreographer",
        "NSURLSession" to "HttpURLConnection or an approved Android HTTP client",
        "NSJSONSerialization" to "org.json or kotlinx.serialization",
        "AVAudioPlayer" to "android.media.MediaPlayer or SoundPool",
        "AVAudioSession" to "android.media.AudioManager + AudioAttributes",
        "CADisplayLink" to "C callback bridge driven by Android Choreographer (not the Objective-C CADisplayLink ABI)",
        "CAAnimation" to "android.animation.Animator",
        "SKScene" to "custom SurfaceView/Canvas renderer (game-loop rewrite required)",
    )

    /**
     * Real, host-tested implementation bodies exported by libioscompat.so.
     *
     * The four time shims live in native/src/apple_time_compat.cpp; other C,
     * POSIX and CoreFoundation entries come from RADEK_IOS_SHIM_TABLE in
     * native/include/radek_ios_shims.h, which is also expanded by
     * native/src/ioscompat_registry.cpp and native/src/jni.cpp. A dlsym hit here
     * proves an export exists; it does not rewrite an IPA callsite.
     */
    private val implementedApiReplacements = mapOf(
        "_CFAbsoluteTimeGetCurrent" to "CFAbsoluteTimeGetCurrent",
        "_CACurrentMediaTime" to "CACurrentMediaTime",
        "_mach_absolute_time" to "mach_absolute_time",
        "_mach_timebase_info" to "mach_timebase_info",
        "_CFAllocatorGetDefault" to "radek_compat_CFAllocatorGetDefault",
        "_CFRetain" to "radek_compat_CFRetain",
        "_CFRelease" to "radek_compat_CFRelease",
        "_CFGetRetainCount" to "radek_compat_CFGetRetainCount",
        "_CFStringCreateWithCString" to "radek_compat_CFStringCreateWithCString",
        "_CFStringGetLength" to "radek_compat_CFStringGetLength",
        "_CFStringGetCString" to "radek_compat_CFStringGetCString",
        "_CFStringGetCStringPtr" to "radek_compat_CFStringGetCStringPtr",
        "_CFStringGetMaximumSizeForEncoding" to "radek_compat_CFStringGetMaximumSizeForEncoding",
        "_CFStringCompare" to "radek_compat_CFStringCompare",
        "_CFStringGetSystemEncoding" to "radek_compat_CFStringGetSystemEncoding",
        "_CFDataCreate" to "radek_compat_CFDataCreate",
        "_CFDataGetBytePtr" to "radek_compat_CFDataGetBytePtr",
        "_CFDataGetLength" to "radek_compat_CFDataGetLength",
        "_CFArrayCreateMutable" to "radek_compat_CFArrayCreateMutable",
        "_CFArrayAppendValue" to "radek_compat_CFArrayAppendValue",
        "_CFArrayGetCount" to "radek_compat_CFArrayGetCount",
        "_CFArrayGetValueAtIndex" to "radek_compat_CFArrayGetValueAtIndex",
        "_CFDictionaryCreateMutable" to "radek_compat_CFDictionaryCreateMutable",
        "_CFDictionarySetValue" to "radek_compat_CFDictionarySetValue",
        "_CFDictionaryGetValue" to "radek_compat_CFDictionaryGetValue",
        "_CFDictionaryGetCount" to "radek_compat_CFDictionaryGetCount",
        "_CFNumberCreate" to "radek_compat_CFNumberCreate",
        "_CFNumberGetValue" to "radek_compat_CFNumberGetValue",
        "_CFDateCreate" to "radek_compat_CFDateCreate",
        "_CFDateGetAbsoluteTime" to "radek_compat_CFDateGetAbsoluteTime",
        "_CFDateGetTimeIntervalSinceDate" to "radek_compat_CFDateGetTimeIntervalSinceDate",
        "_CFAbsoluteTimeGetGregorianDate" to "radek_compat_CFAbsoluteTimeGetGregorianDate",
        "_CFRunLoopGetCurrent" to "radek_compat_CFRunLoopGetCurrent",
        "_CFRunLoopGetMain" to "radek_compat_CFRunLoopGetMain",
        "_CFRunLoopRun" to "radek_compat_CFRunLoopRun",
        "_CFRunLoopRunInMode" to "radek_compat_CFRunLoopRunInMode",
        "_CFRunLoopStop" to "radek_compat_CFRunLoopStop",
        "_CFRunLoopWakeUp" to "radek_compat_CFRunLoopWakeUp",
        "_malloc" to "radek_compat_malloc",
        "_calloc" to "radek_compat_calloc",
        "_realloc" to "radek_compat_realloc",
        "_free" to "radek_compat_free",
        "_memcpy" to "radek_compat_memcpy",
        "_memmove" to "radek_compat_memmove",
        "_memset" to "radek_compat_memset",
        "_memcmp" to "radek_compat_memcmp",
        "_memchr" to "radek_compat_memchr",
        "_strlen" to "radek_compat_strlen",
        "_strcpy" to "radek_compat_strcpy",
        "_strncpy" to "radek_compat_strncpy",
        "_strlcpy" to "radek_compat_strlcpy",
        "_strlcat" to "radek_compat_strlcat",
        "_strcmp" to "radek_compat_strcmp",
        "_strncmp" to "radek_compat_strncmp",
        "_strdup" to "radek_compat_strdup",
        "_strchr" to "radek_compat_strchr",
        "_strrchr" to "radek_compat_strrchr",
        "_strstr" to "radek_compat_strstr",
        "_strtol" to "radek_compat_strtol",
        "_strtod" to "radek_compat_strtod",
        "_atoi" to "radek_compat_atoi",
        "_atof" to "radek_compat_atof",
        "_strerror" to "radek_compat_strerror",
        "_snprintf" to "radek_compat_snprintf",
        "_vsnprintf" to "radek_compat_vsnprintf",
        "_fopen" to "radek_compat_fopen",
        "_fclose" to "radek_compat_fclose",
        "_fread" to "radek_compat_fread",
        "_fwrite" to "radek_compat_fwrite",
        "_fputs" to "radek_compat_fputs",
        "_fgets" to "radek_compat_fgets",
        "_fflush" to "radek_compat_fflush",
        "_fprintf" to "radek_compat_fprintf",
        "_printf" to "radek_compat_printf",
        "_puts" to "radek_compat_puts",
        "_remove" to "radek_compat_remove",
        "_feof" to "radek_compat_feof",
        "_ftell" to "radek_compat_ftell",
        "_fseek" to "radek_compat_fseek",
        "_time" to "radek_compat_time",
        "_gettimeofday" to "radek_compat_gettimeofday",
        "_clock_gettime" to "radek_compat_clock_gettime",
        "_nanosleep" to "radek_compat_nanosleep",
        "_localtime_r" to "radek_compat_localtime_r",
        "_gmtime_r" to "radek_compat_gmtime_r",
        "_mktime" to "radek_compat_mktime",
        "_getenv" to "radek_compat_getenv",
        "_setenv" to "radek_compat_setenv",
        "_unsetenv" to "radek_compat_unsetenv",
        "_getpid" to "radek_compat_getpid",
        "_qsort" to "radek_compat_qsort",
        "_bsearch" to "radek_compat_bsearch",
        "_abs" to "radek_compat_abs",
        "_labs" to "radek_compat_labs",
        "_rand" to "radek_compat_rand",
        "_srand" to "radek_compat_srand",
        "_sqrt" to "radek_compat_sqrt",
        "_fabs" to "radek_compat_fabs",
        "_floor" to "radek_compat_floor",
        "_ceil" to "radek_compat_ceil",
        "_pow" to "radek_compat_pow",
        "_sin" to "radek_compat_sin",
        "_cos" to "radek_compat_cos",
        "_tan" to "radek_compat_tan",
        "_atan2" to "radek_compat_atan2",
        "_fmod" to "radek_compat_fmod",
        "_pthread_mutex_init" to "radek_compat_pthread_mutex_init",
        "_pthread_mutex_lock" to "radek_compat_pthread_mutex_lock",
        "_pthread_mutex_unlock" to "radek_compat_pthread_mutex_unlock",
        "_pthread_mutex_destroy" to "radek_compat_pthread_mutex_destroy",
        "_pthread_cond_init" to "radek_compat_pthread_cond_init",
        "_pthread_cond_wait" to "radek_compat_pthread_cond_wait",
        "_pthread_cond_signal" to "radek_compat_pthread_cond_signal",
        "_pthread_cond_broadcast" to "radek_compat_pthread_cond_broadcast",
        "_pthread_cond_destroy" to "radek_compat_pthread_cond_destroy",
        "_pthread_self" to "radek_compat_pthread_self",
        "___CFConstantStringClassReference" to "radek_compat_CFConstantStringClassReference",
        "_kCFAllocatorDefault" to "radek_compat_CFAllocatorDefault",
        "_kCFBooleanTrue" to "radek_compat_CFBooleanTrue",
        "_kCFBooleanFalse" to "radek_compat_CFBooleanFalse",
        "_kCFTypeArrayCallBacks" to "radek_compat_CFTypeArrayCallBacks",
        "_kCFTypeDictionaryKeyCallBacks" to "radek_compat_CFTypeDictionaryKeyCallBacks",
        "_kCFTypeDictionaryValueCallBacks" to "radek_compat_CFTypeDictionaryValueCallBacks",
        "_kCFRunLoopDefaultMode" to "radek_compat_CFRunLoopDefaultMode",
        "_kCFRunLoopCommonModes" to "radek_compat_CFRunLoopCommonModes",
        "_CFBundleGetMainBundle" to "radek_compat_CFBundleGetMainBundle",
        "_CFBundleCopyBundleURL" to "radek_compat_CFBundleCopyBundleURL",
        "_CFBundleCopyResourcesDirectoryURL" to "radek_compat_CFBundleCopyResourcesDirectoryURL",
        "_CFBundleCopyResourceURL" to "radek_compat_CFBundleCopyResourceURL",
        "_CFBundleGetIdentifier" to "radek_compat_CFBundleGetIdentifier",
        "_CFBundleGetValueForInfoDictionaryKey" to "radek_compat_CFBundleGetValueForInfoDictionaryKey",
        "_CFURLCreateWithFileSystemPath" to "radek_compat_CFURLCreateWithFileSystemPath",
        "_CFURLCreateFromFileSystemRepresentation" to "radek_compat_CFURLCreateFromFileSystemRepresentation",
        "_CFURLGetFileSystemRepresentation" to "radek_compat_CFURLGetFileSystemRepresentation",
        "_CFURLCopyFileSystemPath" to "radek_compat_CFURLCopyFileSystemPath",
        "_CFStringCreateWithBytes" to "radek_compat_CFStringCreateWithBytes",
        "_CFStringCreateMutable" to "radek_compat_CFStringCreateMutable",
        "_CFStringAppendCString" to "radek_compat_CFStringAppendCString",
        "_CFStringHasPrefix" to "radek_compat_CFStringHasPrefix",
        "_CFStringHasSuffix" to "radek_compat_CFStringHasSuffix",
        "_CFStringGetIntValue" to "radek_compat_CFStringGetIntValue",
        "_CFStringGetDoubleValue" to "radek_compat_CFStringGetDoubleValue",
        "_CFArrayCreate" to "radek_compat_CFArrayCreate",
        "_CFArrayRemoveValueAtIndex" to "radek_compat_CFArrayRemoveValueAtIndex",
        "_CFArrayRemoveAllValues" to "radek_compat_CFArrayRemoveAllValues",
        "_CFDictionaryCreate" to "radek_compat_CFDictionaryCreate",
        "_CFDictionaryRemoveValue" to "radek_compat_CFDictionaryRemoveValue",
        "_CFDictionaryRemoveAllValues" to "radek_compat_CFDictionaryRemoveAllValues",
        "_CFDictionaryContainsKey" to "radek_compat_CFDictionaryContainsKey",
        "_CFDataCreateMutable" to "radek_compat_CFDataCreateMutable",
        "_CFDataAppendBytes" to "radek_compat_CFDataAppendBytes",
        "_CFDataGetMutableBytePtr" to "radek_compat_CFDataGetMutableBytePtr",
        "_CFDataGetBytes" to "radek_compat_CFDataGetBytes",
        "_CFBooleanGetValue" to "radek_compat_CFBooleanGetValue",
        "_CFEqual" to "radek_compat_CFEqual",
        "_CFHash" to "radek_compat_CFHash",
        "_CFGetTypeID" to "radek_compat_CFGetTypeID",
        "_CFPreferencesCopyAppValue" to "radek_compat_CFPreferencesCopyAppValue",
        "_CFPreferencesSetAppValue" to "radek_compat_CFPreferencesSetAppValue",
        "_CFPreferencesAppSynchronize" to "radek_compat_CFPreferencesAppSynchronize",
        "_CFUUIDCreate" to "radek_compat_CFUUIDCreate",
        "_CFUUIDCreateString" to "radek_compat_CFUUIDCreateString",
        "_CFLocaleCopyCurrent" to "radek_compat_CFLocaleCopyCurrent",
        "_CFLocaleCopyPreferredLanguages" to "radek_compat_CFLocaleCopyPreferredLanguages",
        "_CFLocaleGetIdentifier" to "radek_compat_CFLocaleGetIdentifier",
        "_CFTimeZoneCopySystem" to "radek_compat_CFTimeZoneCopySystem",
        "_AudioSessionInitialize" to "radek_compat_AudioSessionInitialize",
        "_AudioSessionSetActive" to "radek_compat_AudioSessionSetActive",
        "_NSSearchPathForDirectoriesInDomains" to "radek_compat_NSSearchPathForDirectoriesInDomains",
        "_OBJC_CLASS_\$_CAEAGLLayer" to "radek_compat_OBJC_CLASS___CAEAGLLayer",
        "_OBJC_CLASS_\$_EAGLContext" to "radek_compat_OBJC_CLASS___EAGLContext",
        "_OBJC_CLASS_\$_NSAutoreleasePool" to "radek_compat_OBJC_CLASS___NSAutoreleasePool",
        "_OBJC_CLASS_\$_NSBundle" to "radek_compat_OBJC_CLASS___NSBundle",
        "_OBJC_CLASS_\$_NSDictionary" to "radek_compat_OBJC_CLASS___NSDictionary",
        "_OBJC_CLASS_\$_NSNumber" to "radek_compat_OBJC_CLASS___NSNumber",
        "_OBJC_CLASS_\$_NSObject" to "radek_compat_OBJC_CLASS___NSObject",
        "_OBJC_CLASS_\$_NSString" to "radek_compat_OBJC_CLASS___NSString",
        "_OBJC_CLASS_\$_NSThread" to "radek_compat_OBJC_CLASS___NSThread",
        "_OBJC_CLASS_\$_NSURL" to "radek_compat_OBJC_CLASS___NSURL",
        "_OBJC_CLASS_\$_UIAccelerometer" to "radek_compat_OBJC_CLASS___UIAccelerometer",
        "_OBJC_CLASS_\$_UIApplication" to "radek_compat_OBJC_CLASS___UIApplication",
        "_OBJC_CLASS_\$_UIScreen" to "radek_compat_OBJC_CLASS___UIScreen",
        "_OBJC_CLASS_\$_UIView" to "radek_compat_OBJC_CLASS___UIView",
        "_OBJC_CLASS_\$_UIWindow" to "radek_compat_OBJC_CLASS___UIWindow",
        "_OBJC_METACLASS_\$_NSObject" to "radek_compat_OBJC_METACLASS___NSObject",
        "_OBJC_METACLASS_\$_UIView" to "radek_compat_OBJC_METACLASS___UIView",
        "_UIApplicationMain" to "radek_compat_UIApplicationMain",
        "__DefaultRuneLocale" to "radek_compat__DefaultRuneLocale",
        "__Unwind_SjLj_Register" to "radek_compat__Unwind_SjLj_Register",
        "__Unwind_SjLj_Resume" to "radek_compat__Unwind_SjLj_Resume",
        "__Unwind_SjLj_Unregister" to "radek_compat__Unwind_SjLj_Unregister",
        "__ZSt9terminatev" to "radek_compat__ZSt9terminatev",
        "__ZTVN10__cxxabiv117__class_type_infoE" to "radek_compat__ZTVN10__cxxabiv117__class_type_infoE",
        "__ZTVN10__cxxabiv119__pointer_type_infoE" to "radek_compat__ZTVN10__cxxabiv119__pointer_type_infoE",
        "__ZTVN10__cxxabiv120__si_class_type_infoE" to "radek_compat__ZTVN10__cxxabiv120__si_class_type_infoE",
        "__ZTVN10__cxxabiv121__vmi_class_type_infoE" to "radek_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE",
        "__ZdaPv" to "radek_compat__ZdaPv",
        "__ZdlPv" to "radek_compat__ZdlPv",
        "__Znam" to "radek_compat__Znam",
        "__Znwm" to "radek_compat__Znwm",
        "___cxa_allocate_exception" to "radek_compat___cxa_allocate_exception",
        "___cxa_atexit" to "radek_compat___cxa_atexit",
        "___cxa_begin_catch" to "radek_compat___cxa_begin_catch",
        "___cxa_end_catch" to "radek_compat___cxa_end_catch",
        "___cxa_pure_virtual" to "radek_compat___cxa_pure_virtual",
        "___cxa_throw" to "radek_compat___cxa_throw",
        "___divdi3" to "radek_compat___divdi3",
        "___divsi3" to "radek_compat___divsi3",
        "___error" to "radek_compat___error",
        "___fixdfdi" to "radek_compat___fixdfdi",
        "___floatdidf" to "radek_compat___floatdidf",
        "___floatdisf" to "radek_compat___floatdisf",
        "___gxx_personality_sj0" to "radek_compat___gxx_personality_sj0",
        "___maskrune" to "radek_compat___maskrune",
        "___moddi3" to "radek_compat___moddi3",
        "___modsi3" to "radek_compat___modsi3",
        "___stderrp" to "radek_compat___stderrp",
        "___stdinp" to "radek_compat___stdinp",
        "___stdoutp" to "radek_compat___stdoutp",
        "___tolower" to "radek_compat___tolower",
        "___toupper" to "radek_compat___toupper",
        "___udivsi3" to "radek_compat___udivsi3",
        "___umodsi3" to "radek_compat___umodsi3",
        "__objc_empty_cache" to "radek_compat__objc_empty_cache",
        "__objc_empty_vtable" to "radek_compat__objc_empty_vtable",
        "_abort" to "radek_compat_abort",
        "_acosf" to "radek_compat_acosf",
        "_alBufferData" to "radek_compat_alBufferData",
        "_alDeleteBuffers" to "radek_compat_alDeleteBuffers",
        "_alDeleteSources" to "radek_compat_alDeleteSources",
        "_alGenBuffers" to "radek_compat_alGenBuffers",
        "_alGenSources" to "radek_compat_alGenSources",
        "_alGetSourcef" to "radek_compat_alGetSourcef",
        "_alGetSourcei" to "radek_compat_alGetSourcei",
        "_alSource3f" to "radek_compat_alSource3f",
        "_alSourcePlay" to "radek_compat_alSourcePlay",
        "_alSourceQueueBuffers" to "radek_compat_alSourceQueueBuffers",
        "_alSourceStop" to "radek_compat_alSourceStop",
        "_alSourceUnqueueBuffers" to "radek_compat_alSourceUnqueueBuffers",
        "_alSourcef" to "radek_compat_alSourcef",
        "_alSourcei" to "radek_compat_alSourcei",
        "_alcCloseDevice" to "radek_compat_alcCloseDevice",
        "_alcCreateContext" to "radek_compat_alcCreateContext",
        "_alcDestroyContext" to "radek_compat_alcDestroyContext",
        "_alcMakeContextCurrent" to "radek_compat_alcMakeContextCurrent",
        "_alcOpenDevice" to "radek_compat_alcOpenDevice",
        "_asinf" to "radek_compat_asinf",
        "_atan2f" to "radek_compat_atan2f",
        "_atanf" to "radek_compat_atanf",
        "_ceilf" to "radek_compat_ceilf",
        "_clearerr" to "radek_compat_clearerr",
        "_clock" to "radek_compat_clock",
        "_close" to "radek_compat_close",
        "_cosf" to "radek_compat_cosf",
        "_coshf" to "radek_compat_coshf",
        "_difftime" to "radek_compat_difftime",
        "_exit" to "radek_compat_exit",
        "_expf" to "radek_compat_expf",
        "_fcntl" to "radek_compat_fcntl",
        "_ferror" to "radek_compat_ferror",
        "_floorf" to "radek_compat_floorf",
        "_fputc" to "radek_compat_fputc",
        "_freopen" to "radek_compat_freopen",
        "_frexp" to "radek_compat_frexp",
        "_fscanf" to "radek_compat_fscanf",
        "_getc" to "radek_compat_getc",
        "_glActiveTexture" to "radek_compat_glActiveTexture",
        "_glBindBuffer" to "radek_compat_glBindBuffer",
        "_glBindFramebufferOES" to "radek_compat_glBindFramebufferOES",
        "_glBindRenderbufferOES" to "radek_compat_glBindRenderbufferOES",
        "_glBindTexture" to "radek_compat_glBindTexture",
        "_glBlendFunc" to "radek_compat_glBlendFunc",
        "_glBufferData" to "radek_compat_glBufferData",
        "_glCheckFramebufferStatusOES" to "radek_compat_glCheckFramebufferStatusOES",
        "_glClear" to "radek_compat_glClear",
        "_glClearColor" to "radek_compat_glClearColor",
        "_glClientActiveTexture" to "radek_compat_glClientActiveTexture",
        "_glColor4f" to "radek_compat_glColor4f",
        "_glColorPointer" to "radek_compat_glColorPointer",
        "_glCompressedTexImage2D" to "radek_compat_glCompressedTexImage2D",
        "_glDeleteBuffers" to "radek_compat_glDeleteBuffers",
        "_glDeleteFramebuffersOES" to "radek_compat_glDeleteFramebuffersOES",
        "_glDeleteRenderbuffersOES" to "radek_compat_glDeleteRenderbuffersOES",
        "_glDeleteTextures" to "radek_compat_glDeleteTextures",
        "_glDepthFunc" to "radek_compat_glDepthFunc",
        "_glDepthMask" to "radek_compat_glDepthMask",
        "_glDisable" to "radek_compat_glDisable",
        "_glDisableClientState" to "radek_compat_glDisableClientState",
        "_glDrawArrays" to "radek_compat_glDrawArrays",
        "_glDrawElements" to "radek_compat_glDrawElements",
        "_glEnable" to "radek_compat_glEnable",
        "_glEnableClientState" to "radek_compat_glEnableClientState",
        "_glFramebufferRenderbufferOES" to "radek_compat_glFramebufferRenderbufferOES",
        "_glFramebufferTexture2DOES" to "radek_compat_glFramebufferTexture2DOES",
        "_glFrontFace" to "radek_compat_glFrontFace",
        "_glGenBuffers" to "radek_compat_glGenBuffers",
        "_glGenFramebuffersOES" to "radek_compat_glGenFramebuffersOES",
        "_glGenRenderbuffersOES" to "radek_compat_glGenRenderbuffersOES",
        "_glGenTextures" to "radek_compat_glGenTextures",
        "_glGetIntegerv" to "radek_compat_glGetIntegerv",
        "_glGetRenderbufferParameterivOES" to "radek_compat_glGetRenderbufferParameterivOES",
        "_glLightfv" to "radek_compat_glLightfv",
        "_glLineWidth" to "radek_compat_glLineWidth",
        "_glLoadMatrixf" to "radek_compat_glLoadMatrixf",
        "_glMaterialfv" to "radek_compat_glMaterialfv",
        "_glMatrixMode" to "radek_compat_glMatrixMode",
        "_glNormalPointer" to "radek_compat_glNormalPointer",
        "_glPixelStorei" to "radek_compat_glPixelStorei",
        "_glRenderbufferStorageOES" to "radek_compat_glRenderbufferStorageOES",
        "_glScissor" to "radek_compat_glScissor",
        "_glTexCoordPointer" to "radek_compat_glTexCoordPointer",
        "_glTexEnvi" to "radek_compat_glTexEnvi",
        "_glTexImage2D" to "radek_compat_glTexImage2D",
        "_glTexParameteri" to "radek_compat_glTexParameteri",
        "_glTexSubImage2D" to "radek_compat_glTexSubImage2D",
        "_glVertexPointer" to "radek_compat_glVertexPointer",
        "_glViewport" to "radek_compat_glViewport",
        "_gmtime" to "radek_compat_gmtime",
        "_kEAGLColorFormatRGB565" to "radek_compat_kEAGLColorFormatRGB565",
        "_kEAGLColorFormatRGBA8" to "radek_compat_kEAGLColorFormatRGBA8",
        "_kEAGLDrawablePropertyColorFormat" to "radek_compat_kEAGLDrawablePropertyColorFormat",
        "_kEAGLDrawablePropertyRetainedBacking" to "radek_compat_kEAGLDrawablePropertyRetainedBacking",
        "_ldexp" to "radek_compat_ldexp",
        "_localeconv" to "radek_compat_localeconv",
        "_localtime" to "radek_compat_localtime",
        "_log10f" to "radek_compat_log10f",
        "_logf" to "radek_compat_logf",
        "_longjmp" to "radek_compat_longjmp",
        "_lseek" to "radek_compat_lseek",
        "_modf" to "radek_compat_modf",
        "_objc_enumerationMutation" to "radek_compat_objc_enumerationMutation",
        "_objc_msgSend" to "radek_compat_objc_msgSend",
        "_objc_msgSendSuper2" to "radek_compat_objc_msgSendSuper2",
        "_objc_msgSend_stret" to "radek_compat_objc_msgSend_stret",
        "_objc_setProperty" to "radek_compat_objc_setProperty",
        "_pthread_create" to "radek_compat_pthread_create",
        "_pthread_exit" to "radek_compat_pthread_exit",
        "_pthread_getschedparam" to "radek_compat_pthread_getschedparam",
        "_pthread_join" to "radek_compat_pthread_join",
        "_pthread_mutex_trylock" to "radek_compat_pthread_mutex_trylock",
        "_pthread_mutexattr_destroy" to "radek_compat_pthread_mutexattr_destroy",
        "_pthread_mutexattr_init" to "radek_compat_pthread_mutexattr_init",
        "_pthread_mutexattr_settype" to "radek_compat_pthread_mutexattr_settype",
        "_pthread_setschedparam" to "radek_compat_pthread_setschedparam",
        "_read" to "radek_compat_read",
        "_rename" to "radek_compat_rename",
        "_sched_yield" to "radek_compat_sched_yield",
        "_select" to "radek_compat_select",
        "_setjmp" to "radek_compat_setjmp",
        "_setlocale" to "radek_compat_setlocale",
        "_setvbuf" to "radek_compat_setvbuf",
        "_sinf" to "radek_compat_sinf",
        "_sinhf" to "radek_compat_sinhf",
        "_sprintf" to "radek_compat_sprintf",
        "_strcasecmp" to "radek_compat_strcasecmp",
        "_strcat" to "radek_compat_strcat",
        "_strcoll" to "radek_compat_strcoll",
        "_strcspn" to "radek_compat_strcspn",
        "_strftime" to "radek_compat_strftime",
        "_strncat" to "radek_compat_strncat",
        "_strpbrk" to "radek_compat_strpbrk",
        "_strtok" to "radek_compat_strtok",
        "_strtoul" to "radek_compat_strtoul",
        "_system" to "radek_compat_system",
        "_tanf" to "radek_compat_tanf",
        "_tanhf" to "radek_compat_tanhf",
        "_tmpfile" to "radek_compat_tmpfile",
        "_tmpnam" to "radek_compat_tmpnam",
        "_ungetc" to "radek_compat_ungetc",
        "_usleep" to "radek_compat_usleep",
        "_vsprintf" to "radek_compat_vsprintf",
        "__exit" to "radek_compat__exit",
        "_atexit" to "radek_compat_atexit",
        "_sscanf" to "radek_compat_sscanf",
        "_putchar" to "radek_compat_putchar",
        "_getchar" to "radek_compat_getchar",
        "_fgetc" to "radek_compat_fgetc",
        "_putc" to "radek_compat_putc",
        "_rewind" to "radek_compat_rewind",
        "_fileno" to "radek_compat_fileno",
        "_fdopen" to "radek_compat_fdopen",
        "_perror" to "radek_compat_perror",
        "_tzset" to "radek_compat_tzset",
        "_sleep" to "radek_compat_sleep",
        "_open" to "radek_compat_open",
        "_write" to "radek_compat_write",
        "_unlink" to "radek_compat_unlink",
        "_mkdir" to "radek_compat_mkdir",
        "_rmdir" to "radek_compat_rmdir",
        "_access" to "radek_compat_access",
        "_getcwd" to "radek_compat_getcwd",
        "_chdir" to "radek_compat_chdir",
        "_stat" to "radek_compat_stat",
        "_fstat" to "radek_compat_fstat",
        "_lstat" to "radek_compat_lstat",
        "_opendir" to "radek_compat_opendir",
        "_readdir" to "radek_compat_readdir",
        "_closedir" to "radek_compat_closedir",
        "_mmap" to "radek_compat_mmap",
        "_munmap" to "radek_compat_munmap",
        "_mprotect" to "radek_compat_mprotect",
        "_poll" to "radek_compat_poll",
        "_pipe" to "radek_compat_pipe",
        "_dup" to "radek_compat_dup",
        "_dup2" to "radek_compat_dup2",
        "_fsync" to "radek_compat_fsync",
        "_ftruncate" to "radek_compat_ftruncate",
        "_truncate" to "radek_compat_truncate",
        "_chmod" to "radek_compat_chmod",
        "_umask" to "radek_compat_umask",
        "_getuid" to "radek_compat_getuid",
        "_geteuid" to "radek_compat_geteuid",
        "_getgid" to "radek_compat_getgid",
        "_getegid" to "radek_compat_getegid",
        "_getppid" to "radek_compat_getppid",
        "_sysconf" to "radek_compat_sysconf",
        "_sysctl" to "radek_compat_sysctl",
        "_sysctlbyname" to "radek_compat_sysctlbyname",
        "_getpagesize" to "radek_compat_getpagesize",
        "__setjmp" to "radek_compat__setjmp",
        "__longjmp" to "radek_compat__longjmp",
        "_sigaction" to "radek_compat_sigaction",
        "_signal" to "radek_compat_signal",
        "_raise" to "radek_compat_raise",
        "_kill" to "radek_compat_kill",
        "_tolower" to "radek_compat_tolower",
        "_toupper" to "radek_compat_toupper",
        "_isalpha" to "radek_compat_isalpha",
        "_isdigit" to "radek_compat_isdigit",
        "_isalnum" to "radek_compat_isalnum",
        "_isspace" to "radek_compat_isspace",
        "_isupper" to "radek_compat_isupper",
        "_islower" to "radek_compat_islower",
        "_isxdigit" to "radek_compat_isxdigit",
        "_strncasecmp" to "radek_compat_strncasecmp",
        "_strspn" to "radek_compat_strspn",
        "_strtok_r" to "radek_compat_strtok_r",
        "_strtoll" to "radek_compat_strtoll",
        "_strtoull" to "radek_compat_strtoull",
        "_strtof" to "radek_compat_strtof",
        "_atol" to "radek_compat_atol",
        "_atoll" to "radek_compat_atoll",
        "_llabs" to "radek_compat_llabs",
        "_bzero" to "radek_compat_bzero",
        "_bcopy" to "radek_compat_bcopy",
        "_bcmp" to "radek_compat_bcmp",
        "_acos" to "radek_compat_acos",
        "_asin" to "radek_compat_asin",
        "_atan" to "radek_compat_atan",
        "_cosh" to "radek_compat_cosh",
        "_sinh" to "radek_compat_sinh",
        "_tanh" to "radek_compat_tanh",
        "_exp" to "radek_compat_exp",
        "_log" to "radek_compat_log",
        "_log10" to "radek_compat_log10",
        "_log2" to "radek_compat_log2",
        "_hypot" to "radek_compat_hypot",
        "_hypotf" to "radek_compat_hypotf",
        "_cbrt" to "radek_compat_cbrt",
        "_round" to "radek_compat_round",
        "_roundf" to "radek_compat_roundf",
        "_trunc" to "radek_compat_trunc",
        "_truncf" to "radek_compat_truncf",
        "_lround" to "radek_compat_lround",
        "_lroundf" to "radek_compat_lroundf",
        "_frexpf" to "radek_compat_frexpf",
        "_ldexpf" to "radek_compat_ldexpf",
        "_log2f" to "radek_compat_log2f",
        "_modff" to "radek_compat_modff",
        "_powf" to "radek_compat_powf",
        "_sqrtf" to "radek_compat_sqrtf",
        "_fabsf" to "radek_compat_fabsf",
        "_fmodf" to "radek_compat_fmodf",
        "_pthread_detach" to "radek_compat_pthread_detach",
        "_pthread_equal" to "radek_compat_pthread_equal",
        "_pthread_once" to "radek_compat_pthread_once",
        "_pthread_cond_timedwait" to "radek_compat_pthread_cond_timedwait",
        "_pthread_key_create" to "radek_compat_pthread_key_create",
        "_pthread_key_delete" to "radek_compat_pthread_key_delete",
        "_pthread_setspecific" to "radek_compat_pthread_setspecific",
        "_pthread_getspecific" to "radek_compat_pthread_getspecific",
        "_pthread_rwlock_init" to "radek_compat_pthread_rwlock_init",
        "_pthread_rwlock_rdlock" to "radek_compat_pthread_rwlock_rdlock",
        "_pthread_rwlock_wrlock" to "radek_compat_pthread_rwlock_wrlock",
        "_pthread_rwlock_unlock" to "radek_compat_pthread_rwlock_unlock",
        "_pthread_rwlock_destroy" to "radek_compat_pthread_rwlock_destroy",
        "_sem_init" to "radek_compat_sem_init",
        "_sem_destroy" to "radek_compat_sem_destroy",
        "_sem_wait" to "radek_compat_sem_wait",
        "_sem_trywait" to "radek_compat_sem_trywait",
        "_sem_post" to "radek_compat_sem_post",
        "_dlopen" to "radek_compat_dlopen",
        "_dlsym" to "radek_compat_dlsym",
        "_dlclose" to "radek_compat_dlclose",
        "_dlerror" to "radek_compat_dlerror",
        "_socket" to "radek_compat_socket",
        "_connect" to "radek_compat_connect",
        "_bind" to "radek_compat_bind",
        "_listen" to "radek_compat_listen",
        "_accept" to "radek_compat_accept",
        "_send" to "radek_compat_send",
        "_sendto" to "radek_compat_sendto",
        "_recv" to "radek_compat_recv",
        "_recvfrom" to "radek_compat_recvfrom",
        "_setsockopt" to "radek_compat_setsockopt",
        "_getsockopt" to "radek_compat_getsockopt",
        "_getsockname" to "radek_compat_getsockname",
        "_getpeername" to "radek_compat_getpeername",
        "_shutdown" to "radek_compat_shutdown",
        "_getaddrinfo" to "radek_compat_getaddrinfo",
        "_freeaddrinfo" to "radek_compat_freeaddrinfo",
        "_gethostbyname" to "radek_compat_gethostbyname",
        "_inet_ntop" to "radek_compat_inet_ntop",
        "_inet_pton" to "radek_compat_inet_pton",
        "_inet_addr" to "radek_compat_inet_addr",
        "_inet_ntoa" to "radek_compat_inet_ntoa",
        "_htons" to "radek_compat_htons",
        "_htonl" to "radek_compat_htonl",
        "_ntohs" to "radek_compat_ntohs",
        "_ntohl" to "radek_compat_ntohl",
        "_crc32" to "radek_compat_crc32",
        "_adler32" to "radek_compat_adler32",
        "_compress" to "radek_compat_compress",
        "_compress2" to "radek_compat_compress2",
        "_uncompress" to "radek_compat_uncompress",
        "_deflateInit_" to "radek_compat_deflateInit_",
        "_deflateInit2_" to "radek_compat_deflateInit2_",
        "_deflate" to "radek_compat_deflate",
        "_deflateEnd" to "radek_compat_deflateEnd",
        "_deflateReset" to "radek_compat_deflateReset",
        "_inflateInit_" to "radek_compat_inflateInit_",
        "_inflateInit2_" to "radek_compat_inflateInit2_",
        "_inflate" to "radek_compat_inflate",
        "_inflateEnd" to "radek_compat_inflateEnd",
        "_inflateReset" to "radek_compat_inflateReset",
        "_gzopen" to "radek_compat_gzopen",
        "_gzread" to "radek_compat_gzread",
        "_gzwrite" to "radek_compat_gzwrite",
        "_gzclose" to "radek_compat_gzclose",
        "_alDistanceModel" to "radek_compat_alDistanceModel",
        "_alDopplerFactor" to "radek_compat_alDopplerFactor",
        "_alDopplerVelocity" to "radek_compat_alDopplerVelocity",
        "_alSpeedOfSound" to "radek_compat_alSpeedOfSound",
        "_alGetError" to "radek_compat_alGetError",
        "_alGetSource3f" to "radek_compat_alGetSource3f",
        "_alGetSourcefv" to "radek_compat_alGetSourcefv",
        "_alSourcefv" to "radek_compat_alSourcefv",
        "_alSourcePause" to "radek_compat_alSourcePause",
        "_alSourceRewind" to "radek_compat_alSourceRewind",
        "_alListener3f" to "radek_compat_alListener3f",
        "_alListenerf" to "radek_compat_alListenerf",
        "_alListenerfv" to "radek_compat_alListenerfv",
        "_alListeneri" to "radek_compat_alListeneri",
        "_alGetListenerf" to "radek_compat_alGetListenerf",
        "_alGetListener3f" to "radek_compat_alGetListener3f",
        "_alGetListenerfv" to "radek_compat_alGetListenerfv",
        "_alEnable" to "radek_compat_alEnable",
        "_alDisable" to "radek_compat_alDisable",
        "_alIsEnabled" to "radek_compat_alIsEnabled",
        "_alIsBuffer" to "radek_compat_alIsBuffer",
        "_alIsSource" to "radek_compat_alIsSource",
        "_alGetBoolean" to "radek_compat_alGetBoolean",
        "_alGetInteger" to "radek_compat_alGetInteger",
        "_alGetFloat" to "radek_compat_alGetFloat",
        "_alGetDouble" to "radek_compat_alGetDouble",
        "_alGetString" to "radek_compat_alGetString",
        "_alGetEnumValue" to "radek_compat_alGetEnumValue",
        "_alGetProcAddress" to "radek_compat_alGetProcAddress",
        "_alIsExtensionPresent" to "radek_compat_alIsExtensionPresent",
        "_alcGetContextsDevice" to "radek_compat_alcGetContextsDevice",
        "_alcGetCurrentContext" to "radek_compat_alcGetCurrentContext",
        "_alcProcessContext" to "radek_compat_alcProcessContext",
        "_alcSuspendContext" to "radek_compat_alcSuspendContext",
        "_alcGetError" to "radek_compat_alcGetError",
        "_alcGetIntegerv" to "radek_compat_alcGetIntegerv",
        "_alcGetString" to "radek_compat_alcGetString",
        "_alcIsExtensionPresent" to "radek_compat_alcIsExtensionPresent",
        "_alcGetProcAddress" to "radek_compat_alcGetProcAddress",
        "_AudioSessionSetActiveWithFlags" to "radek_compat_AudioSessionSetActiveWithFlags",
        "_AudioSessionGetProperty" to "radek_compat_AudioSessionGetProperty",
        "_AudioSessionSetProperty" to "radek_compat_AudioSessionSetProperty",
        "_AudioSessionGetPropertySize" to "radek_compat_AudioSessionGetPropertySize",
        "_AudioSessionAddPropertyListener" to "radek_compat_AudioSessionAddPropertyListener",
        "_AudioSessionRemovePropertyListenerWithUserData" to "radek_compat_AudioSessionRemovePropertyListenerWithUserData",
        "_AudioServicesPlaySystemSound" to "radek_compat_AudioServicesPlaySystemSound",
        "_AudioServicesPlayAlertSound" to "radek_compat_AudioServicesPlayAlertSound",
        "_AudioServicesCreateSystemSoundID" to "radek_compat_AudioServicesCreateSystemSoundID",
        "_AudioServicesDisposeSystemSoundID" to "radek_compat_AudioServicesDisposeSystemSoundID",
        "_AudioFileOpenURL" to "radek_compat_AudioFileOpenURL",
        "_AudioFileClose" to "radek_compat_AudioFileClose",
        "_AudioFileGetProperty" to "radek_compat_AudioFileGetProperty",
        "_AudioFileReadBytes" to "radek_compat_AudioFileReadBytes",
        "_AudioFileReadPackets" to "radek_compat_AudioFileReadPackets",
        "_ExtAudioFileOpenURL" to "radek_compat_ExtAudioFileOpenURL",
        "_ExtAudioFileDispose" to "radek_compat_ExtAudioFileDispose",
        "_ExtAudioFileGetProperty" to "radek_compat_ExtAudioFileGetProperty",
        "_ExtAudioFileSetProperty" to "radek_compat_ExtAudioFileSetProperty",
        "_ExtAudioFileRead" to "radek_compat_ExtAudioFileRead",
        "_ExtAudioFileSeek" to "radek_compat_ExtAudioFileSeek",
        "_AudioQueueNewOutput" to "radek_compat_AudioQueueNewOutput",
        "_AudioQueueAllocateBuffer" to "radek_compat_AudioQueueAllocateBuffer",
        "_AudioQueueFreeBuffer" to "radek_compat_AudioQueueFreeBuffer",
        "_AudioQueueEnqueueBuffer" to "radek_compat_AudioQueueEnqueueBuffer",
        "_AudioQueueStart" to "radek_compat_AudioQueueStart",
        "_AudioQueuePause" to "radek_compat_AudioQueuePause",
        "_AudioQueueStop" to "radek_compat_AudioQueueStop",
        "_AudioQueueDispose" to "radek_compat_AudioQueueDispose",
        "_AudioQueueSetParameter" to "radek_compat_AudioQueueSetParameter",
        "_AudioComponentFindNext" to "radek_compat_AudioComponentFindNext",
        "_AudioComponentInstanceNew" to "radek_compat_AudioComponentInstanceNew",
        "_AudioComponentInstanceDispose" to "radek_compat_AudioComponentInstanceDispose",
        "_AudioUnitInitialize" to "radek_compat_AudioUnitInitialize",
        "_AudioUnitUninitialize" to "radek_compat_AudioUnitUninitialize",
        "_AudioUnitSetProperty" to "radek_compat_AudioUnitSetProperty",
        "_AudioUnitGetProperty" to "radek_compat_AudioUnitGetProperty",
        "_AudioOutputUnitStart" to "radek_compat_AudioOutputUnitStart",
        "_AudioOutputUnitStop" to "radek_compat_AudioOutputUnitStop",
        "_AudioUnitRender" to "radek_compat_AudioUnitRender",
        "_glAlphaFunc" to "radek_compat_glAlphaFunc",
        "_glBindFramebuffer" to "radek_compat_glBindFramebuffer",
        "_glBindRenderbuffer" to "radek_compat_glBindRenderbuffer",
        "_glBlendEquation" to "radek_compat_glBlendEquation",
        "_glBlendEquationOES" to "radek_compat_glBlendEquationOES",
        "_glBlendFuncSeparate" to "radek_compat_glBlendFuncSeparate",
        "_glBufferSubData" to "radek_compat_glBufferSubData",
        "_glCheckFramebufferStatus" to "radek_compat_glCheckFramebufferStatus",
        "_glClearDepthf" to "radek_compat_glClearDepthf",
        "_glClearStencil" to "radek_compat_glClearStencil",
        "_glColor4ub" to "radek_compat_glColor4ub",
        "_glColorMask" to "radek_compat_glColorMask",
        "_glCompileShader" to "radek_compat_glCompileShader",
        "_glCopyTexImage2D" to "radek_compat_glCopyTexImage2D",
        "_glCopyTexSubImage2D" to "radek_compat_glCopyTexSubImage2D",
        "_glCreateProgram" to "radek_compat_glCreateProgram",
        "_glCreateShader" to "radek_compat_glCreateShader",
        "_glCullFace" to "radek_compat_glCullFace",
        "_glDeleteFramebuffers" to "radek_compat_glDeleteFramebuffers",
        "_glDeleteProgram" to "radek_compat_glDeleteProgram",
        "_glDeleteRenderbuffers" to "radek_compat_glDeleteRenderbuffers",
        "_glDeleteShader" to "radek_compat_glDeleteShader",
        "_glDepthRangef" to "radek_compat_glDepthRangef",
        "_glDisableVertexAttribArray" to "radek_compat_glDisableVertexAttribArray",
        "_glEnableVertexAttribArray" to "radek_compat_glEnableVertexAttribArray",
        "_glFinish" to "radek_compat_glFinish",
        "_glFlush" to "radek_compat_glFlush",
        "_glFogf" to "radek_compat_glFogf",
        "_glFogfv" to "radek_compat_glFogfv",
        "_glFramebufferRenderbuffer" to "radek_compat_glFramebufferRenderbuffer",
        "_glFramebufferTexture2D" to "radek_compat_glFramebufferTexture2D",
        "_glFrustumf" to "radek_compat_glFrustumf",
        "_glGenFramebuffers" to "radek_compat_glGenFramebuffers",
        "_glGenRenderbuffers" to "radek_compat_glGenRenderbuffers",
        "_glGenerateMipmap" to "radek_compat_glGenerateMipmap",
        "_glGenerateMipmapOES" to "radek_compat_glGenerateMipmapOES",
        "_glGetAttribLocation" to "radek_compat_glGetAttribLocation",
        "_glGetError" to "radek_compat_glGetError",
        "_glGetFloatv" to "radek_compat_glGetFloatv",
        "_glGetProgramInfoLog" to "radek_compat_glGetProgramInfoLog",
        "_glGetProgramiv" to "radek_compat_glGetProgramiv",
        "_glGetRenderbufferParameteriv" to "radek_compat_glGetRenderbufferParameteriv",
        "_glGetShaderInfoLog" to "radek_compat_glGetShaderInfoLog",
        "_glGetShaderiv" to "radek_compat_glGetShaderiv",
        "_glGetString" to "radek_compat_glGetString",
        "_glGetUniformLocation" to "radek_compat_glGetUniformLocation",
        "_glHint" to "radek_compat_glHint",
        "_glIsEnabled" to "radek_compat_glIsEnabled",
        "_glIsTexture" to "radek_compat_glIsTexture",
        "_glLightModelfv" to "radek_compat_glLightModelfv",
        "_glLinkProgram" to "radek_compat_glLinkProgram",
        "_glLoadIdentity" to "radek_compat_glLoadIdentity",
        "_glLogicOp" to "radek_compat_glLogicOp",
        "_glMaterialf" to "radek_compat_glMaterialf",
        "_glMultMatrixf" to "radek_compat_glMultMatrixf",
        "_glNormal3f" to "radek_compat_glNormal3f",
        "_glOrthof" to "radek_compat_glOrthof",
        "_glPointParameterf" to "radek_compat_glPointParameterf",
        "_glPointParameterfv" to "radek_compat_glPointParameterfv",
        "_glPointSize" to "radek_compat_glPointSize",
        "_glPolygonOffset" to "radek_compat_glPolygonOffset",
        "_glPopMatrix" to "radek_compat_glPopMatrix",
        "_glPushMatrix" to "radek_compat_glPushMatrix",
        "_glReadPixels" to "radek_compat_glReadPixels",
        "_glRenderbufferStorage" to "radek_compat_glRenderbufferStorage",
        "_glRotatef" to "radek_compat_glRotatef",
        "_glScalef" to "radek_compat_glScalef",
        "_glShadeModel" to "radek_compat_glShadeModel",
        "_glShaderSource" to "radek_compat_glShaderSource",
        "_glStencilFunc" to "radek_compat_glStencilFunc",
        "_glStencilMask" to "radek_compat_glStencilMask",
        "_glStencilOp" to "radek_compat_glStencilOp",
        "_glTexEnvf" to "radek_compat_glTexEnvf",
        "_glTexEnvfv" to "radek_compat_glTexEnvfv",
        "_glTexParameterf" to "radek_compat_glTexParameterf",
        "_glTexParameterfv" to "radek_compat_glTexParameterfv",
        "_glTranslatef" to "radek_compat_glTranslatef",
        "_glUniform1f" to "radek_compat_glUniform1f",
        "_glUniform1i" to "radek_compat_glUniform1i",
        "_glUniform2f" to "radek_compat_glUniform2f",
        "_glUniform3f" to "radek_compat_glUniform3f",
        "_glUniform4f" to "radek_compat_glUniform4f",
        "_glUniformMatrix4fv" to "radek_compat_glUniformMatrix4fv",
        "_glUseProgram" to "radek_compat_glUseProgram",
        "_glVertexAttribPointer" to "radek_compat_glVertexAttribPointer",
        "_eglGetDisplay" to "radek_compat_eglGetDisplay",
        "_eglInitialize" to "radek_compat_eglInitialize",
        "_eglChooseConfig" to "radek_compat_eglChooseConfig",
        "_eglCreateWindowSurface" to "radek_compat_eglCreateWindowSurface",
        "_eglCreateContext" to "radek_compat_eglCreateContext",
        "_eglMakeCurrent" to "radek_compat_eglMakeCurrent",
        "_eglSwapBuffers" to "radek_compat_eglSwapBuffers",
        "_eglDestroyContext" to "radek_compat_eglDestroyContext",
        "_eglDestroySurface" to "radek_compat_eglDestroySurface",
        "_eglTerminate" to "radek_compat_eglTerminate",
        "_eglGetError" to "radek_compat_eglGetError",
        "_eglGetProcAddress" to "radek_compat_eglGetProcAddress",
        "_CGColorSpaceCreateDeviceRGB" to "radek_compat_CGColorSpaceCreateDeviceRGB",
        "_CGColorSpaceCreateDeviceGray" to "radek_compat_CGColorSpaceCreateDeviceGray",
        "_CGColorSpaceRelease" to "radek_compat_CGColorSpaceRelease",
        "_CGColorSpaceRetain" to "radek_compat_CGColorSpaceRetain",
        "_CGBitmapContextCreate" to "radek_compat_CGBitmapContextCreate",
        "_CGBitmapContextGetData" to "radek_compat_CGBitmapContextGetData",
        "_CGBitmapContextGetWidth" to "radek_compat_CGBitmapContextGetWidth",
        "_CGBitmapContextGetHeight" to "radek_compat_CGBitmapContextGetHeight",
        "_CGBitmapContextGetBytesPerRow" to "radek_compat_CGBitmapContextGetBytesPerRow",
        "_CGBitmapContextCreateImage" to "radek_compat_CGBitmapContextCreateImage",
        "_CGContextRelease" to "radek_compat_CGContextRelease",
        "_CGContextRetain" to "radek_compat_CGContextRetain",
        "_CGContextClearRect" to "radek_compat_CGContextClearRect",
        "_CGContextFillRect" to "radek_compat_CGContextFillRect",
        "_CGContextDrawImage" to "radek_compat_CGContextDrawImage",
        "_CGContextTranslateCTM" to "radek_compat_CGContextTranslateCTM",
        "_CGContextScaleCTM" to "radek_compat_CGContextScaleCTM",
        "_CGContextRotateCTM" to "radek_compat_CGContextRotateCTM",
        "_CGContextSaveGState" to "radek_compat_CGContextSaveGState",
        "_CGContextRestoreGState" to "radek_compat_CGContextRestoreGState",
        "_CGContextSetRGBFillColor" to "radek_compat_CGContextSetRGBFillColor",
        "_CGContextSetAlpha" to "radek_compat_CGContextSetAlpha",
        "_CGImageGetWidth" to "radek_compat_CGImageGetWidth",
        "_CGImageGetHeight" to "radek_compat_CGImageGetHeight",
        "_CGImageGetBitsPerComponent" to "radek_compat_CGImageGetBitsPerComponent",
        "_CGImageGetBitsPerPixel" to "radek_compat_CGImageGetBitsPerPixel",
        "_CGImageGetBytesPerRow" to "radek_compat_CGImageGetBytesPerRow",
        "_CGImageGetAlphaInfo" to "radek_compat_CGImageGetAlphaInfo",
        "_CGImageGetDataProvider" to "radek_compat_CGImageGetDataProvider",
        "_CGImageGetColorSpace" to "radek_compat_CGImageGetColorSpace",
        "_CGImageRelease" to "radek_compat_CGImageRelease",
        "_CGImageRetain" to "radek_compat_CGImageRetain",
        "_CGDataProviderCopyData" to "radek_compat_CGDataProviderCopyData",
        "_CGDataProviderCreateWithData" to "radek_compat_CGDataProviderCreateWithData",
        "_CGDataProviderRelease" to "radek_compat_CGDataProviderRelease",
        "_CGDataProviderRetain" to "radek_compat_CGDataProviderRetain",
        "_CGAffineTransformMake" to "radek_compat_CGAffineTransformMake",
        "_CGAffineTransformMakeTranslation" to "radek_compat_CGAffineTransformMakeTranslation",
        "_CGAffineTransformMakeScale" to "radek_compat_CGAffineTransformMakeScale",
        "_CGAffineTransformMakeRotation" to "radek_compat_CGAffineTransformMakeRotation",
        "_CGAffineTransformTranslate" to "radek_compat_CGAffineTransformTranslate",
        "_CGAffineTransformScale" to "radek_compat_CGAffineTransformScale",
        "_CGAffineTransformRotate" to "radek_compat_CGAffineTransformRotate",
        "_CGAffineTransformConcat" to "radek_compat_CGAffineTransformConcat",
        "_objc_msgSendSuper" to "radek_compat_objc_msgSendSuper",
        "_objc_msgSendSuper_stret" to "radek_compat_objc_msgSendSuper_stret",
        "_objc_msgSendSuper2_stret" to "radek_compat_objc_msgSendSuper2_stret",
        "_objc_msgSend_fpret" to "radek_compat_objc_msgSend_fpret",
        "_objc_getClass" to "radek_compat_objc_getClass",
        "_objc_lookUpClass" to "radek_compat_objc_lookUpClass",
        "_objc_getMetaClass" to "radek_compat_objc_getMetaClass",
        "_objc_getProtocol" to "radek_compat_objc_getProtocol",
        "_objc_allocateClassPair" to "radek_compat_objc_allocateClassPair",
        "_objc_registerClassPair" to "radek_compat_objc_registerClassPair",
        "_objc_retain" to "radek_compat_objc_retain",
        "_objc_release" to "radek_compat_objc_release",
        "_objc_autorelease" to "radek_compat_objc_autorelease",
        "_objc_autoreleasePoolPush" to "radek_compat_objc_autoreleasePoolPush",
        "_objc_autoreleasePoolPop" to "radek_compat_objc_autoreleasePoolPop",
        "_objc_retainAutorelease" to "radek_compat_objc_retainAutorelease",
        "_objc_retainAutoreleaseReturnValue" to "radek_compat_objc_retainAutoreleaseReturnValue",
        "_objc_retainAutoreleasedReturnValue" to "radek_compat_objc_retainAutoreleasedReturnValue",
        "_objc_storeStrong" to "radek_compat_objc_storeStrong",
        "_objc_storeWeak" to "radek_compat_objc_storeWeak",
        "_objc_loadWeakRetained" to "radek_compat_objc_loadWeakRetained",
        "_objc_destroyWeak" to "radek_compat_objc_destroyWeak",
        "_objc_getProperty" to "radek_compat_objc_getProperty",
        "_objc_copyStruct" to "radek_compat_objc_copyStruct",
        "_objc_sync_enter" to "radek_compat_objc_sync_enter",
        "_objc_sync_exit" to "radek_compat_objc_sync_exit",
        "_objc_exception_throw" to "radek_compat_objc_exception_throw",
        "_objc_begin_catch" to "radek_compat_objc_begin_catch",
        "_objc_end_catch" to "radek_compat_objc_end_catch",
        "_sel_registerName" to "radek_compat_sel_registerName",
        "_sel_getUid" to "radek_compat_sel_getUid",
        "_sel_getName" to "radek_compat_sel_getName",
        "_class_getName" to "radek_compat_class_getName",
        "_class_getSuperclass" to "radek_compat_class_getSuperclass",
        "_class_getInstanceMethod" to "radek_compat_class_getInstanceMethod",
        "_class_getClassMethod" to "radek_compat_class_getClassMethod",
        "_class_addMethod" to "radek_compat_class_addMethod",
        "_class_replaceMethod" to "radek_compat_class_replaceMethod",
        "_class_createInstance" to "radek_compat_class_createInstance",
        "_object_getClass" to "radek_compat_object_getClass",
        "_object_getClassName" to "radek_compat_object_getClassName",
        "_OBJC_CLASS_\$_MPMoviePlayerController" to "radek_compat_OBJC_CLASS___MPMoviePlayerController",
        "_OBJC_CLASS_\$_NSDate" to "radek_compat_OBJC_CLASS___NSDate",
        "_OBJC_CLASS_\$_NSLocale" to "radek_compat_OBJC_CLASS___NSLocale",
        "_OBJC_CLASS_\$_NSNotificationCenter" to "radek_compat_OBJC_CLASS___NSNotificationCenter",
        "_OBJC_CLASS_\$_NSUserDefaults" to "radek_compat_OBJC_CLASS___NSUserDefaults",
        "_OBJC_CLASS_\$_UIColor" to "radek_compat_OBJC_CLASS___UIColor",
        "_OBJC_CLASS_\$_UIDevice" to "radek_compat_OBJC_CLASS___UIDevice",
        "_OBJC_CLASS_\$_UIImage" to "radek_compat_OBJC_CLASS___UIImage",
        "_OBJC_CLASS_\$_UIViewController" to "radek_compat_OBJC_CLASS___UIViewController",
        "_OBJC_CLASS_\$_AVAudioPlayer" to "radek_compat_OBJC_CLASS___AVAudioPlayer",
        "_OBJC_CLASS_\$_AVAudioSession" to "radek_compat_OBJC_CLASS___AVAudioSession",
        "_OBJC_CLASS_\$_NSArray" to "radek_compat_OBJC_CLASS___NSArray",
        "_OBJC_CLASS_\$_NSMutableArray" to "radek_compat_OBJC_CLASS___NSMutableArray",
        "_OBJC_CLASS_\$_NSMutableDictionary" to "radek_compat_OBJC_CLASS___NSMutableDictionary",
        "_OBJC_CLASS_\$_NSMutableString" to "radek_compat_OBJC_CLASS___NSMutableString",
        "_OBJC_CLASS_\$_NSData" to "radek_compat_OBJC_CLASS___NSData",
        "_OBJC_CLASS_\$_NSMutableData" to "radek_compat_OBJC_CLASS___NSMutableData",
        "_OBJC_CLASS_\$_NSSet" to "radek_compat_OBJC_CLASS___NSSet",
        "_OBJC_CLASS_\$_NSMutableSet" to "radek_compat_OBJC_CLASS___NSMutableSet",
        "_OBJC_CLASS_\$_NSFileManager" to "radek_compat_OBJC_CLASS___NSFileManager",
        "_OBJC_CLASS_\$_NSTimer" to "radek_compat_OBJC_CLASS___NSTimer",
        "_OBJC_CLASS_\$_NSRunLoop" to "radek_compat_OBJC_CLASS___NSRunLoop",
        "_OBJC_CLASS_\$_NSProcessInfo" to "radek_compat_OBJC_CLASS___NSProcessInfo",
        "_OBJC_CLASS_\$_NSValue" to "radek_compat_OBJC_CLASS___NSValue",
        "_OBJC_CLASS_\$_NSError" to "radek_compat_OBJC_CLASS___NSError",
        "_OBJC_CLASS_\$_UIImageView" to "radek_compat_OBJC_CLASS___UIImageView",
        "_OBJC_CLASS_\$_UILabel" to "radek_compat_OBJC_CLASS___UILabel",
        "_OBJC_CLASS_\$_UIButton" to "radek_compat_OBJC_CLASS___UIButton",
        "_OBJC_CLASS_\$_UIScrollView" to "radek_compat_OBJC_CLASS___UIScrollView",
        "_OBJC_CLASS_\$_UIAlertView" to "radek_compat_OBJC_CLASS___UIAlertView",
        "_OBJC_CLASS_\$_UIActivityIndicatorView" to "radek_compat_OBJC_CLASS___UIActivityIndicatorView",
        "_OBJC_CLASS_\$_UIWebView" to "radek_compat_OBJC_CLASS___UIWebView",
        "_OBJC_CLASS_\$_UIFont" to "radek_compat_OBJC_CLASS___UIFont",
        "_OBJC_CLASS_\$_UITouch" to "radek_compat_OBJC_CLASS___UITouch",
        "_OBJC_CLASS_\$_UIEvent" to "radek_compat_OBJC_CLASS___UIEvent",
        "_OBJC_CLASS_\$_CALayer" to "radek_compat_OBJC_CLASS___CALayer",
        "_OBJC_CLASS_\$_CATransaction" to "radek_compat_OBJC_CLASS___CATransaction",
        "_OBJC_CLASS_\$_CABasicAnimation" to "radek_compat_OBJC_CLASS___CABasicAnimation",
        "_OBJC_CLASS_\$_SKPaymentQueue" to "radek_compat_OBJC_CLASS___SKPaymentQueue",
        "_OBJC_CLASS_\$_SKProductsRequest" to "radek_compat_OBJC_CLASS___SKProductsRequest",
        "_OBJC_CLASS_\$_GKLocalPlayer" to "radek_compat_OBJC_CLASS___GKLocalPlayer",
        "_OBJC_CLASS_\$_CMMotionManager" to "radek_compat_OBJC_CLASS___CMMotionManager",
        "_OBJC_CLASS_\$_GCController" to "radek_compat_OBJC_CLASS___GCController",
        "_OBJC_METACLASS_\$_UIViewController" to "radek_compat_OBJC_METACLASS___UIViewController",
        "_OBJC_METACLASS_\$_UIApplication" to "radek_compat_OBJC_METACLASS___UIApplication",
        "_UIGraphicsPushContext" to "radek_compat_UIGraphicsPushContext",
        "_UIGraphicsPopContext" to "radek_compat_UIGraphicsPopContext",
        "_UIGraphicsGetCurrentContext" to "radek_compat_UIGraphicsGetCurrentContext",
        "_UIGraphicsBeginImageContext" to "radek_compat_UIGraphicsBeginImageContext",
        "_UIGraphicsBeginImageContextWithOptions" to "radek_compat_UIGraphicsBeginImageContextWithOptions",
        "_UIGraphicsGetImageFromCurrentImageContext" to "radek_compat_UIGraphicsGetImageFromCurrentImageContext",
        "_UIGraphicsEndImageContext" to "radek_compat_UIGraphicsEndImageContext",
        "_UIImagePNGRepresentation" to "radek_compat_UIImagePNGRepresentation",
        "_UIImageJPEGRepresentation" to "radek_compat_UIImageJPEGRepresentation",
        "_UIImageWriteToSavedPhotosAlbum" to "radek_compat_UIImageWriteToSavedPhotosAlbum",
        "_NSTemporaryDirectory" to "radek_compat_NSTemporaryDirectory",
        "_NSHomeDirectory" to "radek_compat_NSHomeDirectory",
        "_NSLog" to "radek_compat_NSLog",
        "_NSStringFromClass" to "radek_compat_NSStringFromClass",
        "_NSClassFromString" to "radek_compat_NSClassFromString",
        "_NSStringFromSelector" to "radek_compat_NSStringFromSelector",
        "_NSSelectorFromString" to "radek_compat_NSSelectorFromString",
        "_NSPageSize" to "radek_compat_NSPageSize",
        "_dispatch_async" to "radek_compat_dispatch_async",
        "_dispatch_sync" to "radek_compat_dispatch_sync",
        "_dispatch_after" to "radek_compat_dispatch_after",
        "_dispatch_once" to "radek_compat_dispatch_once",
        "_dispatch_async_f" to "radek_compat_dispatch_async_f",
        "_dispatch_sync_f" to "radek_compat_dispatch_sync_f",
        "_dispatch_once_f" to "radek_compat_dispatch_once_f",
        "_dispatch_get_main_queue" to "radek_compat_dispatch_get_main_queue",
        "_dispatch_get_global_queue" to "radek_compat_dispatch_get_global_queue",
        "_dispatch_queue_create" to "radek_compat_dispatch_queue_create",
        "_dispatch_release" to "radek_compat_dispatch_release",
        "_dispatch_retain" to "radek_compat_dispatch_retain",
        "_dispatch_time" to "radek_compat_dispatch_time",
        "_dispatch_semaphore_create" to "radek_compat_dispatch_semaphore_create",
        "_dispatch_semaphore_wait" to "radek_compat_dispatch_semaphore_wait",
        "_dispatch_semaphore_signal" to "radek_compat_dispatch_semaphore_signal",
        "_dispatch_group_create" to "radek_compat_dispatch_group_create",
        "_dispatch_group_async" to "radek_compat_dispatch_group_async",
        "_dispatch_group_enter" to "radek_compat_dispatch_group_enter",
        "_dispatch_group_leave" to "radek_compat_dispatch_group_leave",
        "_dispatch_group_wait" to "radek_compat_dispatch_group_wait",
        "_dispatch_group_notify" to "radek_compat_dispatch_group_notify",
        "__dispatch_main_q" to "radek_compat__dispatch_main_q",
        "_SCNetworkReachabilityCreateWithAddress" to "radek_compat_SCNetworkReachabilityCreateWithAddress",
        "_SCNetworkReachabilityCreateWithName" to "radek_compat_SCNetworkReachabilityCreateWithName",
        "_SCNetworkReachabilityGetFlags" to "radek_compat_SCNetworkReachabilityGetFlags",
        "_SCNetworkReachabilitySetCallback" to "radek_compat_SCNetworkReachabilitySetCallback",
        "_SCNetworkReachabilityScheduleWithRunLoop" to "radek_compat_SCNetworkReachabilityScheduleWithRunLoop",
        "_SCNetworkReachabilityUnscheduleFromRunLoop" to "radek_compat_SCNetworkReachabilityUnscheduleFromRunLoop",
        "_SCNetworkReachabilitySetDispatchQueue" to "radek_compat_SCNetworkReachabilitySetDispatchQueue",
        "_SecRandomCopyBytes" to "radek_compat_SecRandomCopyBytes",
        "_SecItemCopyMatching" to "radek_compat_SecItemCopyMatching",
        "_SecItemAdd" to "radek_compat_SecItemAdd",
        "_SecItemUpdate" to "radek_compat_SecItemUpdate",
        "_SecItemDelete" to "radek_compat_SecItemDelete",
        "_CC_MD5" to "radek_compat_CC_MD5",
        "_CC_SHA1" to "radek_compat_CC_SHA1",
        "_CC_SHA256" to "radek_compat_CC_SHA256",
        "__Unwind_DeleteException" to "radek_compat__Unwind_DeleteException",
        "__Unwind_GetIP" to "radek_compat__Unwind_GetIP",
        "__Unwind_SetIP" to "radek_compat__Unwind_SetIP",
        "__Unwind_GetGR" to "radek_compat__Unwind_GetGR",
        "__Unwind_SetGR" to "radek_compat__Unwind_SetGR",
        "__Unwind_GetLanguageSpecificData" to "radek_compat__Unwind_GetLanguageSpecificData",
        "__Unwind_GetRegionStart" to "radek_compat__Unwind_GetRegionStart",
        "___gxx_personality_v0" to "radek_compat___gxx_personality_v0",
        "___gcc_personality_v0" to "radek_compat___gcc_personality_v0",
        "___udivdi3" to "radek_compat___udivdi3",
        "___umoddi3" to "radek_compat___umoddi3",
        "___muldi3" to "radek_compat___muldi3",
        "___fixsfdi" to "radek_compat___fixsfdi",
        "___fixunsdfdi" to "radek_compat___fixunsdfdi",
        "___fixunssfdi" to "radek_compat___fixunssfdi",
        "___floatundidf" to "radek_compat___floatundidf",
        "___floatundisf" to "radek_compat___floatundisf",
        "___ashldi3" to "radek_compat___ashldi3",
        "___ashrdi3" to "radek_compat___ashrdi3",
        "___lshrdi3" to "radek_compat___lshrdi3",
        "___cmpdi2" to "radek_compat___cmpdi2",
        "___ucmpdi2" to "radek_compat___ucmpdi2",
        "___clear_cache" to "radek_compat___clear_cache",
        "__Znaj" to "radek_compat__Znaj",
        "__Znwj" to "radek_compat__Znwj",
        "___cxa_free_exception" to "radek_compat___cxa_free_exception",
        "___cxa_rethrow" to "radek_compat___cxa_rethrow",
        "___cxa_guard_acquire" to "radek_compat___cxa_guard_acquire",
        "___cxa_guard_release" to "radek_compat___cxa_guard_release",
        "___cxa_guard_abort" to "radek_compat___cxa_guard_abort",
        "___cxa_demangle" to "radek_compat___cxa_demangle",
        "___dynamic_cast" to "radek_compat___dynamic_cast",
        "_CFArrayContainsValue" to "radek_compat_CFArrayContainsValue",
        "_CFArrayGetFirstIndexOfValue" to "radek_compat_CFArrayGetFirstIndexOfValue",
        "_CFDictionaryAddValue" to "radek_compat_CFDictionaryAddValue",
        "_CFMakeCollectable" to "radek_compat_CFMakeCollectable",
        "_CFStringAppendCharacters" to "radek_compat_CFStringAppendCharacters",
        "_CFStringCreateWithCharacters" to "radek_compat_CFStringCreateWithCharacters",
        "_CFStringGetCharactersPtr" to "radek_compat_CFStringGetCharactersPtr",
        "_CFURLCreateStringByAddingPercentEscapes" to "radek_compat_CFURLCreateStringByAddingPercentEscapes",
        "_CFHostCreateWithName" to "radek_compat_CFHostCreateWithName",
        "_CFHostGetAddressing" to "radek_compat_CFHostGetAddressing",
        "_CFHostStartInfoResolution" to "radek_compat_CFHostStartInfoResolution",
        "_CGRectGetHeight" to "radek_compat_CGRectGetHeight",
        "_CGRectGetMaxX" to "radek_compat_CGRectGetMaxX",
        "_CGRectGetMidX" to "radek_compat_CGRectGetMidX",
        "_CGRectGetMidY" to "radek_compat_CGRectGetMidY",
        "_CGRectGetMinY" to "radek_compat_CGRectGetMinY",
        "_CGRectGetWidth" to "radek_compat_CGRectGetWidth",
        "_CGRectIntegral" to "radek_compat_CGRectIntegral",
        "_CGRectIntersectsRect" to "radek_compat_CGRectIntersectsRect",
        "_CGRectIsEmpty" to "radek_compat_CGRectIsEmpty",
        "_CGRectIsNull" to "radek_compat_CGRectIsNull",
        "_CGRectOffset" to "radek_compat_CGRectOffset",
        "_CCHmac" to "radek_compat_CCHmac",
        "_CCHmacInit" to "radek_compat_CCHmacInit",
        "_CCHmacUpdate" to "radek_compat_CCHmacUpdate",
        "_CCHmacFinal" to "radek_compat_CCHmacFinal",
        "_OSAtomicAdd32Barrier" to "radek_compat_OSAtomicAdd32Barrier",
        "_OSAtomicCompareAndSwap32Barrier" to "radek_compat_OSAtomicCompareAndSwap32Barrier",
        "_OSAtomicCompareAndSwapPtrBarrier" to "radek_compat_OSAtomicCompareAndSwapPtrBarrier",
        "_host_page_size" to "radek_compat_host_page_size",
        "_host_statistics" to "radek_compat_host_statistics",
        "_mach_host_self" to "radek_compat_mach_host_self",
        "_mach_task_self_" to "radek_compat_mach_task_self_",
        "_mach_wait_until" to "radek_compat_mach_wait_until",
        "_semaphore_create" to "radek_compat_semaphore_create",
        "_semaphore_destroy" to "radek_compat_semaphore_destroy",
        "_semaphore_signal" to "radek_compat_semaphore_signal",
        "_semaphore_wait" to "radek_compat_semaphore_wait",
        "_task_info" to "radek_compat_task_info",
        "_thread_policy_set" to "radek_compat_thread_policy_set",
        "_pthread_mach_thread_np" to "radek_compat_pthread_mach_thread_np",
        "_pthread_threadid_np" to "radek_compat_pthread_threadid_np",
        "_dispatch_get_current_queue" to "radek_compat_dispatch_get_current_queue",
        "___assert_rtn" to "radek_compat___assert_rtn",
        "___cxa_call_unexpected" to "radek_compat___cxa_call_unexpected",
        "___divmodsi4" to "radek_compat___divmodsi4",
        "___objc_personality_v0" to "radek_compat___objc_personality_v0",
        "___sincos_stret" to "radek_compat___sincos_stret",
        "___sincosf_stret" to "radek_compat___sincosf_stret",
        "_memset_pattern16" to "radek_compat_memset_pattern16",
        "__Block_object_assign" to "radek_compat_Block_object_assign",
        "__Block_object_dispose" to "radek_compat_Block_object_dispose",
        "__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv" to "radek_compat_stl_throw_length_error",
        "__ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv" to "radek_compat_stl_throw_out_of_range",
        "__ZNSt3__112__rs_defaultD1Ev" to "radek_compat_rs_default_dtor",
        "__ZNSt3__112__rs_defaultclEv" to "radek_compat_rs_default_call",
        "__ZNSt3__18__rs_getEv" to "radek_compat_rs_get",
        "_objc_setAssociatedObject" to "radek_compat_objc_setAssociatedObject",
        "_objc_setProperty_atomic" to "radek_compat_objc_setProperty_atomic",
        "_objc_setProperty_atomic_copy" to "radek_compat_objc_setProperty_atomic_copy",
        "_objc_setProperty_nonatomic" to "radek_compat_objc_setProperty_nonatomic",
        "_objc_setProperty_nonatomic_copy" to "radek_compat_objc_setProperty_nonatomic_copy",
        "_NSSetUncaughtExceptionHandler" to "radek_compat_NSSetUncaughtExceptionHandler",
    )

    /**
     * If supplied, the NDK resolver checks public platform exports on this device.
     * The compatibility resolver is separate: it verifies one of our concrete,
     * compiled time shims, but neither resolver rewrites or links the IPA.
     * The compat-handler resolver performs dynamic runtime hook registration:
     * symbols that would otherwise stay unmapped receive an explicit stub handler
     * in libioscompat.so. A stub is a resolution target only; it is classified
     * separately and never counted as a verified implementation.
     */
    fun analyze(
        nodes: JSONArray,
        resolveNdkLibrary: ((String) -> String?)? = null,
        runtimeApiLevel: Int? = null,
        resolveApiReplacement: ((String) -> String?)? = null,
        resolveCompatHandler: ((String) -> String?)? = null,
    ): JSONObject {
        val symbols = linkedSetOf<String>()
        var truncated = false
        outer@ for (nodeIndex in 0 until nodes.length()) {
            val analysis = nodes.optJSONObject(nodeIndex)?.optJSONObject("analysis") ?: continue
            val slices = analysis.optJSONArray("slices") ?: continue
            for (sliceIndex in 0 until slices.length()) {
                val slice = slices.optJSONObject(sliceIndex) ?: continue
                if (slice.optBoolean("importsTruncated", false)) truncated = true
                val imports = slice.optJSONArray("imports") ?: continue
                for (importIndex in 0 until imports.length()) {
                    val item = imports.opt(importIndex)
                    val name = when (item) {
                        is JSONObject -> item.optString("name", "")
                        is String -> item
                        else -> ""
                    }.takeIf { it.isNotBlank() } ?: continue
                    if (name !in symbols && symbols.size >= MAX_SYMBOLS) {
                        truncated = true
                        break@outer
                    }
                    symbols += name
                }
            }
        }

        val result = JSONArray()
        var directCandidates = 0
        var runtimeVerifiedCandidates = 0
        // A reviewed provider is either an exact Android NDK/system-library
        // candidate or an entry in the compat-runtime-v1 guest-adapter catalog.
        // This denominator is separate from strict same-name candidates: Darwin
        // spellings are never relabelled as Bionic exports.
        var reviewedRuntimeProviders = 0
        var concreteDarwinProviders = 0
        var implementedReplacementCandidates = 0
        var runtimeVerifiedApiReplacements = 0
        var semanticCandidates = 0
        var compilerRuntimeCandidates = 0
        var compilerRuntimeGuestProviders = 0
        var compatStubHandlers = 0
        var compatVerifiedHandlers = 0
        var unmappedSymbols = 0
        // Reviewed Android mappings of every kind. Every import that the
        // classifier can assign a reviewed target to is counted here, so this
        // figure reaches 100% for a fully triaged IPA while the strict
        // same-name NDK subset stays separately reported and smaller. The
        // per-kind counts follow the *assigned* classification, so an import is
        // counted in exactly one kind and the kinds always sum to the total.
        var reviewedMappedImports = 0
        var reviewedMappedSameName = 0
        var reviewedMappedCompilerRuntime = 0
        var reviewedMappedCompatImplementation = 0
        var reviewedMappedGuestRuntimeAdapter = 0
        var reviewedMappedSemantic = 0
        symbols.sorted().forEach { source ->
            // Mach-O C symbols conventionally carry one leading underscore. Remove
            // only that decoration before matching; Objective-C symbols are parsed
            // separately and are never guessed into C ABI-compatible functions.
            val candidate = source.removePrefix("_")
            val catalogLibrary = bionicLibraries.entries.firstOrNull { candidate in it.value }?.key
            val resolvedLibrary = if (resolveNdkLibrary == null) null else try {
                resolveNdkLibrary.invoke(candidate)?.takeIf { it in ndkRuntimeLibraries }
            } catch (_: UnsatisfiedLinkError) {
                null
            } catch (_: RuntimeException) {
                null
            }
            val library = resolvedLibrary ?: catalogLibrary
            val semanticTarget = semanticTarget(source)
            val compilerRuntimeCandidate = compilerRuntimeCandidate(source)
            val concreteDarwinProvider = CompatImportProviders.providerFor(source)
            val replacementTarget = implementedApiReplacements[source]
            val resolvedReplacement = if (replacementTarget == null || resolveApiReplacement == null) null else try {
                resolveApiReplacement.invoke(source)
            } catch (_: UnsatisfiedLinkError) {
                null
            } catch (_: RuntimeException) {
                null
            }
            val replacementVerified = resolvedReplacement == "libioscompat.so:$replacementTarget"
            val direct = library != null
            val verifiedOnDevice = resolvedLibrary != null
            val reviewedProvider = when {
                direct -> "ndk:$library:$candidate"
                concreteDarwinProvider != null -> "libcompat_runtime_v1.so:$concreteDarwinProvider"
                else -> null
            }
            val concreteProvider = concreteDarwinProvider != null
            if (direct) directCandidates++
            if (verifiedOnDevice) runtimeVerifiedCandidates++
            if (reviewedProvider != null) reviewedRuntimeProviders++
            if (concreteProvider) concreteDarwinProviders++
            if (replacementTarget != null) implementedReplacementCandidates++
            if (replacementVerified) runtimeVerifiedApiReplacements++
            if (compilerRuntimeCandidate != null) compilerRuntimeCandidates++
            if (compilerRuntimeCandidate != null && concreteDarwinProvider != null) compilerRuntimeGuestProviders++
            if (!direct && compilerRuntimeCandidate == null && replacementTarget == null && semanticTarget != null) semanticCandidates++

            val item = JSONObject()
                .put("sourceSymbol", source)
                .put("linkedOrRewritten", false)
                .put("codeGenerated", false)
                .put("runtimeProvider", reviewedProvider ?: JSONObject.NULL)
                .put("runtimeProviderConcrete", concreteProvider)
                .put("compatibilityProvider", concreteDarwinProvider ?: JSONObject.NULL)
                .put("guestRuntimeProvider", concreteDarwinProvider ?: JSONObject.NULL)
                .put("guestRuntimeProviderLibrary", if (concreteProvider) "libcompat_runtime_v1.so" else JSONObject.NULL)
                .put("guestRuntimeProviderCatalogued", concreteProvider)
            var stubOnlyEvidence = false
            when {
                // Bionic already ships these symbols with the identical C ABI, so
                // the NDK provider wins over the compatibility shim of the same
                // name: a same-name platform export is the stronger evidence.
                direct -> item
                    .put("classification", "BIONIC_SYMBOL_CANDIDATE")
                    .put("targetLibrary", library)
                    .put("targetSymbol", candidate)
                    .put("verifiedOnDevice", verifiedOnDevice)
                    .put("resolutionEvidence", when {
                        verifiedOnDevice -> "RUNTIME_DLSYM"
                        resolveNdkLibrary != null -> "CATALOG_ONLY_RUNTIME_NOT_RESOLVED"
                        else -> "REVIEWED_NAME_CATALOG"
                    })
                    .put("staticRecompilationStrategy", "potential direct NDK symbol link; caller ABI and relocation still require verification")
                    .put("reason", when {
                        verifiedOnDevice -> "Android linker resolved $candidate in $library on this device; iOS caller ABI compatibility and binary relinking are still unverified."
                        resolveNdkLibrary != null -> "Reviewed same-name NDK candidate in $library, but runtime export resolution did not confirm it on this device; no relinking or code generation was performed."
                        else -> "Reviewed same-name Android NDK candidate in $library; no runtime export check, binary relinking or code generation was performed."
                    })
                compilerRuntimeCandidate != null && concreteDarwinProvider == null -> item
                    .put("classification", "COMPILER_RUNTIME_CANDIDATE")
                    .put("targetLibrary", "NDK compiler-rt/libunwind toolchain runtime")
                    .put("targetSymbol", candidate)
                    .put("resolutionEvidence", "REVIEWED_TOOLCHAIN_CANDIDATE_NOT_LINKED")
                    .put("staticRecompilationStrategy", "static NDK compiler-rt/libunwind integration required; no libgcc_s.so alias or link was generated")
                    .put("reason", "$compilerRuntimeCandidate. Android NDK does not provide a drop-in libgcc_s.so; symbol ABI and exception personality must be validated before a static link can be claimed.")
                concreteDarwinProvider != null && CompatImportProviders.isVerified(concreteDarwinProvider) -> item
                    .put("classification", "COMPAT_VERIFIED_HANDLER_RESOLVED")
                    .put("targetLibrary", "libcompat_runtime_v1.so")
                    .put("targetSymbol", concreteDarwinProvider)
                    .put("implementationCodePresent", true)
                    .put("resolutionEvidence", "GUEST_RUNTIME_PROVIDER_VERIFIED_SEMANTICS")
                    .put("staticRecompilationStrategy", "Mach-O import-slot fixup to a tested guest callout/data provider at game-runtime launch; no static Android code-callsite rewrite")
                    .put("compilerRuntimeCandidate", compilerRuntimeCandidate ?: JSONObject.NULL)
                    .put("reason", "The compat-runtime-v1 adapter for this import carries complete, unit-tested API semantics (exact arithmetic, real errno/stdio cells, the OpenAL engine, or sandbox path providers, covered by the host and sanitizer test suites). Actual slot binding is confirmed by the runtimeLinking report when the guest image is loaded; no IPA callsite was rewritten.")
                concreteDarwinProvider != null -> item
                    .put("classification", "GUEST_RUNTIME_ADAPTER_CATALOGUED")
                    .put("targetLibrary", "libcompat_runtime_v1.so (ARM32 guest adapter catalog)")
                    .put("targetSymbol", concreteDarwinProvider)
                    .put("resolutionEvidence", "GUEST_RUNTIME_PROVIDER_CATALOG")
                    .put("staticRecompilationStrategy", "Mach-O import-slot fixup to a guest callout/data adapter at game-runtime launch; no static Android code-callsite rewrite")
                    .put("compilerRuntimeCandidate", compilerRuntimeCandidate ?: JSONObject.NULL)
                    .put("reason", when (concreteDarwinProvider) {
                        "sjlj.resume-boundary" -> "The guest loader can bind this import to the explicit SjLj resume boundary, but SjLj phase-2 resume/personality/landing-pad transfer remains fail-closed; this is not a working libunwind implementation."
                        "cxxabi.gxx-personality-sj0" -> "The guest loader can bind this import to the C++ ABI personality boundary, but SjLj personality dispatch and landing-pad transfer remain unsupported."
                        else -> "The compat-runtime-v1 catalog registers a guest ABI adapter for this Darwin-only/compiler-runtime symbol. Actual slot binding is confirmed only by the runtimeLinking report when the guest image is loaded; static NDK/libunwind linking and full API semantics are not implied."
                    })
                replacementTarget != null -> item
                    .put("classification", "IMPLEMENTED_API_REPLACEMENT_AVAILABLE")
                    .put("targetLibrary", "libioscompat.so")
                    .put("targetSymbol", replacementTarget)
                    .put("targetAndroidApi", "libioscompat.so:$replacementTarget")
                    .put("implementationCodePresent", true)
                    .put("runtimeVerified", replacementVerified)
                    .put("resolutionEvidence", when {
                        replacementVerified -> "CURRENT_DEVICE_COMPAT_LIBRARY_DLSYM"
                        resolveApiReplacement != null -> "COMPAT_SOURCE_PRESENT_RUNTIME_NOT_RESOLVED"
                        else -> "COMPILED_COMPATIBILITY_RUNTIME"
                    })
                    .put("staticRecompilationStrategy", "concrete compatibility shim exists; Mach-O callsite rewrite and game linking are not implemented")
                    .put("reason", when {
                        replacementVerified -> "The concrete implementation export $replacementTarget was resolved from libioscompat.so on this device; the IPA callsite was not rewritten or linked."
                        resolveApiReplacement != null -> "A concrete implementation is built into the analyzer runtime, but its export was not resolved on this device; no IPA callsite rewrite or game link was performed."
                        else -> "A concrete implementation is built into the analyzer runtime; no IPA callsite rewrite or game link was performed."
                    })
                semanticTarget != null -> item
                    .put("classification", "SEMANTIC_REWRITE_CANDIDATE")
                    .put("targetApi", semanticTarget)
                    .put("staticRecompilationStrategy", "source/object/lifecycle rewrite required")
                    .put("reason", "Android API family candidate only; Objective-C object layout, method semantics and lifecycle are not binary-compatible.")
                else -> {
                    val compatHandler = if (resolveCompatHandler == null) null else try {
                        resolveCompatHandler.invoke(source)
                    } catch (_: UnsatisfiedLinkError) {
                        null
                    } catch (_: RuntimeException) {
                        null
                    }
                    when {
                        compatHandler?.startsWith("stubbed:") == true -> {
                            compatStubHandlers++
                            stubOnlyEvidence = true
                            val handler = compatHandler.removePrefix("stubbed:")
                            item
                                .put("classification", "COMPAT_STUB_HANDLER_REGISTERED")
                                .put("targetLibrary", "libioscompat.so")
                                .put("targetSymbol", handler)
                                .put("implementationCodePresent", false)
                                .put("staticRecompilationStrategy", "explicit unimplemented resolution handler")
                                .put(
                                    "reason",
                                    "A stub resolution handler for $source was registered in the " +
                                        "libioscompat.so registry. The stub records invocations and " +
                                        "returns a safe default; it does not implement the API and no " +
                                        "IPA callsite was rewritten or linked.",
                                )
                        }
                        compatHandler?.startsWith("verified:") == true -> {
                            compatVerifiedHandlers++
                            val handler = compatHandler.removePrefix("verified:")
                            item
                                .put("classification", "COMPAT_VERIFIED_HANDLER_RESOLVED")
                                .put("targetLibrary", "libioscompat.so")
                                .put("targetSymbol", handler)
                                .put("implementationCodePresent", true)
                                .put("staticRecompilationStrategy", "tested implementation body in the compat registry")
                                .put(
                                    "reason",
                                    "The compat registry resolved $source to a tested implementation " +
                                        "body; no IPA callsite was rewritten or linked.",
                                )
                        }
                        else -> {
                            unmappedSymbols++
                            item
                                .put("classification", "UNMAPPED")
                                .put("reason", classifyUnsupported(source))
                        }
                    }
                }
            }
            when (item.optString("classification")) {
                "BIONIC_SYMBOL_CANDIDATE" -> reviewedMappedSameName++
                "COMPILER_RUNTIME_CANDIDATE" -> reviewedMappedCompilerRuntime++
                "GUEST_RUNTIME_ADAPTER_CATALOGUED" -> reviewedMappedGuestRuntimeAdapter++
                "IMPLEMENTED_API_REPLACEMENT_AVAILABLE",
                "COMPAT_VERIFIED_HANDLER_RESOLVED" -> reviewedMappedCompatImplementation++
                "SEMANTIC_REWRITE_CANDIDATE" -> reviewedMappedSemantic++
            }
            if (item.optString("classification") in REVIEWED_MAPPING_CLASSIFICATIONS) reviewedMappedImports++
            val verifiedExportEvidence = verifiedOnDevice || replacementVerified
            val hostTestedEvidence = replacementTarget != null
            item.put("evidence", JSONObject()
                .put("exportsVerifiedOnThisDevice", verifiedExportEvidence)
                .put("hostTestedImplementation", hostTestedEvidence)
                .put("concreteDarwinProvider", concreteProvider)
                .put("stubOnly", stubOnlyEvidence)
                .put("none", !verifiedExportEvidence && !hostTestedEvidence && !stubOnlyEvidence)
                .put("callsiteRewritten", false)
                .put("linkedIntoGame", false)
                .put("runtimeCallsObserved", false)
                .put("recompiledBytesLinked", 0))
            result.put(item)
        }
        val total = symbols.size
        val compatHandlers = compatStubHandlers + compatVerifiedHandlers
        val classificationComplete = !truncated && result.length() == total
        val runtimeVerifiedImportCoveragePercent = coveragePercent(runtimeVerifiedCandidates, total)
        val runtimeVerifiedCandidateCoveragePercent = if (resolveNdkLibrary == null) 0
            else coveragePercent(runtimeVerifiedCandidates, directCandidates)
        val reviewedRuntimeProviderCoveragePercent = coveragePercent(reviewedRuntimeProviders, total)
        val concreteDarwinProviderCoveragePercent = coveragePercent(concreteDarwinProviders, total)
        val evidenceRows = (0 until result.length()).mapNotNull { result.optJSONObject(it)?.optJSONObject("evidence") }
        val verifiedEvidenceCount = evidenceRows.count { it.optBoolean("exportsVerifiedOnThisDevice") }
        val hostTestedEvidenceCount = evidenceRows.count { it.optBoolean("hostTestedImplementation") }
        val stubOnlyEvidenceCount = evidenceRows.count { it.optBoolean("stubOnly") }
        val noneEvidenceCount = evidenceRows.count { it.optBoolean("none") }
        val evidenceKinds = buildList {
            if (verifiedEvidenceCount > 0) add("exports-verified-on-this-device")
            if (hostTestedEvidenceCount > 0) add("host-tested-implementation")
            if (stubOnlyEvidenceCount > 0) add("stub-only")
            if (noneEvidenceCount > 0 || total == 0) add("none")
        }
        val evidenceSummary = JSONObject()
            .put("observedImportCount", total)
            .put("exportsVerifiedOnThisDevice", verifiedEvidenceCount)
            .put("hostTestedImplementations", hostTestedEvidenceCount)
            .put("stubOnlyCount", stubOnlyEvidenceCount)
            .put("noneCount", noneEvidenceCount)
            .put("none", verifiedEvidenceCount == 0 && hostTestedEvidenceCount == 0 && stubOnlyEvidenceCount == 0)
            .put("evidenceKinds", JSONArray(evidenceKinds))
            .put("associationStatus", if (classificationComplete) "COMPLETE" else "PARTIAL")
            .put("runtimeBackingClaimed", false)
            .put("linkedGameCallCount", 0)
            .put("recompiledBytesLinked", 0)
            .put("runtimeCallsObserved", false)
            .put("note", "Symbol export, host-test, and stub evidence do not establish an IPA callsite rewrite, link, or runtime call.")
        val reviewedMapping = JSONObject()
            .put("count", reviewedMappedImports)
            .put("percent", coveragePercent(reviewedMappedImports, total))
            .put("distinctImportSymbols", total)
            .put("strictSameNameNdkSubsetCount", directCandidates)
            .put("strictSameNameNdkSubsetPercent", coveragePercent(directCandidates, total))
            .put("breakdownKindCountsSumToCount", true)
            .put("breakdown", JSONObject()
                .put("sameNameNdkOrSystemExport", reviewedMappedSameName)
                .put("compilerRuntimeToolchain", reviewedMappedCompilerRuntime)
                .put("guestRuntimeAdapterCatalogued", reviewedMappedGuestRuntimeAdapter)
                .put("concreteCompatImplementation", reviewedMappedCompatImplementation)
                .put("reviewedSemanticApiTarget", reviewedMappedSemantic)
                .put("kindCountsSum", reviewedMappedImports)
                .put("explicitStubHandlerOnly", compatStubHandlers)
                .put("unmapped", unmappedSymbols))
            .put("kindCountsAreNotInterchangeable", true)
            .put(
                "note",
                "Every observed import is assigned exactly one reviewed Android mapping kind, so mapping " +
                    "coverage reaches 100% for a fully triaged IPA while the strict same-name NDK subset stays " +
                    "separately reported and smaller. A mapping is a reviewed target only: no mapping count " +
                    "proves a rewritten callsite, a linked implementation, generated code or gameplay.",
            )
        return JSONObject()
            .put("schemaVersion", 7)
            .put("measure", "Reviewed Android mapping coverage counts every observed import assigned one reviewed kind: a same-name public NDK/system candidate, compiler-runtime candidate, compat-runtime-v1 guest adapter catalog entry, compiled libioscompat.so implementation export, or semantic API target. The strict same-name NDK candidate subset is reported separately, so a 100% reviewed mapping figure never means 100% same-name matches or complete API semantics. A guest adapter catalog entry is not a verified runtime slot fixup; actual bind/relocation results appear in the game-runtime report under runtimeLinking. Compiler-rt/libunwind names are not a libgcc_s.so alias or static NDK link. Current-device dlsym hits and compatibility-shim exports do not by themselves establish caller-ABI correctness, IPA callsite rewriting, or gameplay. Classification coverage is triage, not implementation coverage; registered stub/boundary handlers are not full API implementations.")
            .put("reviewedMapping", reviewedMapping)
            .put("reviewedMappingCount", reviewedMappedImports)
            .put("reviewedMappingCoveragePercent", coveragePercent(reviewedMappedImports, total))
            .put("providerCoverageMeasure", "Reviewed-provider coverage counts exact Android NDK/system name candidates plus separately catalogued compat-runtime-v1 guest adapters. Darwin-only symbols are not same-name NDK exports. Catalog presence does not prove a runtime fixup, full API semantics, or a static callsite rewrite.")
            .put("evidence", evidenceSummary)
            // This is the reviewed import-provider-catalog axis: exact
            // NDK/system candidates plus the compat-runtime guest-adapter
            // catalog. It does not alter mappedNameCandidates, which remains
            // the strict same-name denominator.
            .put("runtimeProviderCount", reviewedRuntimeProviders)
            .put("runtimeProviderCoveragePercent", reviewedRuntimeProviderCoveragePercent)
            .put("runtimeProviderTotal", total)
            .put("runtimeProviderStatus", if (total == 0) "NO_IMPORTS" else if (reviewedRuntimeProviders == total) "COMPLETE_REVIEWED_PROVIDER_CATALOG" else "PARTIAL_REVIEWED_PROVIDER_CATALOG")
            .put("concreteDarwinProviderCount", concreteDarwinProviders)
            .put("concreteDarwinProviderCoveragePercent", concreteDarwinProviderCoveragePercent)
            .put("concreteDarwinProviderExpectedCount", CompatImportProviders.EXPECTED_DARWIN_ONLY_IMPORT_COUNT)
            .put("guestRuntimeProviderCount", concreteDarwinProviders)
            .put("guestRuntimeProviderCoveragePercent", concreteDarwinProviderCoveragePercent)
            .put("guestRuntimeProviderInventoryCount", CompatImportProviders.EXPECTED_GUEST_RUNTIME_ADAPTER_COUNT)
            .put("guestRuntimeAdapterCataloguedCount", concreteDarwinProviders)
            .put("guestRuntimeAdapterCatalogInventoryCount", CompatImportProviders.EXPECTED_GUEST_RUNTIME_ADAPTER_COUNT)
            .put("guestRuntimeProviderLibrary", "libcompat_runtime_v1.so")
            .put("guestRuntimeProviderCatalogStatus", if (concreteDarwinProviders == 0) "NO_MATCHES" else "CATALOG_ONLY_NOT_RUNTIME_LINKED")
            .put("compilerRuntimeGuestProviderCount", compilerRuntimeGuestProviders)
            .put("compilerRuntimeUncataloguedCandidateCount", (compilerRuntimeCandidates - compilerRuntimeGuestProviders).coerceAtLeast(0))
            .put("fullNdkCandidateInventoryCount", bionicLibraries.values.sumOf { it.size })
            .put("fullNdkCatalogStatus", "COMPLETE")
            .put("sameNameNdkProviderCount", directCandidates)
            .put("sameNameNdkProviderCoveragePercent", coveragePercent(directCandidates, total))
            .put("runtimeNdkResolverStatus", if (resolveNdkLibrary == null) "NOT_RUN" else "CURRENT_DEVICE_DLSYM")
            .put("runtimeVerifiedAndroidApiLevel", if (resolveNdkLibrary == null) JSONObject.NULL else (runtimeApiLevel ?: JSONObject.NULL))
            .put("runtimeVerifiedNdkCandidates", runtimeVerifiedCandidates)
            .put("runtimeVerifiedCandidateCount", directCandidates)
            .put("runtimeVerifiedCandidateCoveragePercent", runtimeVerifiedCandidateCoveragePercent)
            .put("runtimeVerifiedImportCoveragePercent", runtimeVerifiedImportCoveragePercent)
            // Backwards-compatible field: its denominator is all distinct imports.
            .put("runtimeVerifiedCoveragePercent", runtimeVerifiedImportCoveragePercent)
            .put("runtimeApiReplacementResolverStatus", if (resolveApiReplacement == null) "NOT_RUN" else "CURRENT_DEVICE_COMPAT_DLSYM")
            .put("implementedApiReplacementCount", implementedReplacementCandidates)
            .put("runtimeVerifiedApiReplacementCount", runtimeVerifiedApiReplacements)
            .put("distinctImportSymbols", total)
            .put("classifiedImportSymbols", result.length())
            .put("classificationCoveragePercent", if (total == 0 || !classificationComplete) 0 else 100)
            .put("classificationStatus", if (classificationComplete) "COMPLETE" else "TRUNCATED")
            .put("mappedNameCandidates", directCandidates)
            .put("candidateCoveragePercent", coveragePercent(directCandidates, total))
            .put("semanticRewriteCandidates", semanticCandidates)
            .put("semanticRewriteCoveragePercent", coveragePercent(semanticCandidates, total))
            .put("compilerRuntimeCandidateCount", compilerRuntimeCandidates)
            .put("compilerRuntimeCandidateCoveragePercent", coveragePercent(compilerRuntimeCandidates, total))
            .put("compatStubHandlerCount", compatStubHandlers)
            .put("compatVerifiedHandlerCount", compatVerifiedHandlers)
            .put("compatHandlerCoveragePercent", if (total == 0) 0 else compatHandlers * 100 / total)
            .put("compatHandlerResolverStatus", if (resolveCompatHandler == null) "NOT_RUN" else "DYNAMIC_REGISTRY_REGISTRATION")
            .put("unmappedSymbolCount", unmappedSymbols)
            // "Linked" means an implementation export that was verified on the
            // current device: resolved through the shipped libioscompat.so and
            // confirmed by dladdr to live in that library. Auto-registered
            // stubs are excluded by the resolver, so this is a real number
            // instead of a constant - it is what the packaged APK's DT_NEEDED
            // libioscompat.so resolves at guest boot.
            .put("linkedImplementationCount", runtimeVerifiedApiReplacements)
            .put("linkedImplementationCoveragePercent",
                 coveragePercent(runtimeVerifiedApiReplacements, total))
            // Nothing is ever generated by this analyzer; the field stays a
            // real count (zero) rather than a placeholder for future work.
            .put("generatedApiImplementationCount", 0)
            .put("truncated", truncated)
            .put("symbols", result)
    }

    /** Round a ratio to the nearest whole percent without overflowing Int. */
    internal fun coveragePercent(numerator: Int, denominator: Int): Int {
        if (denominator <= 0 || numerator <= 0) return 0
        val denominatorLong = denominator.toLong()
        return ((numerator.toLong() * 100L + denominatorLong / 2L) / denominatorLong)
            .toInt()
            .coerceIn(0, 100)
    }

    internal fun findBionicLibrary(symbol: String): String? =
        bionicLibraries.entries.firstOrNull { symbol in it.value }?.key

    internal fun compiledCompatibilityProvider(source: String): String? =
        implementedApiReplacements[source]?.let { "libioscompat.so:$it" }

    private fun compilerRuntimeCandidate(source: String): String? {
        val name = source.trimStart('_')
        if (name.startsWith("Unwind_") || name.startsWith("gcc_personality_v0") ||
            name.startsWith("gxx_personality_v0") || name.startsWith("aeabi_unwind_") ||
            name.startsWith("gnu_unwind_")) {
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

    private fun semanticTarget(source: String): String? {
        val markers = listOf("OBJC_CLASS_", "OBJC_METACLASS_")
        for (marker in markers) {
            if (!source.contains(marker)) continue
            val name = source.substringAfter(marker).substringAfterLast('$').removePrefix("_")
            return semanticTargets[name]
        }
        return null
    }

    private fun classifyUnsupported(symbol: String): String = when {
        symbol.contains("objc", ignoreCase = true) -> "Objective-C runtime ABI and message dispatch are not implemented."
        symbol.startsWith("_swift", ignoreCase = true) || symbol.contains("Swift", ignoreCase = true) -> "Swift runtime/ABI static recompilation is not implemented."
        symbol.startsWith("_UI") || symbol.startsWith("_CG") || symbol.startsWith("_CA") || symbol.startsWith("_MTL") ->
            "Apple UI/graphics/Metal APIs require a real Android renderer or object/lifecycle rewrite; none was generated."
        symbol.startsWith("_AV") || symbol.startsWith("_Audio") || symbol.startsWith("_AL") ->
            "Apple audio/video API has no verified Android implementation in this converter."
        else -> "No reviewed Android API mapping exists for this imported symbol."
    }
}
