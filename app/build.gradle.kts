import java.io.File
import java.util.zip.ZipFile

plugins { id("com.android.application"); id("org.jetbrains.kotlin.android") }

android {
    namespace = "dev.radek.conventor"
    compileSdk = 35
    ndkVersion = "27.2.12479018"
    defaultConfig {
        applicationId = "dev.radek.conventor"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { cppFlags += listOf("-std=c++17", "-Wall", "-Wextra") } }
    }
    externalNativeBuild { cmake { path = file("../native/CMakeLists.txt"); version = "3.22.1" } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
    testOptions { unitTests.isIncludeAndroidResources = true }
    buildTypes { release { isMinifyEnabled = false } }
}

fun registerTemplateEmbedTask(module: String, assetDirectory: String, iconFileName: String): TaskProvider<*> {
    val templateApkDirectory = project(module).layout.buildDirectory.dir("outputs/apk/debug")
    val generatedAssets = layout.buildDirectory.dir("generated/assets/$assetDirectory")
    return tasks.register("embed${assetDirectory.replace("-", "").replaceFirstChar { it.uppercase() }}") {
        dependsOn("$module:assembleDebug")
        inputs.dir(templateApkDirectory)
        outputs.dir(generatedAssets)
        doLast {
            val apks = templateApkDirectory.get().asFile.listFiles()
                .orEmpty().filter { it.isFile && it.extension.equals("apk", ignoreCase = true) }
            require(apks.size == 1) { "expected one $assetDirectory template APK, found ${apks.size}" }
            val assetRoot = generatedAssets.get().dir(assetDirectory).asFile
            assetRoot.deleteRecursively()
            require(assetRoot.mkdirs()) { "cannot create generated $assetDirectory assets" }
            val entries = mapOf(
                "AndroidManifest.xml" to "AndroidManifest.xml",
                "resources.arsc" to "resources.arsc",
                "classes.dex" to "classes.dex",
            )
            ZipFile(apks.single()).use { template ->
                entries.forEach { (entryName, assetName) ->
                    val entry = template.getEntry(entryName) ?: error("$assetDirectory template missing $entryName")
                    require(entry.size in 1..(32L * 1024 * 1024)) { "$assetDirectory template entry too large: $entryName" }
                    val destination = File(assetRoot, assetName)
                    template.getInputStream(entry).use { input -> destination.outputStream().use { output -> input.copyTo(output) } }
                }
                val iconEntries = template.entries().asSequence().filter {
                    it.name.startsWith("res/") && it.name.substringAfterLast('/') == iconFileName
                }.toList()
                require(iconEntries.size == 1) { "expected one $assetDirectory template launcher icon, found ${iconEntries.size}" }
                val iconEntry = iconEntries.single()
                require(iconEntry.size in 1..(32L * 1024 * 1024)) { "$assetDirectory template icon too large" }
                val iconDestination = File(assetRoot, "fallback-icon.png")
                template.getInputStream(iconEntry).use { input -> iconDestination.outputStream().use { output -> input.copyTo(output) } }
                File(assetRoot, "icon-entry-path.txt").writeText(iconEntry.name)
            }
        }
    }.also { task ->
        android.sourceSets.getByName("main").assets.srcDir(generatedAssets)
        tasks.named("preBuild").configure { dependsOn(task) }
    }
}

val embedPlaceholderTemplate = registerTemplateEmbedTask(":placeholder-template", "placeholder-template", "generated_placeholder_icon.png")
val embedConvertedTemplate = registerTemplateEmbedTask(":converted-template", "converted-template", "generated_converted_icon.png")

// Surface full assertion messages and test stdout in the CI console; the default
// logging prints only the exception class and source line, which hides values.
tasks.withType<Test> {
    testLogging {
        events("failed")
        exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
        showStandardStreams = true
    }
}

dependencies {
    implementation("com.android.tools.build:apksig:8.7.3")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
}
