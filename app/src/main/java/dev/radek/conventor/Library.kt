package dev.radek.conventor

import android.content.Context
import android.net.Uri
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.UUID

object NativeBridge {
    init { System.loadLibrary("radek") }
    external fun analyze(bytes: ByteArray): String
}

enum class ConversionState { IMPORTED, ANALYZING, CONVERTING, PACKAGING, VALIDATING, READY, PARTIAL, BLOCKED, FAILED }

/** Persistent private library. Android imports/analyzes; compilation uses the host CLI. */
class Library(private val context: Context) {
    val root = File(context.filesDir, "library").apply { mkdirs() }
    fun entries(): List<Pair<File, JSONObject>> = root.listFiles().orEmpty().filter { it.isDirectory }.mapNotNull { dir ->
        try { dir to JSONObject(File(dir, "report.json").readText()) } catch (_: Exception) { null }
    }.sortedByDescending { it.first.name }

    fun save(dir: File, report: JSONObject) {
        val temporary = File(dir, "report.json.tmp")
        temporary.writeText(report.toString(2))
        require(temporary.renameTo(File(dir, "report.json"))) { "cannot persist report" }
    }

    fun recoverInterrupted() {
        root.listFiles().orEmpty().filter { it.isDirectory && !File(it, "report.json").isFile }.forEach { it.deleteRecursively() }
        entries().forEach { (dir, report) ->
            if (report.optString("state") in listOf("IMPORTED", "ANALYZING", "CONVERTING", "PACKAGING", "VALIDATING")) {
                report.put("state", "FAILED").put("error", "Process ended before work completed; reimport to retry")
                File(dir, "source.ipa").delete(); File(dir, "extracted").deleteRecursively()
                save(dir, report)
            }
        }
    }

