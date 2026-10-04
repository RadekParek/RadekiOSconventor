package dev.radek.conventor

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
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
    private fun formatBytes(value: Long) = "%.1f MiB".format(java.util.Locale.ROOT, value / 1048576.0)
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
            var changed = false
            if (report.optString("state") in listOf("IMPORTED", "ANALYZING", "CONVERTING", "PACKAGING", "VALIDATING")) {
                val interruptedMessage = "Process ended before work completed; inspect the last saved stage or retry"
                report.put("state", "FAILED").put("error", interruptedMessage)
                report.optJSONObject("analysisProgress")
                    ?.put("status", "FAILED")
                    ?.put("message", interruptedMessage)
                report.optJSONObject("conversionProgress")
                    ?.put("status", "FAILED")
                    ?.put("message", interruptedMessage)
                report.optJSONObject("workflowProgress")
                    ?.put("status", "FAILED")
                    ?.put("message", interruptedMessage)
                File(dir, "extracted").deleteRecursively()
                val savedHash = report.optJSONObject("source")?.optString("sha256").orEmpty()
                if (!savedHash.matches(Regex("[0-9a-f]{64}"))) File(dir, "source.ipa").delete()
                changed = true
            }
            report.optJSONObject("placeholderBuildProgress")
                ?.takeIf { it.optString("status") == "BUILDING" || it.optString("status") == "SIGNING" || it.optString("status") == "VERIFYING" }
                ?.let { placeholder ->
                    placeholder.put("status", "FAILED")
                        .put("percent", 0)
                        .put("message", "Placeholder build was interrupted before it finished; retry Force to rebuild it.")
                    changed = true
                }
            if (changed) save(dir, report)
        }
    }

    private fun sourceDetails(uri: Uri): Pair<String, Long?> {
        val queried = try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { cursor ->
                if (!cursor.moveToFirst()) null else {
                    val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
                    val name = if (nameIndex >= 0) cursor.getString(nameIndex) else null
                    val size = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) cursor.getLong(sizeIndex).takeIf { it >= 0 } else null
                    name to size
                }
            }
        } catch (_: Exception) { null }
        val fallbackName = queried?.first?.takeIf { it.isNotBlank() }
            ?: uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() }
            ?: "converted.ipa"
        return fallbackName to queried?.second
    }

    fun import(uri: Uri, progress: (Int, String) -> Unit): Pair<File, JSONObject> {
        val (originalName, expectedSourceBytes) = sourceDetails(uri)
        val dir = File(root, "${System.currentTimeMillis()}-${UUID.randomUUID()}").apply { check(mkdir()) }
        val events = JSONArray()
        val report = JSONObject()
            .put("schemaVersion", 1)
            .put("authorizationConfirmed", true)
            .put("events", events)
            .put("conversionProgress", JSONObject()
                .put("percent", 0)
                .put("stage", "NOT_STARTED")
                .put("status", "NOT_BUILT")
                .put("message", "No complete-game Android conversion was run during analysis; the separate Force action may create a non-playable placeholder."))
            .put("portProgress", JSONObject()
                .put("percent", 0)
                .put("status", "NO_RUNNABLE_ANDROID_GAME_CODE")
                .put("basis", "No Android game code has been generated. API candidates and analysis progress do not count as playable code."))
        val source = File(dir, "source.ipa")
        var sourceReady = false
        var progressPercent = 0
        var lastPersistedPercent = -1
        var lastUiPercent = -1
        var lastUiAt = 0L
        fun updateProgress(value: Int, stage: String, message: String, forceSave: Boolean = false) {
            progressPercent = maxOf(progressPercent, value.coerceIn(0, 100))
            val progressStatus = when (stage) {
                "PARTIAL", "BLOCKED", "FAILED" -> "COMPLETE"
                else -> "RUNNING"
            }
            report.put("analysisProgress", JSONObject()
                .put("percent", progressPercent)
                .put("stage", stage)
                .put("status", progressStatus)
                .put("message", message)
                .put("updatedAt", java.time.Instant.now().toString()))
            val now = System.currentTimeMillis()
            if (progressPercent != lastUiPercent || now - lastUiAt >= 250L || forceSave) {
                progress(progressPercent, message)
                lastUiPercent = progressPercent
                lastUiAt = now
            }
            if (forceSave || progressPercent != lastPersistedPercent) {
                save(dir, report)
                lastPersistedPercent = progressPercent
            }
        }
        fun log(state: ConversionState, message: String, percent: Int) {
            report.put("state", state.name)
            val event = JSONObject().put("time", java.time.Instant.now().toString()).put("stage", state.name).put("message", message)
            events.put(event); File(dir, "conversion.jsonl").appendText(event.toString() + "\n")
            updateProgress(percent, state.name, message, forceSave = true)
            save(dir, report)
        }
        try {
            log(ConversionState.IMPORTED, "Copying selected IPA into isolated private storage", 0)
            val digest = MessageDigest.getInstance("SHA-256")
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "cannot open selected document" }
                source.outputStream().use { output ->
                    val buffer = ByteArray(65536); var total = 0L
                    while (true) {
                        val n = input.read(buffer); if (n < 0) break
                        total += n; require(total <= SafeZip.MAX_ARCHIVE) { "IPA exceeds 512 MiB" }
                        digest.update(buffer, 0, n); output.write(buffer, 0, n)
                        val copyPercent = expectedSourceBytes?.takeIf { it > 0 }?.let { (total * 5L / it).toInt() } ?: 2
                        updateProgress(copyPercent.coerceAtMost(5), "IMPORTING", "Copying authorized IPA · ${formatBytes(total)}")
                    }
                }
            }
            updateProgress(5, "IMPORTED", "Input staged; checking archive and bundle contents", forceSave = true)
            val hash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
            report.put("source", JSONObject().put("path", "source.ipa").put("originalName", originalName)
                .put("sha256", hash).put("bytes", source.length()))
            save(dir, report)
            sourceReady = true
            SafeZip.extract(
                source,
                File(dir, "extracted"),
                onFile = { done, total ->
                    val percent = if (total > 0) 5 + done * 20 / total else 25
                    updateProgress(percent, "EXTRACTING", "Extracting IPA entries · $done / $total")
                },
                onBytes = { done, total, path ->
                    val percent = if (total > 0) 5 + (done * 20L / total).toInt() else 25
                    updateProgress(percent, "EXTRACTING", "Extracting ${path.substringAfterLast('/')} · ${formatBytes(done)}")
                },
            )
            log(ConversionState.ANALYZING, "Reading Info.plist and application icon", 25)
            val apps = File(dir, "extracted/Payload").listFiles().orEmpty().filter { it.isDirectory && it.name.endsWith(".app") }
            require(apps.size == 1) { "expected exactly one Payload/*.app" }
            val app = apps.single()
            val plistFile = File(app, "Info.plist"); require(plistFile.length() <= 8 * 1024 * 1024)
            val plist = Plist.read(plistFile.readBytes())
            val executable = plist["CFBundleExecutable"] as? String ?: error("CFBundleExecutable missing")
            require(SafeZip.validateName(executable) == executable && '/' !in executable)
            val bundle = plist["CFBundleIdentifier"] as? String ?: error("CFBundleIdentifier missing")
            require(bundle.isNotBlank())
            val minimumIOSVersion = (plist["MinimumOSVersion"] as? String)?.takeIf { it.isNotBlank() }.orEmpty()
            report.put("application", JSONObject().put("name", plist["CFBundleDisplayName"] ?: plist["CFBundleName"] ?: executable)
                .put("bundleId", bundle).put("version", plist["CFBundleShortVersionString"] ?: "")
                .put("build", plist["CFBundleVersion"] ?: "").put("executable", executable)
                .put("minimumIOSVersion", minimumIOSVersion).put("fileSize", source.length()).put("sha256", hash))
            val names = mutableListOf<String>()
            for (key in listOf("CFBundleIcons", "CFBundleIcons~ipad")) {
                val icons = plist[key] as? Map<*, *>
                val primary = icons?.get("CFBundlePrimaryIcon") as? Map<*, *>
                (primary?.get("CFBundleIconName") as? String)?.let { names.add(it) }
                names.addAll((primary?.get("CFBundleIconFiles") as? List<*>)?.filterIsInstance<String>().orEmpty())
            }
            names.addAll((plist["CFBundleIconFiles"] as? List<*>)?.filterIsInstance<String>().orEmpty())
            (plist["CFBundleIconFile"] as? String)?.let { names.add(it) }
            updateProgress(30, "ANALYZING", "Bundle metadata read; recovering the original launcher icon", forceSave = true)
            val iconReport = extractIcon(app, names, dir)
            report.put("icon", iconReport)
            updateProgress(40, "ICON_RECOVERY", if (iconReport.optString("status") == "SUPPORTED") "Recovered the original game icon for the analysis library" else "No compatible icon could be decoded")
            updateProgress(44, "ANALYZING", "Launcher icon recovered; preparing bounded native analysis")
            val binary = File(app, executable)
            require(binary.isFile && binary.length() <= 64 * 1024 * 1024) { "Missing executable or exceeds the on-device 64 MiB analysis limit; use the host analyzer for larger files" }
            log(ConversionState.ANALYZING, "Parsing Mach-O load commands, symbols, fixups and dependencies", 33)
            val macho = JSONObject(NativeBridge.analyze(binary.readBytes()))
            updateProgress(38, "ANALYZING", "Main executable parsed; checking embedded Mach-O images", forceSave = true)
            report.put("machO", macho)
            updateProgress(53, "Mach-O analysis", "Primary executable analysis completed")
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
            updateProgress(45, "ANALYZING", "Dependency inventory complete; cataloging API candidates only", forceSave = true)
            report.put("dependencies", JSONObject().put("nodes", nodes).put("edges", graph))
            val apiMapping = AndroidApiMapper.analyze(nodes)
            report.put("apiMapping", apiMapping)
            report.put("apiTranslation", JSONObject()
                .put("status", "NOT_IMPLEMENTED")
                .put("attempted", false)
                .put("generatedApiReplacements", 0)
                .put("codeGenerated", false)
                .put("message", "Imported symbols and semantic targets are candidates only; no Android API replacement implementation was generated or linked."))
            report.put("portProgress", JSONObject()
                .put("percent", 0)
                .put("status", "NO_RUNNABLE_ANDROID_CODE_BUILT")
                .put("basis", "On-device importer analyzed the IPA but emitted no Android executable code; this is actual output progress, not a stability prediction."))
            log(ConversionState.ANALYZING,
                "Inventoried ${apiMapping.getInt("distinctImportSymbols")} API symbols; ${apiMapping.getInt("mappedNameCandidates")} direct NDK names and ${apiMapping.getInt("semanticRewriteCandidates")} semantic rewrite candidates were not generated or linked.", 50)
            val reason = when {
                encrypted -> "Protected/encrypted Mach-O. Conversion prohibited; no DRM or FairPlay bypass."
                !hasCandidate -> "No supported ARM64/ARMv7/ARMv6 slice. ARM64e PAC reconstruction is blocked."
                incompatible -> "Frameworks, imports, incomplete dyld bindings, metadata or embedded code require unsupported compatibility/linker implementations."
                else -> "Analysis completed, but complete iOS-to-Android game-code translation, API replacement, and packaging are not implemented. No playable game APK can be produced from this analysis; Force can build a separate branded placeholder."
            }
            report.put("blockers", JSONArray().put(reason)).put("hostCommand", "python3 -m radek analyze input.ipa --authorized --output workspace/analysis")
            val terminalState = if (encrypted || incompatible || !hasCandidate) ConversionState.BLOCKED else ConversionState.PARTIAL
            report.put("conversionProgress", JSONObject()
                .put("percent", 0)
                .put("stage", "NOT_BUILT")
                .put("status", "NOT_BUILT")
                .put("message", reason)
                .put("basis", "No game code is translated during IPA analysis. A user-triggered placeholder is tracked separately and is not counted as Android game-code progress."))
            log(terminalState, reason, 100)
            save(dir, report)
        } catch (e: Exception) {
            report.put("error", "${e.javaClass.simpleName}: ${e.message}")
            log(ConversionState.FAILED, e.message ?: "Import failed", progressPercent)
        } finally {
            if (!sourceReady) source.delete()
            File(dir, "extracted").deleteRecursively()
        }
        return dir to report
    }
}

