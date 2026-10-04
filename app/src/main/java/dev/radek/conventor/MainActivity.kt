package dev.radek.conventor

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.widget.*
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors
import java.util.zip.ZipFile

private object Jobs {
    private val executor = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    @Volatile var busy = false
    @Volatile var percent = 0
    @Volatile var title = "Working"
    @Volatile var message = ""
    var listener: (() -> Unit)? = null
    fun begin(jobTitle: String) { title = jobTitle; percent = 0; message = "Preparing" }
    fun update(value: Int, text: String) {
        percent = value.coerceIn(0, 100)
        message = text
        main.post { listener?.invoke() }
    }
    fun update(text: String) = update(percent, text)
    @Synchronized fun run(block: () -> Unit) {
        check(!busy) { "A job is already running" }; busy = true
        executor.execute { try { block() } catch (e: Exception) { update(percent, "Failed at $percent%: ${e.message}") } finally { busy = false; main.post { listener?.invoke() } } }
    }
}

class MainActivity : Activity() {
    private var bgColor = Color.rgb(11, 16, 29)
    private var panel = Color.rgb(20, 29, 47)
    private var muted = Color.rgb(160, 178, 199)
    private var accent = Color.rgb(92, 227, 181)
    private var textColor = Color.WHITE
    private var outlineColor = Color.rgb(45, 59, 79)
    private var lightMode = false
    private val preferences by lazy { getSharedPreferences("radek_settings", MODE_PRIVATE) }
    private lateinit var library: Library
    private lateinit var body: LinearLayout
    private var selected: File? = null
    private var progressLabel: TextView? = null
    private var progressBar: ProgressBar? = null
    private var wasBusy = false
    private var returnToDetailAfterJob: File? = null
    private var pendingInstall: Pair<File, String>? = null
    private val pickerIpa = 100
    private val pickerApk = 101
    private fun dp(n: Int) = (n * resources.displayMetrics.density).toInt()
    private fun applyThemePalette() {
        lightMode = preferences.getBoolean("light_theme", false)
        if (lightMode) {
            bgColor = Color.rgb(245, 247, 250)
            panel = Color.WHITE
            muted = Color.rgb(92, 104, 119)
            accent = Color.rgb(0, 125, 112)
            textColor = Color.rgb(22, 32, 46)
            outlineColor = Color.rgb(220, 226, 234)
        } else {
            bgColor = Color.rgb(11, 16, 29)
            panel = Color.rgb(20, 29, 47)
            muted = Color.rgb(160, 178, 199)
            accent = Color.rgb(92, 227, 181)
            textColor = Color.WHITE
            outlineColor = Color.rgb(45, 59, 79)
        }
        window.statusBarColor = bgColor
        window.navigationBarColor = bgColor
        window.decorView.systemUiVisibility = if (lightMode) {
            View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
        } else 0
    }

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        applyThemePalette()
        library = Library(applicationContext)
        if (!Jobs.busy) library.recoverInterrupted()
        selected = state?.getString("selected")?.let { File(library.root, it) }
        Jobs.listener = {
            progressLabel?.text = "${Jobs.percent}% · ${Jobs.message}"
            progressBar?.progress = Jobs.percent
            if (wasBusy && !Jobs.busy) {
                wasBusy = false
                val target = returnToDetailAfterJob
                returnToDetailAfterJob = null
                if (target != null && target.isDirectory) detail(target) else home()
            }
        }
        if (selected != null && File(selected, "report.json").isFile) detail(selected!!) else home()
    }
    override fun onSaveInstanceState(out: Bundle) { super.onSaveInstanceState(out); out.putString("selected", selected?.name) }
    override fun onDestroy() { Jobs.listener = null; super.onDestroy() }
    override fun onResume() {
        super.onResume()
        if (Jobs.busy) {
            wasBusy = true
            progressLabel?.text = "${Jobs.percent}% · ${Jobs.message}"
            progressBar?.progress = Jobs.percent
        }
        val pending = pendingInstall
        if (pending != null && packageManager.canRequestPackageInstalls()) {
            pendingInstall = null
            installArtifact(pending.first, pending.second)
        }
    }

    private fun rounded(color: Int): GradientDrawable = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(18).toFloat()
        setStroke(dp(1), outlineColor)
    }
    private fun screen() {
        progressLabel = null
        progressBar = null
        val scroll = ScrollView(this).apply { setBackgroundColor(bgColor); isFillViewport = true }
        body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(22), dp(30), dp(22), dp(36)) }
        scroll.addView(body); setContentView(scroll)
    }
    private fun text(value: String, size: Float = 16f, color: Int = textColor, bold: Boolean = false, parent: LinearLayout = body): TextView = TextView(this).apply {
        text = value; textSize = size; setTextColor(color); if (bold) setTypeface(typeface, Typeface.BOLD)
        setPadding(0, dp(6), 0, dp(7)); parent.addView(this)
    }
    private fun button(label: String, primary: Boolean = false, parent: LinearLayout = body, action: () -> Unit): Button = Button(this).apply {
        text = label; isAllCaps = false; textSize = 15f; setTextColor(if (primary) bgColor else accent)
        background = android.graphics.drawable.RippleDrawable(
            android.content.res.ColorStateList.valueOf(if (primary) Color.WHITE else accent),
            rounded(if (primary) accent else panel), null,
        )
        elevation = dp(1).toFloat()
        parent.addView(this, LinearLayout.LayoutParams(-1, dp(54)).apply { topMargin = dp(10); bottomMargin = dp(3) })
        setOnClickListener { action() }
    }
    private fun dangerButton(label: String, action: () -> Unit): Button = Button(this).apply {
        text = label; isAllCaps = false; textSize = 15f; setTextColor(Color.WHITE)
        background = android.graphics.drawable.RippleDrawable(
            android.content.res.ColorStateList.valueOf(Color.WHITE), rounded(Color.rgb(189, 52, 58)), null,
        )
        elevation = dp(2).toFloat()
        body.addView(this, LinearLayout.LayoutParams(-1, dp(54)).apply { topMargin = dp(12); bottomMargin = dp(4) })
        setOnClickListener { action() }
    }
    private fun progressIndicator(parent: LinearLayout, value: Int, height: Int = 8) {
        val indicator = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            progress = value.coerceIn(0, 100)
            progressTintList = android.content.res.ColorStateList.valueOf(accent)
            progressBackgroundTintList = android.content.res.ColorStateList.valueOf(outlineColor)
            contentDescription = "IPA analysis or validated host conversion progress ${progress}%"
        }
        parent.addView(indicator, LinearLayout.LayoutParams(-1, dp(height)).apply { topMargin = dp(4); bottomMargin = dp(4) })
    }
    private fun addJobProgressCard(parent: LinearLayout = body) {
        val jobCard = card(parent)
        text(Jobs.title, 17f, textColor, true, jobCard)
        val bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            progress = Jobs.percent
            progressTintList = android.content.res.ColorStateList.valueOf(accent)
        }
        progressBar = bar
        jobCard.addView(bar, LinearLayout.LayoutParams(-1, dp(12)).apply { topMargin = dp(8); bottomMargin = dp(4) })
        progressLabel = text("${Jobs.percent}% · ${Jobs.message}", 13f, muted, parent = jobCard)
    }
    private fun card(parent: LinearLayout = body): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL; background = rounded(panel); setPadding(dp(18), dp(14), dp(18), dp(16))
        parent.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(14) })
    }
    private fun statusColor(state: String) = when (state) {
        "READY" -> accent
        "FAILED", "BLOCKED" -> if (lightMode) Color.rgb(179, 38, 30) else Color.rgb(255, 157, 139)
        else -> if (lightMode) Color.rgb(145, 83, 0) else Color.rgb(245, 203, 116)
    }
    private fun formatBytes(n: Long) = "%.1f MiB".format(n / 1048576.0)
    private fun home() {
        selected = null; screen()
        text("RADEK  /  IPA CONVERTER", 11f, accent, true)
        text("IPA to Android", 30f, textColor, true)
        text("Import an IPA to automatically inspect its code and Android compatibility.", 15f, muted)
        val info = card()
        text("Complete game conversion unavailable", 17f, textColor, true, info)
        text("The on-device app analyzes the IPA but does not translate iOS code or replace iOS APIs. Analysis does not emit a game APK. For an imported app you own or may convert, Force can separately build a signed, installable placeholder branded with its name and recovered icon; it contains no translated game code and is not playable. Host APKs remain accepted only when they declare a complete game conversion and pass provenance and package checks.", 14f, muted, parent = info)
        val add = button("Choose IPA", true) { authorize() }; add.isEnabled = !Jobs.busy
        button("Settings", parent = body) { settingsScreen() }.isEnabled = !Jobs.busy
        if (Jobs.busy) {
            wasBusy = true
            addJobProgressCard()
        } else if (Jobs.message.startsWith("Failed")) text(Jobs.message, 14f, statusColor("FAILED"))
        text("Game Library", 23f, textColor, true)
        val entries = library.entries()
        text("${entries.size} imported ${if (entries.size == 1) "application" else "applications"} · private device storage", 12f, muted)
        if (entries.isEmpty()) {
            val empty = card(); text("Your library starts here", 19f, textColor, true, empty)
            text("Select an .ipa you own or have permission to convert. Encrypted and FairPlay-protected binaries are never decrypted.", 14f, muted, parent = empty)
        }
        entries.forEach { (dir, report) ->
            val item = card(); val app = report.optJSONObject("application") ?: JSONObject()
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }; item.addView(row)
            val iconPath = File(dir, "icon.png")
            val iconBitmap = if (iconPath.isFile) android.graphics.BitmapFactory.decodeFile(iconPath.path) else null
            val icon = ImageView(this).apply {
                if (iconBitmap != null) setImageBitmap(iconBitmap) else setImageResource(dev.radek.conventor.R.drawable.ic_launcher)
                contentDescription = if (iconBitmap != null) "Application icon"
                else report.optJSONObject("icon")?.optString("reason")?.takeIf { it.isNotBlank() } ?: "Icon unavailable"
            }
            row.addView(icon, LinearLayout.LayoutParams(dp(56), dp(56)).apply { rightMargin = dp(14) })
            val labels = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }; row.addView(labels, LinearLayout.LayoutParams(0, -2, 1f))
            text(app.optString("name", "Import failed"), 19f, textColor, true, labels)
            text(app.optString("bundleId", "No metadata available"), 12f, muted, parent = labels)
            app.optString("minimumIOSVersion").takeIf { it.isNotBlank() }?.let { text("Minimum iOS $it", 11f, muted, parent = labels) }
            val state = report.optString("state", "FAILED")
            text(state, 11f, statusColor(state), true, item)
            report.optJSONObject("portProgress")?.let { port ->
                text("Android game-code translation: ${port.optInt("percent", 0)}% · not a gameplay test", 11f, statusColor("BLOCKED"), true, item)
            }
            report.optJSONObject("analysisProgress")?.let { analysis ->
                val value = analysis.optInt("percent", 0)
                text("IPA analysis: $value% · ${analysis.optString("stage", "queued")} · not an APK-build score", 12f, textColor, true, item)
                progressIndicator(item, value, 6)
                text(analysis.optString("message"), 11f, muted, parent = item)
            }
            report.optJSONObject("conversionProgress")?.let { conversion ->
                text("Complete-game APK: ${conversion.optString("status", "NOT_BUILT")} · ${conversion.optString("message")}", 11f, muted, parent = item)
            }
            report.optJSONObject("apiMapping")?.let { mapping ->
                val mapped = mapping.optInt("mappedNameCandidates", 0)
                val total = mapping.optInt("distinctImportSymbols", 0)
                val coverage = mapping.optInt("candidateCoveragePercent", 0)
                val semantic = mapping.optInt("semanticRewriteCandidates", 0)
                val generated = report.optJSONObject("hostConversion")?.optInt("generatedApiReplacements", 0) ?: 0
                val summary = if (total == 0) "API candidates: N/A (no imported symbols) · host-generated replacements: $generated"
                    else "Candidates only: $coverage% direct NDK names ($mapped/$total) · $semantic semantic targets · host-generated replacements: $generated"
                text(summary, 11f, muted, parent = item)
            }
            report.optJSONObject("hostConversion")?.takeIf { it.optString("status") == "ATTACHED" }?.let { host ->
                text("Complete-game host APK attached · ABI ${host.optString("targetAbi")} · ${host.optInt("nativeCodeBytes", 0)} generated native-code bytes · runtime not tested", 11f, accent, parent = item)
            }
            text("v${app.optString("version", "—")}  ·  ${architectures(report)}  ·  ${formatBytes(app.optLong("fileSize"))}", 12f, muted, parent = item)
            item.isClickable = true; item.setOnClickListener { detail(dir) }; item.contentDescription = "View ${app.optString("name")} details"
        }
        text("v${packageManager.getPackageInfo(packageName, 0).versionName}  /  ARM64 Android  /  offline inspection", 11f, muted)
    }
    private fun settingsScreen() {
        if (Jobs.busy) return
        screen()
        button("← Back") { home() }
        text("Settings", 28f, textColor, true)
        text("Appearance and host analysis target architecture.", 14f, muted)

        val appearance = card()
        text("Appearance", 17f, textColor, true, appearance)
        val lightSwitch = android.widget.Switch(this).apply {
            text = "Use light theme"
            textSize = 15f
            setTextColor(textColor)
            isChecked = preferences.getBoolean("light_theme", false)
        }
        appearance.addView(lightSwitch, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) })
        lightSwitch.setOnCheckedChangeListener { _, enabled ->
            preferences.edit().putBoolean("light_theme", enabled).apply()
            applyThemePalette()
            settingsScreen()
        }

        val host = card()
        text("Host analysis target ABI", 17f, textColor, true, host)
        val preferredAbi = preferences.getString("target_abi", "auto") ?: "auto"
        val abiLabel = when (preferredAbi) {
            "armeabi-v7a" -> "32-bit ARM (armeabi-v7a)"
            "arm64-v8a" -> "64-bit ARM (arm64-v8a)"
            else -> "Automatic · prefers 64-bit ARM64 when present"
        }
        button("Preferred host analysis ABI\n$abiLabel", parent = host) {
            val options = arrayOf(
                "Automatic (prefer ARM64 if the IPA contains it)",
                "32-bit ARM (armeabi-v7a)",
                "64-bit ARM (arm64-v8a)",
            )
            val selectedIndex = when (preferredAbi) { "armeabi-v7a" -> 1; "arm64-v8a" -> 2; else -> 0 }
            AlertDialog.Builder(this)
                .setTitle("Host analysis ABI")
                .setSingleChoiceItems(options, selectedIndex) { dialog, which ->
                    preferences.edit().putString("target_abi", when (which) {
                        1 -> "armeabi-v7a"
                        2 -> "arm64-v8a"
                        else -> "auto"
                    }).apply()
                    dialog.dismiss()
                    settingsScreen()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
        text("Automatic analysis prefers the ARM64 slice in a FAT IPA containing both 32-bit and 64-bit code. Supported ARM32-only inputs select a 32-bit ARMv7 target.", 12f, muted, parent = host)
        text("This preference only affects the copied host analysis command. It does not mean a game APK can currently be generated.", 12f, muted, parent = host)

        button("Restore defaults") {
            preferences.edit()
                .putBoolean("light_theme", false)
                .putString("target_abi", "auto")
                .apply()
            applyThemePalette()
            settingsScreen()
        }
        button("Done", true) { home() }
    }

    private fun documentDisplayName(uri: Uri): String {
        val queried = try {
            contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                val index = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                if (index >= 0 && cursor.moveToFirst()) cursor.getString(index) else null
            }
        } catch (_: Exception) { null }
        return queried?.takeIf { it.isNotBlank() }
            ?: uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() }
            ?: "Selected IPA"
    }

    private fun architectures(report: JSONObject): String {
        val slices = report.optJSONObject("machO")?.optJSONArray("slices") ?: return "unknown CPU"
        return (0 until slices.length()).joinToString(" / ") { slices.getJSONObject(it).getString("architecture") }
    }
    private fun authorize() {
        AlertDialog.Builder(this).setTitle("Authorized files only")
            .setMessage("Confirm that you own this IPA or have permission to convert it. Protection mechanisms will not be bypassed. The source IPA is retained in app-private storage for analysis until you delete this library entry. Analysis does not translate the game; the separate Force action can create only a non-playable, installable placeholder with no translated game code. Complete-game host APKs still require the strict conversion contract.")
            .setNegativeButton("Cancel", null).setPositiveButton("I have permission") { _, _ ->
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "*/*"; addCategory(Intent.CATEGORY_OPENABLE) }, pickerIpa)
            }.show()
    }
    private fun detail(dir: File) {
        selected = dir; screen()
        val report = JSONObject(File(dir, "report.json").readText()); val app = report.optJSONObject("application") ?: JSONObject()
        button("← Game Library") { home() }
        val iconPath = File(dir, "icon.png")
        if (iconPath.isFile) {
            val bitmap = android.graphics.BitmapFactory.decodeFile(iconPath.path)
            if (bitmap != null) {
                val icon = ImageView(this).apply {
                    setImageBitmap(bitmap)
                    contentDescription = "${app.optString("name", "Application")} icon"
                    scaleType = ImageView.ScaleType.FIT_CENTER
                }
                body.addView(icon, LinearLayout.LayoutParams(dp(84), dp(84)).apply { gravity = Gravity.CENTER_HORIZONTAL; topMargin = dp(10) })
            }
        }
        text(app.optString("name", "Application details"), 28f, textColor, true)
        val state = report.optString("state"); text(state, 12f, statusColor(state), true)
        if (Jobs.busy) addJobProgressCard()
        val metadata = card()
        for ((name, value) in listOf(
            "Bundle ID" to app.optString("bundleId"),
            "Version / build" to "${app.optString("version")} / ${app.optString("build")}",
            "Minimum iOS" to app.optString("minimumIOSVersion").ifBlank { "Not declared in Info.plist" },
            "Architecture" to architectures(report),
            "IPA size" to formatBytes(app.optLong("fileSize")),
            "Executable" to app.optString("executable"),
        )) {
            text(name.uppercase(), 10f, muted, true, metadata); text(value, 15f, parent = metadata)
        }
        report.optJSONObject("apiMapping")?.let { mapping ->
            val mapped = mapping.optInt("mappedNameCandidates", 0)
            val total = mapping.optInt("distinctImportSymbols", 0)
            val coverage = mapping.optInt("candidateCoveragePercent", 0)
            val mappingCard = card()
            val semantic = mapping.optInt("semanticRewriteCandidates", 0)
            val summary = if (total == 0) "Android API candidates: N/A (no imports)"
                else "Direct NDK name candidates: $coverage% ($mapped/$total) · semantic rewrite candidates: $semantic"
            text(summary, 16f, textColor, true, mappingCard)
            val generated = report.optJSONObject("hostConversion")?.optInt("generatedApiReplacements", 0) ?: 0
            val mappingDisclosure = if (generated > 0) "The on-device mapper generated no code; an attached complete-game host conversion reports $generated generated API replacement(s). Runtime behavior is not device-tested."
                else "No API implementation or replacement code was generated; these candidates do not predict gameplay compatibility or stability."
            text(mapping.optString("measure") + " $mappingDisclosure", 13f, muted, parent = mappingCard)
        }
        report.optJSONObject("portProgress")?.let { port ->
            val portCard = card()
            text("Android game-code translation progress: ${port.optInt("percent", 0)}%", 16f, statusColor("BLOCKED"), true, portCard)
            text(port.optString("basis"), 13f, muted, parent = portCard)
        }
        report.optJSONObject("analysisProgress")?.let { analysis ->
            val analysisCard = card()
            val value = analysis.optInt("percent", 0)
            text("IPA analysis · $value%", 16f, textColor, true, analysisCard)
            progressIndicator(analysisCard, value, 10)
            text("${analysis.optString("stage", "queued")} · ${analysis.optString("message")}", 12f, muted, parent = analysisCard)
            text("Analysis completion is not APK-build completion or a playability score.", 12f, muted, parent = analysisCard)
        }
        report.optJSONObject("conversionProgress")?.let { conversion ->
            val buildCard = card()
            text("Host APK validation/attachment · ${conversion.optInt("percent", 0)}% · ${conversion.optString("status", "NOT_BUILT")}", 16f, statusColor(conversion.optString("status")), true, buildCard)
            text(conversion.optString("message"), 12f, muted, parent = buildCard)
            text("This is the complete-game APK path; it requires translated reachable code, API replacements, resources and lifecycle. The separate Force action can produce an installable placeholder only, with no translated gameplay.", 12f, muted, parent = buildCard)
        }
        text("Compatibility report", 22f, textColor, true)
        val blockers = report.optJSONArray("blockers")
        if (blockers != null) for (i in 0 until blockers.length()) text(blockers.getString(i), 15f, statusColor(state))
        if (report.has("error")) text(report.getString("error"), 15f, statusColor("FAILED"))
        text("Icon: ${report.optJSONObject("icon")?.optString("reason") ?: "not extracted"}", 13f, muted)
        val edges = report.optJSONObject("dependencies")?.optJSONArray("edges")
        if (edges != null) for (i in 0 until edges.length()) {
            val dep = edges.getJSONObject(i)
            val classification = dep.optString("classification", "unverified").uppercase()
            text("$classification · ${dep.getString("installName")}", 13f, muted)
            text(dep.optString("reason"), 11f, muted)
        }
        button("View full machine-readable report") { showText("Conversion report", report.toString(2)) }
        button("View real conversion logs") { showText("Logs", File(dir, "conversion.jsonl").takeIf { it.isFile }?.readText() ?: "No logs") }
        text("APK conversion", 22f, textColor, true)
        text("A complete iOS-to-Android game translator and framework/API replacements are not implemented. The host CLI can inspect and reconstruct code, but its restricted experimental native-entry output is not a complete game port and is not accepted as one. Force creates only a signed placeholder with the IPA app name and icon where available; the iOS executable and game code are not translated, so this placeholder will not run the game.", 14f, muted)
        button("Copy host analysis command") {
            val abi = preferences.getString("target_abi", "auto") ?: "auto"
            val suffix = if (abi == "auto") "" else " --target-abi $abi"
            val command = "python3 -m radek analyze input.ipa --authorized --output workspace/analysis-result$suffix"
            (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).setPrimaryClip(android.content.ClipData.newPlainText("Host analysis command", command))
            Toast.makeText(this, "Analysis command copied", Toast.LENGTH_SHORT).show()
        }
        if (app.has("sha256")) button("Attach complete host-converted APK") {
            if (!Jobs.busy) startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "application/vnd.android.package-archive"; addCategory(Intent.CATEGORY_OPENABLE) }, pickerApk)
        }
        val hostConversion = report.optJSONObject("hostConversion")
        val hostAttached = hostConversion != null && hostConversion.optString("status") == "ATTACHED" &&
            hostConversion.optBoolean("completeGameConversion", false) && hostConversion.optString("contract") == "complete-game-v1"
        val expectedHostName = ArtifactNames.apkFileName(report)
        val reportedHostName = hostConversion?.optString("artifact").orEmpty()
        val hostOutputFile = if (hostAttached && reportedHostName == expectedHostName) {
            File(dir, expectedHostName).takeIf { it.isFile }
        } else null
        if (hostAttached && hostOutputFile == null) {
            text("The attached APK is missing or its IPA-basename record does not match; it cannot be installed or shared.", 13f, statusColor("FAILED"))
        }
        if (hostOutputFile != null) {
            val hostAbi = hostConversion?.optString("targetAbi").orEmpty()
            text("Complete-game host APK attached · ABI $hostAbi. The host conversion contract passed static checks; on-device execution and actual gameplay have not been tested.", 13f, accent)
            text("Translated reachable functions: ${hostConversion?.optInt("translatedReachableFunctions", 0) ?: 0} · generated API replacements: ${hostConversion?.optInt("generatedApiReplacements", 0) ?: 0} · unresolved reachable functions: ${hostConversion?.optInt("untranslatedReachableFunctions", -1) ?: -1}.", 12f, muted)
            button("Install ${hostOutputFile.name}", true) { installArtifact(dir, hostOutputFile.name) }
            button("Share ${hostOutputFile.name}") { shareResultApk(dir, hostOutputFile.name) }
            if (app.has("sha256")) button("Open installed converted program") {
                val pkg = "dev.radek.converted.p" + app.getString("sha256").take(20)
                val intent = packageManager.getLaunchIntentForPackage(pkg)
                if (intent == null) Toast.makeText(this, "Converted program is not installed or not visible to Android", Toast.LENGTH_LONG).show() else startActivity(intent)
            }
        }
        val placeholderConversion = report.optJSONObject("placeholderConversion")
        val placeholderName = ArtifactNames.placeholderApkFileName(report)
        val placeholderOutputFile = if (placeholderConversion?.optString("status") == "GENERATED") {
            runCatching { PlaceholderArtifactContract.validate(report, dir, placeholderName) }.getOrNull()
        } else null
        if (placeholderConversion?.optString("status") == "GENERATED" && placeholderOutputFile == null) {
            text("The placeholder APK is missing or its digest/metadata is invalid; it cannot be installed or shared.", 13f, statusColor("FAILED"))
        }
        report.optJSONObject("placeholderBuildProgress")?.takeIf { it.optString("status") == "FAILED" }?.let { build ->
            text("Placeholder build failed: ${build.optString("message")}", 13f, statusColor("FAILED"))
        }
        if (placeholderOutputFile != null) {
            val iconDescription = when (placeholderConversion?.optString("iconSource")) {
                "RECOVERED_IPA_ICON" -> "Original IPA icon included"
                "GENERATED_APP_NAME_ICON" -> "Generated name-based icon included; no original icon was recovered"
                else -> "Fallback icon included; no original icon was recovered"
            }
            text("Installable placeholder APK · $iconDescription", 13f, accent, true)
            text("This launches a branded notice screen only. No iOS executable, translated game code, or playable gameplay is included.", 12f, muted)
            button("Install ${placeholderOutputFile.name}", true) { installArtifact(dir, placeholderOutputFile.name) }
            button("Share ${placeholderOutputFile.name}") { shareResultApk(dir, placeholderOutputFile.name) }
        }
        if (File(dir, "source.ipa").isFile && app.has("sha256") && hostOutputFile == null) {
            text("Force builds a signed, installable placeholder APK only; it does not translate or run the game.", 12f, muted)
            dangerButton(if (placeholderOutputFile != null) "Rebuild placeholder APK" else "Force convert to .apk") {
                startPlaceholderBuild(dir)
            }
        }
        button("Delete library entry") {
            if (!Jobs.busy) AlertDialog.Builder(this).setTitle("Delete imported entry?").setMessage("Removes the retained IPA, analysis reports, recovered icon, any validated complete-game host APK and any generated placeholder APK from this device.")
                .setNegativeButton("Cancel", null).setPositiveButton("Delete") { _, _ -> dir.deleteRecursively(); home() }.show()
        }
    }

    private fun startPlaceholderBuild(dir: File) {
        if (Jobs.busy) return
        if (!File(dir, "source.ipa").isFile) {
            Toast.makeText(this, "Retained IPA not found; placeholder cannot be built", Toast.LENGTH_LONG).show()
            return
        }
        Jobs.begin("Building installable placeholder")
        returnToDetailAfterJob = dir
        wasBusy = true
        Jobs.run {
            PlaceholderApkBuilder(applicationContext).build(dir) { percent, message ->
                Jobs.update(percent, message)
            }
        }
        home()
    }

    private fun shareResultApk(dir: File, name: String) {
        val file = File(dir, name)
        if (!file.isFile) {
            Toast.makeText(this, "APK result not found", Toast.LENGTH_LONG).show()
            return
        }
        val uri = Uri.Builder().scheme("content").authority("dev.radek.conventor.results")
            .appendPath(dir.name).appendPath(file.name).build()
        val share = Intent(Intent.ACTION_SEND).apply {
            type = "application/vnd.android.package-archive"
            putExtra(Intent.EXTRA_STREAM, uri)
            clipData = android.content.ClipData.newUri(contentResolver, file.name, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(share, "Share APK result"))
    }

    private fun showText(title: String, value: String) {
        val view = TextView(this).apply { text = value; setTextIsSelectable(true); textSize = 12f; typeface = Typeface.MONOSPACE; setPadding(dp(16), dp(16), dp(16), dp(16)) }
        AlertDialog.Builder(this).setTitle(title).setView(ScrollView(this).apply { addView(view) }).setPositiveButton("Close", null).show()
    }
    @Deprecated("Framework result API retained to avoid third-party dependencies")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data?.data == null) return
        val uri = data.data!!
        if (Jobs.busy) return
        val context = applicationContext
        if (requestCode == pickerIpa) {
            Jobs.begin("Analyzing · ${documentDisplayName(uri)}")
            Jobs.update(0, "Starting IPA analysis; game code is not translated")
            Jobs.run {
                val library = Library(context)
                val (dir, report) = library.import(uri) { percent, message -> Jobs.update(percent, message) }
                returnToDetailAfterJob = dir
                val state = report.optString("state", "FAILED")
                Jobs.update(100, "$state · IPA analysis finished · APK ${report.optJSONObject("conversionProgress")?.optString("status", "NOT_BUILT") ?: "NOT_BUILT"}")
            }
            wasBusy = true; home()
        } else if (requestCode == pickerApk) {
            val dir = selected ?: return
            Jobs.run { attachApk(context, uri, dir); Jobs.update("Host APK attached") }
            wasBusy = true; home()
        }
    }
    private fun installArtifact(dir: File, name: String) {
        if (!File(dir, name).isFile) {
            Toast.makeText(this, "APK result not found", Toast.LENGTH_LONG).show()
            return
        }
        if (!packageManager.canRequestPackageInstalls()) {
            pendingInstall = dir to name
            startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
            return
        }
        pendingInstall = null
        val uri = Uri.Builder().scheme("content").authority("dev.radek.conventor.results")
            .appendPath(dir.name).appendPath(name).build()
        startActivity(Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, "application/vnd.android.package-archive")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
    }
    companion object {
        private fun sha256(input: java.io.InputStream): String {
            val digest = java.security.MessageDigest.getInstance("SHA-256")
            val buffer = ByteArray(65536)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
            return digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
        }

        private fun exactInt(json: JSONObject, key: String): Int {
            val value = json.opt(key)
            require(value is Int || value is Long) { "$key must be an integer" }
            val number = (value as Number).toLong()
            require(number in Int.MIN_VALUE.toLong()..Int.MAX_VALUE.toLong()) { "$key is out of range" }
            return number.toInt()
        }

        private fun attachApk(context: android.content.Context, uri: Uri, dir: File) {
            val temporary = File(dir, "result.pending")
            try {
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "cannot read selected APK" }
                    temporary.outputStream().use { output ->
                        val buffer = ByteArray(65536)
                        var total = 0L
                        while (true) {
                            val n = input.read(buffer)
                            if (n < 0) break
                            total += n
                            require(total <= SafeZip.MAX_ARCHIVE) { "APK exceeds 512 MiB" }
                            output.write(buffer, 0, n)
                        }
                    }
                }
                val report = JSONObject(File(dir, "report.json").readText())
                val hash = report.getJSONObject("application").getString("sha256")
                val expectedPackage = "dev.radek.converted.p" + hash.take(20)
                val expectedName = ArtifactNames.apkFileName(report)
                val selectedAbi = context.getSharedPreferences("radek_settings", android.content.Context.MODE_PRIVATE)
                    .getString("target_abi", "auto") ?: "auto"
                val sourceSlices = report.optJSONObject("machO")?.optJSONArray("slices") ?: error("source architecture report missing")
                val sourceArchitectures = (0 until sourceSlices.length()).map { sourceSlices.getJSONObject(it).optString("architecture") }.toSet()
                val expectedAbi = when (selectedAbi) {
                    "arm64-v8a" -> {
                        require("arm64" in sourceArchitectures) { "selected ARM64 ABI has no matching IPA slice" }
                        "arm64-v8a"
                    }
                    "armeabi-v7a" -> {
                        require(sourceArchitectures.any { it in setOf("armv7s", "armv7", "armv6") }) { "selected ARM32 ABI has no matching IPA slice" }
                        "armeabi-v7a"
                    }
                    else -> when {
                        "arm64" in sourceArchitectures -> "arm64-v8a"
                        sourceArchitectures.any { it in setOf("armv7s", "armv7", "armv6") } -> "armeabi-v7a"
                        else -> error("IPA has no supported ARM32 or ARM64 slice")
                    }
                }
                var targetAbi = ""
                var generatedBytes = 0
                var conversionBackend = ""
                var translatedFunctions = 0
                var generatedApiReplacements = 0
                var untranslatedFunctions = 0
                var recoveredIconHash = ""
                val sourceIcon = File(dir, "icon.png")
                if (sourceIcon.isFile) {
                    recoveredIconHash = java.security.MessageDigest.getInstance("SHA-256")
                        .digest(sourceIcon.readBytes()).joinToString("") { "%02x".format(it.toInt() and 255) }
                }
                ZipFile(temporary).use { zip ->
                    val zipEntries = zip.entries().asSequence().toList()
                    require(zipEntries.size <= 20000) { "APK has too many entries" }
                    require(zipEntries.sumOf { it.size.coerceAtLeast(0L) } <= 1024L * 1024 * 1024) { "APK expands beyond the allowed size" }
                    require(zipEntries.all { it.size <= 256L * 1024 * 1024 }) { "APK contains an oversized entry" }
                    val names = zipEntries.map { it.name }
                    names.forEach { SafeZip.validateName(it) }
                    require(names.size == names.toSet().size) { "APK contains duplicate paths" }
                    require(names.none { it.lowercase(java.util.Locale.ROOT).endsWith(".ipa") }) { "APK must not contain the original IPA" }
                    val entry = zip.getEntry("assets/conversion.json") ?: error("conversion provenance missing")
                    require(entry.size in 1..4194304)
                    val metadata = JSONObject(zip.getInputStream(entry).bufferedReader().use { it.readText() })
                    require(metadata.getString("package") == expectedPackage && metadata.getJSONObject("source").getString("sha256") == hash) { "APK does not match this IPA" }
                    require(metadata.optString("contract") == "complete-game-v1") { "APK is not a complete-game conversion; restricted native-entry packages are rejected" }
                    require(metadata.optString("targetAbi") in setOf("arm64-v8a", "armeabi-v7a")) { "unsupported APK ABI" }
                    val conversion = metadata.optJSONObject("conversion") ?: error("conversion details missing")
                    targetAbi = conversion.optString("targetAbi")
                    require(targetAbi in setOf("arm64-v8a", "armeabi-v7a") && metadata.optString("targetAbi") == targetAbi) { "unsupported or inconsistent converted APK ABI" }
                    require(targetAbi == expectedAbi) { "APK ABI does not follow the selected/source architecture policy (automatic mode prefers ARM64 for FAT ARM32+ARM64 input)" }
                    generatedBytes = exactInt(conversion, "outputBytes")
                    require(generatedBytes in 1..(64 * 1024 * 1024)) { "invalid generated native-code size" }
                    conversionBackend = conversion.opt("backend") as? String ?: error("conversion backend is not a string")
                    require(conversionBackend.isNotBlank()) { "conversion backend missing" }
                    val game = metadata.optJSONObject("gameConversion") ?: error("complete-game evidence missing")
                    require(game.opt("status") == "COMPLETE" && game.opt("completeGameConversion") == true) { "host did not certify a complete game conversion" }
                    val reachableFunctions = exactInt(game, "reachableSourceFunctions")
                    translatedFunctions = exactInt(game, "translatedReachableFunctions")
                    untranslatedFunctions = exactInt(game, "untranslatedReachableFunctions")
                    require(reachableFunctions > 0 && translatedFunctions == reachableFunctions && untranslatedFunctions == 0) { "not all reachable game functions were translated" }
                    val reachableApis = exactInt(game, "reachableApiCount")
                    generatedApiReplacements = exactInt(game, "generatedApiReplacements")
                    val nativeApiPassthroughs = exactInt(game, "nativeApiPassthroughs")
                    require(game.opt("apiCoverageComplete") == true && exactInt(game, "untranslatedReachableApiCount") == 0) { "reachable iOS APIs are not fully implemented or replaced" }
                    require(reachableApis >= 0 && generatedApiReplacements >= 0 && nativeApiPassthroughs >= 0 && reachableApis == generatedApiReplacements + nativeApiPassthroughs) { "API replacement accounting is incomplete" }
                    val replacements = game.optJSONArray("apiReplacements") ?: error("API replacement evidence missing")
                    require(replacements.length() == generatedApiReplacements) { "generated API replacement count mismatch" }
                    for (index in 0 until replacements.length()) {
                        val replacement = replacements.getJSONObject(index)
                        require(replacement.opt("codeGenerated") == true && replacement.opt("linkedIntoApk") == true && replacement.opt("reachableFromEntry") == true) { "API mapping is only a candidate, not reachable generated code" }
                        val sourceSymbol = replacement.opt("sourceSymbol") as? String ?: error("API source symbol must be a string")
                        val targetAndroidApi = replacement.opt("targetAndroidApi") as? String ?: error("target Android API must be a string")
                        require(sourceSymbol.isNotBlank() && targetAndroidApi.isNotBlank()) { "API replacement mapping incomplete" }
                        val implementationHash = replacement.opt("implementationSha256") as? String ?: error("API implementation hash must be a string")
                        require(implementationHash.matches(Regex("[0-9a-f]{64}"))) { "API replacement implementation hash missing" }
                        val implementationArtifact = replacement.opt("implementationArtifact") as? String ?: error("API implementation artifact must be a string")
                        require(implementationArtifact.isNotBlank()) { "generated API implementation path missing" }
                        val safeImplementationArtifact = SafeZip.validateName(implementationArtifact)
                        require(safeImplementationArtifact == implementationArtifact && names.contains(safeImplementationArtifact)) { "generated API implementation is not packaged" }
                        val packagedImplementation = zip.getEntry(safeImplementationArtifact) ?: error("generated API implementation is missing")
                        val packagedHash = zip.getInputStream(packagedImplementation).use { sha256(it) }
                        require(packagedHash == implementationHash) { "generated API implementation hash mismatch" }
                    }
                    require(game.opt("resourcesComplete") == true) { "game resources were not completely packaged" }
                    require(game.opt("lifecycleImplemented") == true) { "Android application lifecycle is not implemented" }
                    val inventoryValue = metadata.opt("resourceInventory")
                    require(inventoryValue == null || inventoryValue is JSONArray) { "game resource inventory is invalid" }
                    val resourceInventory = inventoryValue as? JSONArray
                    for (index in 0 until (resourceInventory?.length() ?: 0)) {
                        val resource = resourceInventory!!.getJSONObject(index)
                        val resourcePath = resource.opt("path") as? String ?: error("resource path must be a string")
                        val safeResourcePath = SafeZip.validateName(resourcePath)
                        require(safeResourcePath == resourcePath) { "resource path is not canonical" }
                        val resourceHash = resource.opt("sha256") as? String ?: error("resource hash must be a string")
                        require(resourceHash.matches(Regex("[0-9a-f]{64}"))) { "resource hash is invalid" }
                        val packagedResourceName = "assets/bundle/$safeResourcePath"
                        val packagedResource = zip.getEntry(packagedResourceName) ?: error("game resource is not packaged: $packagedResourceName")
                        val packagedResourceHash = zip.getInputStream(packagedResource).use { sha256(it) }
                        require(packagedResourceHash == resourceHash) { "game resource integrity failure: $packagedResourceName" }
                    }
                    if (recoveredIconHash.isNotBlank()) {
                        require(game.optString("sourceIconSha256") == recoveredIconHash && game.optString("launcherIconSha256") == recoveredIconHash) { "recovered IPA launcher icon was not preserved" }
                    }
                    require(names.any { it == "lib/$targetAbi/libconverted.so" }) { "converted native library missing for $targetAbi" }
                    require(names.filter { it.startsWith("lib/") && it.endsWith(".so") }.all { it.startsWith("lib/$targetAbi/") }) { "APK contains a native library for a different ABI" }
                    val appleExecutableMagics = setOf("cffaedfe", "cefaedfe", "feedface", "feedfacf", "cafebabe", "cafebabf", "bebafeca", "bfbafeca")
                    for (name in names) {
                        val archiveEntry = zip.getEntry(name) ?: continue
                        zip.getInputStream(archiveEntry).use { input ->
                            val digest = java.security.MessageDigest.getInstance("SHA-256")
                            val head = ByteArray(4)
                            var count = 0
                            while (count < head.size) {
                                val read = input.read(head, count, head.size - count)
                                if (read < 0) break
                                if (read == 0) {
                                    val next = input.read()
                                    if (next < 0) break
                                    head[count++] = next.toByte()
                                } else {
                                    count += read
                                }
                            }
                            val magic = if (count == head.size) head.joinToString("") { "%02x".format(it.toInt() and 255) } else ""
                            require(magic !in appleExecutableMagics) { "Apple executable leaked into APK" }
                            digest.update(head, 0, count)
                            val buffer = ByteArray(65536)
                            while (true) {
                                val read = input.read(buffer)
                                if (read < 0) break
                                if (read == 0) {
                                    val next = input.read()
                                    if (next < 0) break
                                    digest.update(next.toByte())
                                } else {
                                    digest.update(buffer, 0, read)
                                }
                            }
                            val contentHash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
                            require(contentHash != hash) { "APK embeds the original IPA content" }
                        }
                    }
                }
                @Suppress("DEPRECATION")
                val pkg = context.packageManager.getPackageArchiveInfo(temporary.path, android.content.pm.PackageManager.GET_ACTIVITIES or android.content.pm.PackageManager.GET_SIGNATURES)
                    ?: error("Android cannot parse APK")
                require(pkg.packageName == expectedPackage && pkg.activities.orEmpty().any { it.name == "dev.radek.generated.MainActivity" }) { "Android package/entry mismatch" }
                @Suppress("DEPRECATION")
                require(!pkg.signatures.isNullOrEmpty()) { "APK signer missing" }
                val result = File(dir, expectedName)
                if (result.exists()) require(result.delete()) { "cannot replace existing host APK" }
                require(temporary.renameTo(result))
                report.put("hostConversion", JSONObject()
                    .put("status", "ATTACHED")
                    .put("completeGameConversion", true)
                    .put("artifact", result.name)
                    .put("package", expectedPackage)
                    .put("targetAbi", targetAbi)
                    .put("nativeCodeGenerated", true)
                    .put("nativeCodeBytes", generatedBytes)
                    .put("translatedReachableFunctions", translatedFunctions)
                    .put("generatedApiReplacements", generatedApiReplacements)
                    .put("untranslatedReachableFunctions", untranslatedFunctions)
                    .put("sourceIconSha256", recoveredIconHash)
                    .put("backend", conversionBackend)
                    .put("contract", "complete-game-v1")
                    .put("runtimeExecution", "NOT_TESTED")
                    .put("gamePlayability", "NOT_TESTED")
                    .put("installableAndroidPackage", true)
                    .put("completedAt", java.time.Instant.now().toString()))
                report.put("portProgress", JSONObject()
                    .put("percent", 100)
                    .put("status", "COMPLETE_CONVERSION_ATTACHED")
                    .put("basis", "Host metadata reports all reachable code and required APIs translated; runtime execution and gameplay are not tested."))
                report.put("conversionProgress", JSONObject()
                    .put("percent", 100)
                    .put("stage", "HOST_APK_VALIDATED")
                    .put("status", "ATTACHED")
                    .put("message", "A complete-game host APK passed static attachment checks; device execution is not tested."))
                Library(context).save(dir, report)
            } finally { temporary.delete() }
        }
    }
}
