cmake_minimum_required(VERSION 3.22)
project(radek LANGUAGES CXX)
# Dynarmic's A32 headers require C++20; the rest of the native tree is
# forward-compatible with it (and the Gradle NDK build already passes -std=c++20).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(radek_analysis STATIC src/macho.cpp src/trivial.cpp)
set_target_properties(radek_analysis PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(radek_analysis PUBLIC include)
target_compile_options(radek_analysis PRIVATE -Wall -Wextra -Werror)

# This is a small, concrete Android compatibility library, not a general Apple
# framework/runtime. It is linked into the analyzer so its exports can be
# verified on-device; the bounded APK writer also packages this library and
# declares it as DT_NEEDED. This does not rewrite unsupported Mach-O callsites.
add_library(ioscompat SHARED src/apple_time_compat.cpp src/radek_ios_shims.cpp src/ioscompat_registry.cpp src/cad_display_link_compat.cpp)
target_include_directories(ioscompat PUBLIC include)
target_compile_options(ioscompat PRIVATE -Wall -Wextra -Werror)
find_package(Threads REQUIRED)
target_link_libraries(ioscompat PRIVATE Threads::Threads)

if(ANDROID)
    # NDK r27 remains in use by this project. Explicitly request 16 KiB PT_LOAD
    # alignment so the packaged shim runtime works on Android 15+ 16 KiB-page
    # devices as well as the APK's 16 KiB ZIP payload alignment.
    target_link_options(ioscompat PRIVATE "-Wl,-z,max-page-size=16384")
    add_library(radek SHARED src/jni.cpp)
    target_link_options(radek PRIVATE "-Wl,-z,max-page-size=16384")
    target_link_libraries(radek PRIVATE radek_analysis ioscompat dl)
else()
    add_executable(radek-macho src/main.cpp)
    target_link_libraries(radek-macho PRIVATE radek_analysis)
    enable_testing()
    add_executable(runtime-tests tests/runtime.cpp)
    target_include_directories(runtime-tests PRIVATE include)
    target_link_libraries(runtime-tests PRIVATE Threads::Threads)
    add_test(NAME runtime COMMAND runtime-tests)
    add_executable(api-compat-tests tests/apple_time_compat.cpp src/apple_time_compat.cpp)
    target_include_directories(api-compat-tests PRIVATE include)
    target_compile_options(api-compat-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(api-compat-tests PRIVATE Threads::Threads)
    add_test(NAME api-compat COMMAND api-compat-tests)
    add_executable(trivial-tests tests/trivial.cpp)
    target_include_directories(trivial-tests PRIVATE include)
    target_link_libraries(trivial-tests PRIVATE radek_analysis Threads::Threads)
    add_test(NAME trivial COMMAND trivial-tests)
    add_executable(ios-shims-tests tests/radek_ios_shims.cpp src/radek_ios_shims.cpp)
    target_include_directories(ios-shims-tests PRIVATE include)
    target_compile_options(ios-shims-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(ios-shims-tests PRIVATE Threads::Threads)
    add_test(NAME ios-shims COMMAND ios-shims-tests)
    add_executable(ioscompat-registry-tests tests/ioscompat_registry.cpp src/ioscompat_registry.cpp
                                            src/apple_time_compat.cpp src/radek_ios_shims.cpp)
    target_include_directories(ioscompat-registry-tests PRIVATE include)
    target_compile_options(ioscompat-registry-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(ioscompat-registry-tests PRIVATE Threads::Threads)
    add_test(NAME ioscompat-registry COMMAND ioscompat-registry-tests)
    add_executable(cad-display-link-tests tests/cad_display_link_compat.cpp src/cad_display_link_compat.cpp)
    target_include_directories(cad-display-link-tests PRIVATE include)
    target_compile_options(cad-display-link-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(cad-display-link-tests PRIVATE Threads::Threads)
    add_test(NAME cad-display-link COMMAND cad-display-link-tests)
endif()

option(RADEK_BUILD_COMPAT_RUNTIME "Build and test compat-runtime-v1" OFF)
option(RADEK_FETCH_DYNARMIC "Fetch the pinned Dynarmic ARM32 backend" OFF)
if(RADEK_BUILD_COMPAT_RUNTIME)
    find_package(Threads REQUIRED)

    set(COMPAT_RUNTIME_SOURCES
        src/compat_runtime/guest_memory.cpp
        src/compat_runtime/shim_registry.cpp
        src/compat_runtime/macho_loader.cpp
        src/compat_runtime/runner.cpp
        src/compat_runtime/virtual_file_system.cpp
        src/compat_runtime/cpu.cpp
        src/compat_runtime/objc_runtime.cpp
        src/compat_runtime/objc_shims.cpp
        src/compat_runtime/audio_session_shims.cpp
        src/compat_runtime/compiler_rt_shims.cpp
        src/compat_runtime/cxxabi_shims.cpp
        src/compat_runtime/ndk_compat_shims.cpp
        src/compat_runtime/sjlj_unwind.cpp
        src/compat_runtime/trap_shims.cpp
        src/compat_runtime/libsystem_shims.cpp
        src/compat_runtime/gles_shims.cpp
        src/compat_runtime/darwin_compat_shims.cpp
        src/compat_runtime/app_lifecycle.cpp
        src/compat_runtime/openal_backend.cpp
    )
    if(RADEK_FETCH_DYNARMIC)
        include(FetchContent)
        # Dynarmic (suyu-emu fork of MerryMage's dynarmic, the actively
        # maintained ARM JIT) as the sole ARM32 execution backend. Only the A32
        # frontend and the externals its emitter needs are materialized.
        set(DYNARMIC_TESTS OFF CACHE BOOL "Do not build upstream dynarmic tests" FORCE)
        set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "No PCH in the compat-runtime build" FORCE)
        set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "Upstream warnings are not errors here" FORCE)
        set(DYNARMIC_FRONTENDS "A32" CACHE STRING "ARM32 guest frontend only" FORCE)
        set(DYNARMIC_ENABLE_CPU_FEATURE_DETECTION OFF CACHE BOOL "Baseline host CPU features" FORCE)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        # Dynarmic needs Boost headers (boost::variant / boost::icl). Point
        # find_package(Boost) at the pruned vendored header set so host and NDK
        # builds are deterministic and need no system Boost installation.
        set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost-headers"
            CACHE PATH "Vendored Boost headers for the Dynarmic backend" FORCE)
        FetchContent_Declare(
            dynarmic
            GIT_REPOSITORY https://github.com/suyu-emu/dynarmic.git
            GIT_TAG a2ef15df6d833ab7bc66131a90350637f26dd2d4
            GIT_SHALLOW FALSE
            GIT_SUBMODULES "externals/mcl" "externals/fmt" "externals/xbyak" "externals/oaknut" "externals/robin-map" "externals/zydis" "externals/zycore"
        )
        FetchContent_MakeAvailable(dynarmic)
        list(APPEND COMPAT_RUNTIME_SOURCES src/compat_runtime/dynarmic_backend.cpp)
    endif()

    add_library(compat_runtime_core STATIC ${COMPAT_RUNTIME_SOURCES})
    set_target_properties(compat_runtime_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(compat_runtime_core PUBLIC include)
    target_compile_options(compat_runtime_core PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(compat_runtime_core PUBLIC Threads::Threads)
    if(RADEK_FETCH_DYNARMIC)
        target_compile_definitions(compat_runtime_core PRIVATE RADEK_HAVE_DYNARMIC=1)
        target_link_libraries(compat_runtime_core PRIVATE dynarmic)
        # The dynarmic target links its externals PRIVATE, so their header
        # roots do not propagate to us even though our backend includes
        # dynarmic public headers (frontend/A32/a32_ir_emitter.h pulls
        # mcl/stdint.hpp). Feed the externals include roots directly.
        target_include_directories(compat_runtime_core PRIVATE
            ${dynarmic_SOURCE_DIR}/externals/mcl/include
            ${dynarmic_SOURCE_DIR}/externals/fmt/include
            ${dynarmic_SOURCE_DIR}/externals/robin-map/include
            ${dynarmic_SOURCE_DIR}/externals/xbyak
            ${dynarmic_SOURCE_DIR}/externals/oaknut/include
        )
        if(Boost_INCLUDE_DIRS)
            target_include_directories(compat_runtime_core PRIVATE ${Boost_INCLUDE_DIRS})
        endif()
        target_include_directories(compat_runtime_core PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost-headers)
    endif()

    if(ANDROID)
        add_library(compat_runtime_v1 SHARED src/compat_runtime/jni.cpp
                                             src/compat_runtime/gameruntime_jni.cpp)
        target_include_directories(compat_runtime_v1 PRIVATE include)
        target_compile_options(compat_runtime_v1 PRIVATE -Wall -Wextra -Werror)
        # libandroid provides ANativeWindow_fromSurface, used to attach the
        # launcher's Surface as the guest's EGL drawable; libaaudio backs the
        # OpenAL mixer's output stream (minSdk 26 ships AAudio).
        target_link_libraries(compat_runtime_v1 PRIVATE compat_runtime_core android aaudio)
        target_link_options(compat_runtime_v1 PRIVATE "-Wl,-z,max-page-size=16384")
    else()
        # Host boot-attempt probe: loads one 32-bit ARM Mach-O, binds traps,
        # and executes guest instructions until the first used missing import.
        add_executable(radek-gameboot src/compat_runtime/gameboot_main.cpp)
        target_include_directories(radek-gameboot PRIVATE include)
        target_compile_options(radek-gameboot PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(radek-gameboot PRIVATE compat_runtime_core)
        add_executable(compat-runtime-tests tests/compat_runtime.cpp)
        target_include_directories(compat-runtime-tests PRIVATE include)
        target_compile_options(compat-runtime-tests PRIVATE -Wall -Wextra -Werror)
        if(RADEK_FETCH_DYNARMIC)
            target_compile_definitions(compat-runtime-tests PRIVATE RADEK_TEST_REQUIRE_DYNARMIC=1)
        endif()
        target_link_libraries(compat-runtime-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime COMMAND compat-runtime-tests)

        add_executable(compat-runtime-libsystem-tests tests/compat_runtime_libsystem.cpp)
        target_include_directories(compat-runtime-libsystem-tests PRIVATE include)
        target_compile_options(compat-runtime-libsystem-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat-runtime-libsystem-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-libsystem COMMAND compat-runtime-libsystem-tests)

        add_executable(compat-runtime-lifecycle-tests tests/compat_runtime_lifecycle.cpp)
        target_include_directories(compat-runtime-lifecycle-tests PRIVATE include)
        target_compile_options(compat-runtime-lifecycle-tests PRIVATE -Wall -Wextra -Werror)
        if(RADEK_FETCH_DYNARMIC)
            target_compile_definitions(compat-runtime-lifecycle-tests PRIVATE RADEK_TEST_REQUIRE_DYNARMIC=1)
        endif()
        target_link_libraries(compat-runtime-lifecycle-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-lifecycle COMMAND compat-runtime-lifecycle-tests)

        add_executable(compat-runtime-darwin-compat-tests tests/darwin_compat.cpp)
        target_include_directories(compat-runtime-darwin-compat-tests PRIVATE include)
        target_compile_options(compat-runtime-darwin-compat-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat-runtime-darwin-compat-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-darwin-compat COMMAND compat-runtime-darwin-compat-tests)

        add_executable(compat-runtime-openal-tests tests/openal_backend.cpp)
        target_include_directories(compat-runtime-openal-tests PRIVATE include)
        target_compile_options(compat-runtime-openal-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat-runtime-openal-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-openal COMMAND compat-runtime-openal-tests)

        add_executable(compat-runtime-chained-fixups-tests tests/macho_chained_fixups.cpp)
        target_include_directories(compat-runtime-chained-fixups-tests PRIVATE include)
        target_compile_options(compat-runtime-chained-fixups-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat-runtime-chained-fixups-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-chained-fixups COMMAND compat-runtime-chained-fixups-tests)

        add_executable(compat-runtime-cxxabi-tests tests/compat_runtime_cxxabi.cpp)
        target_include_directories(compat-runtime-cxxabi-tests PRIVATE include)
        target_compile_options(compat-runtime-cxxabi-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat-runtime-cxxabi-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-cxxabi COMMAND compat-runtime-cxxabi-tests)

        add_executable(compat-runtime-objc-tests tests/compat_runtime_objc.cpp)
        target_include_directories(compat-runtime-objc-tests PRIVATE include)
        target_compile_options(compat-runtime-objc-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat-runtime-objc-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-objc COMMAND compat-runtime-objc-tests)

        # Trap binding/loader coverage needs no CPU backend; execution
        # assertions run when the pinned Dynarmic backend is linked.
        add_executable(compat-runtime-traps-tests tests/compat_runtime_traps.cpp)
        target_include_directories(compat-runtime-traps-tests PRIVATE include)
        target_compile_options(compat-runtime-traps-tests PRIVATE -Wall -Wextra -Werror)
        if(RADEK_FETCH_DYNARMIC)
            target_compile_definitions(compat-runtime-traps-tests PRIVATE RADEK_TEST_REQUIRE_DYNARMIC=1)
        endif()
        target_link_libraries(compat-runtime-traps-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-traps COMMAND compat-runtime-traps-tests)
    endif()
endif()