    fun import(uri: Uri, progress: (String) -> Unit): Pair<File, JSONObject> {
        val dir = File(root, "${System.currentTimeMillis()}-${UUID.randomUUID()}").apply { check(mkdir()) }
        val events = JSONArray()
        val report = JSONObject().put("schemaVersion", 1).put("authorizationConfirmed", true).put("events", events)
        fun log(state: ConversionState, message: String) {
            report.put("state", state.name)
            val event = JSONObject().put("time", java.time.Instant.now().toString()).put("stage", state.name).put("message", message)
            events.put(event); File(dir, "conversion.jsonl").appendText(event.toString() + "\n")
            save(dir, report); progress(message)
        }
        try {
            log(ConversionState.IMPORTED, "Copying selected IPA into isolated private storage")
            val source = File(dir, "source.ipa")
            val digest = MessageDigest.getInstance("SHA-256")
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "cannot open selected document" }
                source.outputStream().use { output ->
                    val buffer = ByteArray(65536); var total = 0L
                    while (true) { val n = input.read(buffer); if (n < 0) break; total += n; require(total <= SafeZip.MAX_ARCHIVE) { "IPA exceeds 512 MiB" }; digest.update(buffer, 0, n); output.write(buffer, 0, n) }
                }
            }
            val hash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
            SafeZip.extract(source, File(dir, "extracted")) { done, total -> if (done == total || done % 100 == 0) progress("Extracted $done / $total archive entries") }
            log(ConversionState.ANALYZING, "Reading Info.plist and application icon")
            val apps = File(dir, "extracted/Payload").listFiles().orEmpty().filter { it.isDirectory && it.name.endsWith(".app") }
            require(apps.size == 1) { "expected exactly one Payload/*.app" }
            val app = apps.single()
            val plistFile = File(app, "Info.plist"); require(plistFile.length() <= 8 * 1024 * 1024)
            val plist = Plist.read(plistFile.readBytes())
            val executable = plist["CFBundleExecutable"] as? String ?: error("CFBundleExecutable missing")
            require(SafeZip.validateName(executable) == executable && '/' !in executable)
            val bundle = plist["CFBundleIdentifier"] as? String ?: error("CFBundleIdentifier missing")
            require(bundle.isNotBlank())
            report.put("application", JSONObject().put("name", plist["CFBundleDisplayName"] ?: plist["CFBundleName"] ?: executable)
                .put("bundleId", bundle).put("version", plist["CFBundleShortVersionString"] ?: "")
                .put("build", plist["CFBundleVersion"] ?: "").put("executable", executable).put("fileSize", source.length()).put("sha256", hash))
            val names = mutableListOf<String>()
            for (key in listOf("CFBundleIcons", "CFBundleIcons~ipad")) {
                val icons = plist[key] as? Map<*, *>
                val primary = icons?.get("CFBundlePrimaryIcon") as? Map<*, *>
                (primary?.get("CFBundleIconName") as? String)?.let { names.add(it) }
                names.addAll((primary?.get("CFBundleIconFiles") as? List<*>)?.filterIsInstance<String>().orEmpty())
            }
            names.addAll((plist["CFBundleIconFiles"] as? List<*>)?.filterIsInstance<String>().orEmpty())
            (plist["CFBundleIconFile"] as? String)?.let { names.add(it) }
            val iconReport = extractIcon(app, names, dir)
            report.put("icon", iconReport)
            val binary = File(app, executable)
            require(binary.isFile && binary.length() <= 64 * 1024 * 1024) { "Missing executable or exceeds the on-device 64 MiB analysis limit; use the host analyzer for larger files" }
            log(ConversionState.ANALYZING, "Parsing Mach-O load commands, symbols, fixups and dependencies")
            val macho = JSONObject(NativeBridge.analyze(binary.readBytes()))
            report.put("machO", macho)
            val graph = JSONArray(); val nodes = JSONArray()
            var encrypted = false; var incompatible = false
            var hasCandidate = false
            fun inspect(file: File, analysis: JSONObject) {
                nodes.put(JSONObject().put("path", file.relativeTo(app).path).put("analysis", analysis))
                val slices = analysis.getJSONArray("slices")
                for (index in 0 until slices.length()) {
                    val slice = slices.getJSONObject(index)
                    encrypted = encrypted || slice.getBoolean("encrypted")
                    val arch = slice.getString("architecture")
                    if (file == binary && arch in listOf("arm64", "armv7", "armv7s", "armv6")) hasCandidate = true
                    val deps = slice.getJSONArray("dependencies")
                    for (d in 0 until deps.length()) graph.put(JSONObject().put("from", file.relativeTo(app).path)
                        .put("installName", deps.getJSONObject(d).getString("path")).put("classification", "unverified")
                        .put("reason", "Linked dependency; this on-device report cannot prove API reachability and ships no Darwin ABI provider"))
                    incompatible = incompatible || deps.length() > 0 || slice.getJSONArray("imports").length() > 0 || slice.getJSONArray("metadata").length() > 0 || slice.has("chainedFixups") || !slice.optBoolean("bindDecodingComplete", true)
                }
            }
            inspect(binary, macho)
            val magics = setOf("cffaedfe", "cefaedfe", "feedface", "feedfacf", "cafebabe", "cafebabf", "bebafeca", "bfbafeca")
            app.walkTopDown().filter { it.isFile && it != binary }.forEach { file ->
                val head = ByteArray(4)
                val size = file.inputStream().use { it.read(head) }
                if (size == 4 && head.joinToString("") { "%02x".format(it.toInt() and 255) } in magics) {
                    require(file.length() <= 64 * 1024 * 1024) { "Embedded executable exceeds 64 MiB on-device limit" }
                    inspect(file, JSONObject(NativeBridge.analyze(file.readBytes())))
                    incompatible = true
                }
            }
            report.put("dependencies", JSONObject().put("nodes", nodes).put("edges", graph))
            val reason = when {
                encrypted -> "Protected/encrypted Mach-O. Conversion prohibited; no DRM or FairPlay bypass."
                !hasCandidate -> "No supported ARM64/ARMv7/ARMv6 slice. ARM64e PAC reconstruction is blocked."
                incompatible -> "Frameworks, imports, incomplete dyld bindings, metadata or embedded code require unsupported compatibility/linker implementations."
                else -> "Analysis completed. A host SDK/NDK is required to prove the restricted leaf subset, reconstruct native code and package an APK. On-device compilation is not implemented."
            }
            report.put("blockers", JSONArray().put(reason)).put("hostCommand", "python3 -m radek convert input.ipa --authorized --output workspace/result")
            log(if (encrypted || incompatible || !hasCandidate) ConversionState.BLOCKED else ConversionState.PARTIAL, reason)
        } catch (e: Exception) {
            report.put("error", "${e.javaClass.simpleName}: ${e.message}")
            log(ConversionState.FAILED, e.message ?: "Import failed")
        } finally {
            File(dir, "source.ipa").delete(); File(dir, "extracted").deleteRecursively()
        }
        return dir to report
    }
}

/** Icon suffixes, highest scale first: the best available representation wins. */
private val ICON_SUFFIXES = listOf(
    "@3x.png", "@2x.png", ".png", "@3x~ipad.png", "@2x~ipad.png", "~ipad.png",
    "@3x~iphone.png", "@2x~iphone.png", "~iphone.png", "", "@3x.jpg", "@2x.jpg", ".jpg"
)
private const val ICON_MAX_BYTES = 16L * 1024 * 1024
private const val ICON_TARGET = 512