/** Icon suffixes, highest scale first: the best available representation wins. */
private val ICON_SUFFIXES = listOf(
    "@3x.png", "@2x.png", ".png", "@3x~ipad.png", "@2x~ipad.png", "~ipad.png",
    "@3x~iphone.png", "@2x~iphone.png", "~iphone.png",
    "@3x.jpg", "@2x.jpg", ".jpg", "@3x.jpeg", "@2x.jpeg", ".jpeg", ""
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

    // Info.plist names are authoritative. If a name includes an extension, try
    // scale variants of its basename (Icon.png -> Icon@3x.png) before the exact
    // fallback. This also keeps icon choice independent of the Mach-O CPU slices.
    for (rawName in names) {
        val name = rawName.trim()
        if (name.isEmpty()) continue
        SafeZip.validateName(name) // fail closed on traversal or absolute names
        val lower = name.lowercase(java.util.Locale.ROOT)
        val extension = listOf(".png", ".jpg", ".jpeg").firstOrNull { lower.endsWith(it) }
        val base = if (extension == null) name else name.dropLast(extension.length)
        for (suffix in ICON_SUFFIXES) addCandidate(File(app, base + suffix))
        addCandidate(File(app, name))
    }

    fun saveBitmap(
        bitmap: android.graphics.Bitmap,
        source: String,
        format: String,
        decoder: String,
        scale: Double,
        kind: String,
    ): JSONObject {
        val width = bitmap.width
        val height = bitmap.height
        File(dir, "icon.png").outputStream().use {
            require(bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it)) { "cannot save recovered icon" }
        }
        bitmap.recycle()
        return JSONObject().put("status", "SUPPORTED")
            .put("source", source).put("path", "icon.png").put("kind", kind)
            .put("format", format).put("decoder", decoder)
            .put("width", width).put("height", height).put("scale", scale)
            .put("reason", "Decoded bundle icon ($source, ${width}x${height})")
            .put("attempts", attempts)
    }

    fun tryFiles(files: List<File>): JSONObject? {
        for (candidate in files.take(48)) {
            val relative = candidate.relativeTo(app).path
            if (candidate.length() > ICON_MAX_BYTES) {
                attempt(relative, false, "image exceeds size limit")
                continue
            }
            val applePng = IconDecoder.isCgbi(candidate)
            val bitmap = try { IconDecoder.decode(candidate, ICON_TARGET) } catch (_: Exception) { null }
            if (bitmap == null) {
                attempt(relative, false, "unsupported or corrupt image; tried Android PNG/JPEG and Apple CgBI decoders")
                continue
            }
            attempt(relative, true, if (applePng) "decoded and normalized Apple CgBI channel order/alpha" else "decoded image", bitmap.width, bitmap.height)
            val scale = when { "@3x" in candidate.name -> 3.0; "@2x" in candidate.name -> 2.0; else -> 1.0 }
            val format = when {
                applePng -> "cgbi-png"
                candidate.name.endsWith(".jpeg", true) -> "jpeg"
                candidate.name.endsWith(".jpg", true) -> "jpeg"
                else -> "png"
            }
            val decoder = if (applePng) "radek-cgbi+android.graphics.Bitmap" else "android.graphics.BitmapFactory"
            return saveBitmap(bitmap, relative, format, decoder, scale, "file")
        }
        return null
    }

    // Prefer the explicit bundle icon, but tolerate a broken or missing file.
    // This pass occurs before reading the executable, so arm32, arm64 and FAT
    // archives all follow the same icon path.
    tryFiles(candidates)?.let { return it }

    // Modern iOS games commonly keep their only app icon in a compiled asset
    // catalog. Try it before arbitrary bundle textures/screenshots.
    val preferred = names.firstOrNull()?.substringBeforeLast('.', names.firstOrNull().orEmpty())
    val catalogs = app.walkTopDown().filter {
        it.isFile && (it.name.equals("Assets.car", ignoreCase = true) || it.extension.equals("car", ignoreCase = true))
    }.sortedWith(compareBy<File>({ if (it.name.equals("Assets.car", ignoreCase = true)) 0 else 1 }, { it.path }))
    for (catalog in catalogs.take(8)) {
        val extracted = AssetCatalogIcon.extract(catalog, preferred, ICON_TARGET)
        val prefix = catalog.relativeTo(app).path
        for (item in extracted.attempts) {
            attempt("$prefix:${item.asset}", item.ok, item.detail)
        }
        if (extracted.bitmap != null) {
            val source = extracted.asset ?: prefix
            attempt(source, true, "decoded compiled asset-catalog rendition", extracted.bitmap.width, extracted.bitmap.height)
            return saveBitmap(
                extracted.bitmap,
                source,
                extracted.format ?: "asset-catalog-image",
                "assetcatalog+android.graphics.Bitmap",
                extracted.scale,
                "assets.car",
            )
        }
        attempt(prefix, false, extracted.error ?: "no usable icon rendition")
    }

    // Some games ship the launch artwork as an unlisted loose resource. Rank
    // icon-like filenames above backgrounds and generic textures, then try all
    // image files. `iTunesArtwork` is often extensionless, so the decoder sniffs
    // its actual file signature rather than trusting the suffix.
    val fallbackImages = app.walkTopDown()
        .filter {
            it.isFile && (it.extension.lowercase() in setOf("png", "jpg", "jpeg") || it.name.equals("iTunesArtwork", true))
        }
        .sortedWith(compareBy<File>({
            val lower = it.name.lowercase()
            when {
                "appicon" in lower || lower.startsWith("itunesartwork") -> 0
                "icon" in lower -> 1
                "artwork" in lower || "logo" in lower -> 2
                else -> 3
            }
        }, { -it.length() }, { it.path }))
        .toList()
    tryFiles(fallbackImages)?.let { return it }

    return JSONObject().put("status", "UNAVAILABLE")
        .put("reason", "Icon unavailable: no decodable icon image was found in the bundle")
        .put("decoder", "android.graphics.BitmapFactory")
        .put("attempts", attempts)
}
