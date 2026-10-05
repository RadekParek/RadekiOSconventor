cmake_minimum_required(VERSION 3.22)
project(radek LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
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
option(RADEK_FETCH_UNICORN "Fetch the pinned Unicorn ARM32 backend" OFF)
if(RADEK_BUILD_COMPAT_RUNTIME)
    find_package(Threads REQUIRED)

    set(COMPAT_RUNTIME_SOURCES
        src/compat_runtime/guest_memory.cpp
        src/compat_runtime/shim_registry.cpp
        src/compat_runtime/macho_loader.cpp
        src/compat_runtime/runner.cpp
        src/compat_runtime/cpu.cpp
        src/compat_runtime/objc_runtime.cpp
    )
    if(RADEK_FETCH_UNICORN)
        include(FetchContent)
        set(BUILD_SHARED_LIBS ON CACHE BOOL "Build Unicorn as a shared library" FORCE)
        set(UNICORN_BUILD_TESTS OFF CACHE BOOL "Do not build upstream Unicorn tests" FORCE)
        set(UNICORN_INSTALL OFF CACHE BOOL "Do not install the fetched Unicorn dependency" FORCE)
        set(UNICORN_LEGACY_STATIC_ARCHIVE OFF CACHE BOOL "Do not build the legacy static archive" FORCE)
        set(UNICORN_LOGGING OFF CACHE BOOL "Disable Unicorn internal logging" FORCE)
        set(UNICORN_ARCH "arm" CACHE STRING "Build the ARM32 execution backend only" FORCE)
        FetchContent_Declare(
            unicorn
            GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
            GIT_TAG 8028ec436f2d9376525352dd38ed9ed6b9f6be10
            GIT_SHALLOW FALSE
        )
        FetchContent_MakeAvailable(unicorn)
        set_target_properties(unicorn unicorn-common PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if(ANDROID)
            target_link_options(unicorn PRIVATE "-Wl,-z,max-page-size=16384")
        endif()
        list(APPEND COMPAT_RUNTIME_SOURCES src/compat_runtime/unicorn_backend.cpp)
    endif()

    add_library(compat_runtime_core STATIC ${COMPAT_RUNTIME_SOURCES})
    set_target_properties(compat_runtime_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(compat_runtime_core PUBLIC include)
    target_compile_options(compat_runtime_core PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(compat_runtime_core PUBLIC Threads::Threads)
    if(RADEK_FETCH_UNICORN)
        target_compile_definitions(compat_runtime_core PRIVATE RADEK_HAVE_UNICORN=1)
        target_link_libraries(compat_runtime_core PRIVATE unicorn)
    endif()

    if(ANDROID)
        add_library(compat_runtime_v1 SHARED src/compat_runtime/jni.cpp)
        target_include_directories(compat_runtime_v1 PRIVATE include)
        target_compile_options(compat_runtime_v1 PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat_runtime_v1 PRIVATE compat_runtime_core)
        target_link_options(compat_runtime_v1 PRIVATE "-Wl,-z,max-page-size=16384")
    else()
        add_executable(compat-runtime-tests tests/compat_runtime.cpp)
        target_include_directories(compat-runtime-tests PRIVATE include)
        target_compile_options(compat-runtime-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat-runtime-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime COMMAND compat-runtime-tests)

        add_executable(compat-runtime-objc-tests tests/compat_runtime_objc.cpp)
        target_include_directories(compat-runtime-objc-tests PRIVATE include)
        target_compile_options(compat-runtime-objc-tests PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(compat-runtime-objc-tests PRIVATE compat_runtime_core)
        add_test(NAME compat-runtime-objc COMMAND compat-runtime-objc-tests)
    endif()
endif()