/**
 * Resolve the best icon in a bundle and record every attempt.
 * Order: Info.plist names -> scale/device variants -> icon-like bundle images ->
 * any other image. Nothing is invented: when nothing decodes the status is
 * UNAVAILABLE and the library shows that state.
 */
internal fun extractIcon(app: File, names: List<String>, dir: File): JSONObject {
    val attempts = JSONArray()
    fun attempt(source: String, ok: Boolean, detail: String, width: Int = 0, height: Int = 0) {
        attempts.put(JSONObject().put("source", source).put("ok", ok).put("detail", detail)
            .put("width", width).put("height", height))
    }
    val candidates = mutableListOf<File>()
    val seen = mutableSetOf<String>()
    fun addCandidate(file: File) {
        if (!file.isFile) return
        val key = file.relativeTo(app).path.lowercase(java.util.Locale.ROOT)
        if (seen.add(key)) candidates.add(file)
    }
    for (name in names) {
        SafeZip.validateName(name) // fail closed on traversal or absolute names
        for (suffix in ICON_SUFFIXES) addCandidate(File(app, name + suffix))
    }
    // A declared icon is only a preference: games often ship a broken/unsupported
    // plist rendition alongside a perfectly usable loose PNG/JPEG. Always try the
    // ranked bundle fallback after declared candidates, not only when names are absent.
    val fallbackImages = app.walkTopDown()
        .filter { it.isFile && it.extension.lowercase() in setOf("png", "jpg", "jpeg") }
        .sortedWith(compareBy<File>({
            val lower = it.name.lowercase()
            when {
                "appicon" in lower || lower.startsWith("itunesartwork") -> 0
                "icon" in lower -> 1
                "artwork" in lower || "logo" in lower -> 2
                else -> 3
            }
        }, { -it.length() }))
    for (file in fallbackImages) addCandidate(file)

    var best: Pair<File, android.graphics.Bitmap>? = null
    var bestPixels = 0
    var bestScale = 0
    var bestDecoder = "android.graphics.BitmapFactory"
    for (candidate in candidates.take(48)) {
        val relative = candidate.relativeTo(app).path
        if (candidate.length() > ICON_MAX_BYTES) { attempt(relative, false, "image exceeds size limit"); continue }
        var decodeError: String? = null
        val decoded: Pair<Boolean, android.graphics.Bitmap?> = try {
            IconDecoder.isCgbi(candidate) to IconDecoder.decode(candidate, ICON_TARGET)
        } catch (e: Exception) {
            decodeError = e.message
            false to null
        }
        val applePng = decoded.first
        val bitmap = decoded.second
        if (bitmap == null) {
            attempt(relative, false, decodeError?.let { "image decoder rejected candidate: $it" } ?: "unsupported or corrupt image; tried PNG/JPEG and Apple CgBI decoders")
            continue
        }
        val pixels = bitmap.width * bitmap.height
        val scale = when { "@3x" in candidate.name -> 3; "@2x" in candidate.name -> 2; else -> 1 }
        val decoder = if (applePng) "radek-cgbi+android.graphics.Bitmap" else "android.graphics.BitmapFactory"
        attempt(relative, true, if (applePng) "decoded and normalized Apple CgBI channel order/alpha" else "decoded", bitmap.width, bitmap.height)
        if (pixels > bestPixels || (pixels == bestPixels && scale > bestScale)) {
            best?.second?.recycle()
            best = candidate to bitmap
            bestPixels = pixels
            bestScale = scale
            bestDecoder = decoder
        } else bitmap.recycle()
    }
    val chosen = best
    if (chosen == null) {
        return JSONObject().put("status", "UNAVAILABLE")
            .put("reason", "Icon unavailable: no decodable icon image was found in this bundle")
            .put("decoder", "android.graphics.BitmapFactory")
            .put("attempts", attempts)
    }
    val (file, bitmap) = chosen
    val width = bitmap.width
    val height = bitmap.height
    File(dir, "icon.png").outputStream().use { require(bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it)) }
    val scale = when { "@3x" in file.name -> 3.0; "@2x" in file.name -> 2.0; else -> 1.0 }
    val detail = "Decoded bundle icon (${file.relativeTo(app).path}, ${width}x${height})"
    bitmap.recycle()
    return JSONObject().put("status", "SUPPORTED")
        .put("source", file.relativeTo(app).path)
        .put("path", "icon.png")
        .put("kind", "file")
        .put("format", file.extension.lowercase())
        .put("decoder", bestDecoder)
        .put("width", width).put("height", height)
        .put("scale", scale)
        .put("reason", detail)
        .put("attempts", attempts)
}
