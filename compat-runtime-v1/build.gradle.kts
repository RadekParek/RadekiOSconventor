plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.radek.compat.runtime"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-Wall", "-Wextra")
                arguments += listOf(
                    "-DRADEK_BUILD_COMPAT_RUNTIME=ON",
                    "-DRADEK_FETCH_UNICORN=ON",
                )
                // Reuse the one cached pinned Unicorn checkout for both CMake
                // consumers. FETCHCONTENT_SOURCE_DIR_UNICORN bypasses
                // FetchContent's populate step entirely (no download, no
                // per-consumer subbuild), so the host Makefiles build and this
                // Ninja/NDK build cannot collide in a shared base directory.
                // Local builds without the env var keep the default behavior.
                System.getenv("RADEK_UNICORN_SOURCE_DIR")?.let { srcDir ->
                    if (srcDir.isNotEmpty()) {
                        arguments += listOf("-DFETCHCONTENT_SOURCE_DIR_UNICORN=$srcDir")
                    }
                }
                // Opt-in compiler cache for CI and repeat local builds; without
                // RADEK_USE_CCACHE=1 nothing changes for existing environments.
                if (System.getenv("RADEK_USE_CCACHE") == "1") {
                    arguments += listOf(
                        "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
                    )
                }
                // Unicorn is a shared CMake dependency. Declare it as a build target too so AGP
                // packages libunicorn.so into the AAR; the generated game APK needs it beside
                // libcompat_runtime_v1.so at runtime.
                targets += listOf("compat_runtime_v1", "unicorn")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildTypes { release { isMinifyEnabled = false } }
}

dependencies { }
