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
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors
import java.util.zip.ZipFile

private object Jobs {
    private val executor = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    @Volatile var busy = false
    @Volatile var message = ""
    var listener: (() -> Unit)? = null
    fun update(text: String) { message = text; main.post { listener?.invoke() } }
    @Synchronized fun run(block: () -> Unit) {
        check(!busy) { "A job is already running" }; busy = true
        executor.execute { try { block() } catch (e: Exception) { update("Failed: ${e.message}") } finally { busy = false; main.post { listener?.invoke() } } }
    }
}

class MainActivity : Activity() {
    private val bgColor = Color.rgb(12, 19, 31)
    private val panel = Color.rgb(23, 34, 51)
    private val muted = Color.rgb(160, 178, 199)
    private val accent = Color.rgb(92, 227, 181)
    private lateinit var library: Library
    private lateinit var body: LinearLayout
    private var selected: File? = null
    private var progressLabel: TextView? = null
    private var wasBusy = false
    private val pickerIpa = 100
    private val pickerApk = 101
    private fun dp(n: Int) = (n * resources.displayMetrics.density).toInt()

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        window.statusBarColor = bgColor; window.navigationBarColor = bgColor
        library = Library(applicationContext)
        if (!Jobs.busy) library.recoverInterrupted()
        selected = state?.getString("selected")?.let { File(library.root, it) }
        Jobs.listener = {
            progressLabel?.text = Jobs.message
            if (wasBusy && !Jobs.busy) { wasBusy = false; home() }
        }
        if (selected != null && File(selected, "report.json").isFile) detail(selected!!) else home()
    }
    override fun onSaveInstanceState(out: Bundle) { super.onSaveInstanceState(out); out.putString("selected", selected?.name) }
    override fun onDestroy() { Jobs.listener = null; super.onDestroy() }
    override fun onResume() { super.onResume(); if (Jobs.busy) { wasBusy = true; progressLabel?.text = Jobs.message } }

    private fun rounded(color: Int): GradientDrawable = GradientDrawable().apply { setColor(color); cornerRadius = dp(18).toFloat() }
    private fun screen() {
        progressLabel = null
        val scroll = ScrollView(this).apply { setBackgroundColor(bgColor); isFillViewport = true }
        body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(22), dp(30), dp(22), dp(36)) }
        scroll.addView(body); setContentView(scroll)
    }
    private fun text(value: String, size: Float = 16f, color: Int = Color.WHITE, bold: Boolean = false, parent: LinearLayout = body): TextView = TextView(this).apply {
        text = value; textSize = size; setTextColor(color); if (bold) setTypeface(typeface, Typeface.BOLD)
        setPadding(0, dp(6), 0, dp(7)); parent.addView(this)
    }
    private fun button(label: String, primary: Boolean = false, parent: LinearLayout = body, action: () -> Unit): Button = Button(this).apply {
        text = label; isAllCaps = false; textSize = 15f; setTextColor(if (primary) bgColor else accent)
        background = rounded(if (primary) accent else panel)
        parent.addView(this, LinearLayout.LayoutParams(-1, dp(54)).apply { topMargin = dp(12); bottomMargin = dp(4) })
        setOnClickListener { action() }
    }
    private fun card(parent: LinearLayout = body): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL; background = rounded(panel); setPadding(dp(18), dp(14), dp(18), dp(16))
        parent.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(14) })
    }
    private fun statusColor(state: String) = when (state) { "READY" -> accent; "FAILED", "BLOCKED" -> Color.rgb(255, 157, 139); else -> Color.rgb(245, 203, 116) }
    private fun formatBytes(n: Long) = "%.1f MiB".format(n / 1048576.0)
    private fun home() {
        selected = null; screen()
        text("R /  NATIVE CONVERSION LAB", 11f, accent, true)
        text("RadekConventor", 30f, Color.WHITE, true)
        text("Your apps. A different platform.", 16f, muted)
        val info = card()
        text("No emulation. No hidden success.", 17f, Color.WHITE, true, info)
        text("Import authorized IPAs to inspect their real metadata, machine code and dependencies. Native APK compilation currently runs on a Linux host; most iOS frameworks are blocked.", 14f, muted, parent = info)
        val add = button("+ ADD IPA", true) { authorize() }; add.isEnabled = !Jobs.busy
        if (Jobs.busy) {
            wasBusy = true
            body.addView(ProgressBar(this), LinearLayout.LayoutParams(dp(28), dp(28)).apply { topMargin = dp(18) })
            progressLabel = text(Jobs.message, 14f, accent)
        } else if (Jobs.message.startsWith("Failed:")) text(Jobs.message, 14f, statusColor("FAILED"))
        text("Game Library", 23f, Color.WHITE, true)
        val entries = library.entries()
        text("${entries.size} imported ${if (entries.size == 1) "application" else "applications"} · private device storage", 12f, muted)
        if (entries.isEmpty()) {
            val empty = card(); text("Your library starts here", 19f, Color.WHITE, true, empty)
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
            text(app.optString("name", "Import failed"), 19f, Color.WHITE, true, labels)
            text(app.optString("bundleId", "No metadata available"), 12f, muted, parent = labels)
            val state = report.optString("state", "FAILED")
            text(state, 11f, statusColor(state), true, item)
            text("v${app.optString("version", "—")}  ·  ${architectures(report)}  ·  ${formatBytes(app.optLong("fileSize"))}", 12f, muted, parent = item)
            item.isClickable = true; item.setOnClickListener { detail(dir) }; item.contentDescription = "View ${app.optString("name")} details"
        }
        text("v${packageManager.getPackageInfo(packageName, 0).versionName}  /  ARM64 Android  /  offline inspection", 11f, muted)
    }
    private fun architectures(report: JSONObject): String {
        val slices = report.optJSONObject("machO")?.optJSONArray("slices") ?: return "unknown CPU"
        return (0 until slices.length()).joinToString(" / ") { slices.getJSONObject(it).getString("architecture") }
    }
    private fun authorize() {
        AlertDialog.Builder(this).setTitle("Authorized files only")
            .setMessage("Confirm that you own this IPA or have permission to convert it. Protection mechanisms will not be bypassed. Import does not guarantee conversion compatibility.")
            .setNegativeButton("Cancel", null).setPositiveButton("I have permission") { _, _ ->
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "*/*"; addCategory(Intent.CATEGORY_OPENABLE) }, pickerIpa)
            }.show()
    }
    private fun detail(dir: File) {
        selected = dir; screen()
        val report = JSONObject(File(dir, "report.json").readText()); val app = report.optJSONObject("application") ?: JSONObject()
        button("← Game Library") { home() }
        text(app.optString("name", "Application details"), 28f, Color.WHITE, true)
        val state = report.optString("state"); text(state, 12f, statusColor(state), true)
        val metadata = card()
        for ((name, value) in listOf("Bundle ID" to app.optString("bundleId"), "Version / build" to "${app.optString("version")} / ${app.optString("build")}", "Architecture" to architectures(report), "IPA size" to formatBytes(app.optLong("fileSize")), "Executable" to app.optString("executable"))) {
            text(name.uppercase(), 10f, muted, true, metadata); text(value, 15f, parent = metadata)
        }
        text("Compatibility report", 22f, Color.WHITE, true)
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
        text("Native build result", 22f, Color.WHITE, true)
        text("Use the repository's host converter to build the verified leaf subset. The Android app does not contain an SDK/NDK toolchain. UIKit, Swift, graphics, audio and general game conversion are not implemented.", 14f, muted)
        button("Copy host build command") {
            (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).setPrimaryClip(android.content.ClipData.newPlainText("Host build command", "python3 -m radek convert input.ipa --authorized --output workspace/result"))
            Toast.makeText(this, "Command copied", Toast.LENGTH_SHORT).show()
        }
        if (app.has("sha256")) button("Attach host-built APK") {
            if (!Jobs.busy) startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "application/vnd.android.package-archive"; addCategory(Intent.CATEGORY_OPENABLE) }, pickerApk)
        }
        if (File(dir, "RadekiOSConventor-debug.apk").isFile) {
            text("Host result attached. Package identity and provenance matched. This is not a replacement for host signature validation; Android verifies the APK during installation.", 13f, accent)
            button("Install APK", true) { install(dir) }
            button("Open installed app") {
                val pkg = "dev.radek.converted.p" + app.getString("sha256").take(20)
                val intent = packageManager.getLaunchIntentForPackage(pkg)
                if (intent == null) Toast.makeText(this, "App is not installed or not visible to Android", Toast.LENGTH_LONG).show() else startActivity(intent)
            }
        }
        button("Delete library entry") {
            if (!Jobs.busy) AlertDialog.Builder(this).setTitle("Delete imported entry?").setMessage("Removes reports, icon and attached APK from this device.")
                .setNegativeButton("Cancel", null).setPositiveButton("Delete") { _, _ -> dir.deleteRecursively(); home() }.show()
        }
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
            Jobs.run { Library(context).import(uri) { Jobs.update(it) } }
            wasBusy = true; home()
        } else if (requestCode == pickerApk) {
            val dir = selected ?: return
            Jobs.run { attachApk(context, uri, dir); Jobs.update("Host APK attached") }
            wasBusy = true; home()
        }
    }
    private fun install(dir: File) {
        if (!packageManager.canRequestPackageInstalls()) {
            startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName"))); return
        }
        val uri = Uri.parse("content://dev.radek.conventor.results/${dir.name}/RadekiOSConventor-debug.apk")
        startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(uri, "application/vnd.android.package-archive").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
    }
    companion object {
        private fun attachApk(context: android.content.Context, uri: Uri, dir: File) {
            val temporary = File(dir, "result.pending")
            try {
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input); temporary.outputStream().use { output ->
                        val buffer = ByteArray(65536); var total = 0L
                        while (true) { val n = input.read(buffer); if (n < 0) break; total += n; require(total <= SafeZip.MAX_ARCHIVE); output.write(buffer, 0, n) }
                    }
                }
                val report = JSONObject(File(dir, "report.json").readText())
                val hash = report.getJSONObject("application").getString("sha256")
                val expectedPackage = "dev.radek.converted.p" + hash.take(20)
                ZipFile(temporary).use { zip ->
                    val entry = zip.getEntry("assets/conversion.json") ?: error("conversion provenance missing")
                    require(entry.size in 1..4194304)
                    val metadata = JSONObject(zip.getInputStream(entry).bufferedReader().use { it.readText() })
                    require(metadata.getString("package") == expectedPackage && metadata.getJSONObject("source").getString("sha256") == hash) { "APK does not match this IPA" }
                    require(zip.getEntry("lib/arm64-v8a/libconverted.so") != null) { "converted ARM64 library missing" }
                }
                @Suppress("DEPRECATION")
                val pkg = context.packageManager.getPackageArchiveInfo(temporary.path, android.content.pm.PackageManager.GET_ACTIVITIES or android.content.pm.PackageManager.GET_SIGNATURES)
                    ?: error("Android cannot parse APK")
                require(pkg.packageName == expectedPackage && pkg.activities.orEmpty().any { it.name == "dev.radek.generated.MainActivity" }) { "Android package/entry mismatch" }
                @Suppress("DEPRECATION")
                require(!pkg.signatures.isNullOrEmpty()) { "APK signer missing" }
                require(temporary.renameTo(File(dir, "RadekiOSConventor-debug.apk")))
            } finally { temporary.delete() }
        }
    }
}
