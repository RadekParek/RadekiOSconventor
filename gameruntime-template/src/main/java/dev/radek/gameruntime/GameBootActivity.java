package dev.radek.gameruntime;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.zip.CRC32;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * Launcher of a game-runtime boot-attempt APK (contract "game-runtime-v1").
 *
 * <p>The APK embeds one authorized IPA main executable and its bundle assets.
 * This activity runs in fullscreen and plays the recovered bundle splash screen
 * (supporting standard PNG/JPEG, Apple CgBI PNGs, and sprite-sheet descriptors
 * such as SPLASHES.png + SPLASHES.dat) as an automatically advancing boot
 * animation while running the real guest boot through libcompat_runtime_v1.so.
 * The splash never needs a touch: it advances on its own while the guest boots
 * and stops on a stable frame when the boot attempt ends. The device runner has
 * no arbitrary instruction or wall-clock budget, so a game loop is allowed to
 * remain alive for gameplay; it stops only for a real runtime boundary or setup
 * failure. When that happens, the fullscreen diagnostic panel stays open with
 * the exact stop reason instead of crashing.
 */
public final class GameBootActivity extends Activity {
    private static final String RUNTIME_LIBRARY = "compat_runtime_v1";
    private static final String EXECUTABLE_ASSET = "gameboot/main-executable.bin";
    private static final String METADATA_ASSET = "gameboot.json";
    private static final long MAX_EXECUTABLE_BYTES = 256L * 1024L * 1024L;
    private static final long MAX_METADATA_BYTES = 4L * 1024L * 1024L;
    /** Asset directory that carries the guest's own bundle payload. */
    private static final String PAYLOAD_ASSET_ROOT = "bundle";
    private static final String PAYLOAD_MARKER_PREFIX = ".radek-payload-";
    private static final long MAX_PAYLOAD_FILE_BYTES = 1024L * 1024L * 1024L;
    private static final int MAX_PAYLOAD_DEPTH = 24;
    private static final int MAX_SPLASH_IMAGE_BYTES = 16 * 1024 * 1024;
    private static final int MAX_SPLASH_DIMENSION = 4096;
    private static final int MAX_CGBI_INFLATED_BYTES = 16 * 1024 * 1024;
    private static final int DEFAULT_VIEWPORT_WIDTH = 480;
    private static final int DEFAULT_VIEWPORT_HEIGHT = 320;
    /** Automatic boot-animation interval between recovered splash frames. */
    private static final long SPLASH_FRAME_INTERVAL_MS = 900L;
    private static final byte[] PNG_SIGNATURE = new byte[] {
        (byte) 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };
    private static final byte[] CGBI_CHUNK_TYPE = new byte[] { 0x43, 0x67, 0x42, 0x49 };

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private TextView titleView;
    private TextView logView;
    private ScrollView scroller;
    private ImageView splashImageView;
    private TextView splashCaptionView;
    private SurfaceView gameSurfaceView;
    // Surface callbacks can run before System.loadLibrary completes. Retain the
    // latest holder surface so the runtime gets the real window as soon as its
    // JNI entry is available instead of silently staying on an offscreen pbuffer.
    private volatile Surface latestGameSurface;
    private LinearLayout overlayView;
    private final List<SplashFrame> activeSplashFrames = new ArrayList<>();
    private int currentSplashIndex = 0;
    private boolean splashAnimationRunning = false;
    private boolean bootFinished = false;
    private volatile boolean destroyed = false;

    private final Runnable splashAdvance = new Runnable() {
        @Override
        public void run() {
            if (destroyed || bootFinished || activeSplashFrames.size() <= 1) {
                splashAnimationRunning = false;
                return;
            }
            // Every recovered splash frame is shown once and the sequence then
            // stays on the last frame: the boot screen never cycles back.
            if (currentSplashIndex >= activeSplashFrames.size() - 1) {
                splashAnimationRunning = false;
                return;
            }
            showSplashFrame(currentSplashIndex + 1);
            if (currentSplashIndex < activeSplashFrames.size() - 1) {
                mainHandler.postDelayed(this, SPLASH_FRAME_INTERVAL_MS);
            } else {
                splashAnimationRunning = false;
            }
        }
    };

    private static native String runGameBootAttempt(byte[] mainBinary, String payloadDirectory,
            boolean authorizationConfirmed);

    /**
     * Hands the on-screen surface to the runtime so the guest's EAGL drawable can
     * present to it through EGL; a null surface detaches it again. The native
     * runtime is absent in unit tests, so publishing is guarded.
     */
    private static native void setGameSurface(Surface surface);

    private void publishGameSurface(Surface surface) {
        latestGameSurface = surface;
        try {
            setGameSurface(surface);
        } catch (Throwable ignored) {
            // Surface creation commonly wins the race with System.loadLibrary.
            // The boot thread retries latestGameSurface immediately after the
            // runtime loads; unit tests also run safely without JNI.
        }
    }

    /** Descriptor for a single sprite region inside a game splash sprite sheet (e.g. SPLASHES.dat). */
    public static final class SplashSpriteEntry {
        public final String name;
        public final int x;
        public final int y;
        public final int width;
        public final int height;
        public final int pivotX;
        public final int pivotY;

        public SplashSpriteEntry(
                String name,
                int x,
                int y,
                int width,
                int height,
                int pivotX,
                int pivotY) {
            this.name = name;
            this.x = x;
            this.y = y;
            this.width = width;
            this.height = height;
            this.pivotX = pivotX;
            this.pivotY = pivotY;
        }
    }

    /** Parsed representation of a binary sprite-sheet descriptor such as SPLASHES.dat. */
    public static final class SplashSheetDescriptor {
        public final String sheetName;
        public final List<SplashSpriteEntry> entries;

        public SplashSheetDescriptor(String sheetName, List<SplashSpriteEntry> entries) {
            this.sheetName = sheetName;
            this.entries = Collections.unmodifiableList(new ArrayList<>(entries));
        }
    }

    private static final class SplashFrame {
        final String label;
        final String sourcePath;
        final Bitmap bitmap;
        final int width;
        final int height;

        SplashFrame(String label, String sourcePath, Bitmap bitmap, int width, int height) {
            this.label = label;
            this.sourcePath = sourcePath;
            this.bitmap = bitmap;
            this.width = width;
            this.height = height;
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void appendLine(final String line) {
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                if (destroyed || logView == null) return;
                logView.append(line);
                logView.append("\n");
                if (scroller != null) {
                    scroller.post(new Runnable() {
                        @Override
                        public void run() {
                            scroller.fullScroll(ScrollView.FOCUS_DOWN);
                        }
                    });
                }
            }
        });
    }

    /**
     * Report a terminal boot result without throwing on Android's main thread.
     * A missing import or runtime dependency should leave useful diagnostics on
     * screen, not turn a handled boot failure into an application crash. The
     * splash animation stops on its current frame so the stop reason is stable.
     */
    private void showTerminalState(final String title, final String detail) {
        bootFinished = true;
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                if (destroyed || titleView == null || logView == null) return;
                splashAnimationRunning = false;
                mainHandler.removeCallbacks(splashAdvance);
                // The attempt is over: turn the device back to portrait and show
                // the diagnostics the guest produced.
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
                if (gameSurfaceView != null) gameSurfaceView.setVisibility(View.GONE);
                if (overlayView != null) overlayView.setVisibility(View.VISIBLE);
                titleView.setText(title);
                titleView.setTextColor(Color.rgb(255, 190, 92));
                logView.append(detail);
                logView.append("\n");
                logView.append("The diagnostic screen will remain open. This APK is not a playable conversion.\n");
                scrollToBottom();
            }
        });
    }

    private void scrollToBottom() {
        if (scroller == null) return;
        scroller.post(new Runnable() {
            @Override
            public void run() {
                if (!destroyed && scroller != null) scroller.fullScroll(ScrollView.FOCUS_DOWN);
            }
        });
    }

    private byte[] readAssetBounded(String name, long maximum) throws Exception {
        InputStream input = getAssets().open(name);
        try {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[65536];
            long total = 0;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > maximum) throw new IllegalStateException("embedded asset exceeds its limit: " + name);
                output.write(buffer, 0, count);
            }
            if (total == 0) throw new IllegalStateException("embedded asset is empty: " + name);
            return output.toByteArray();
        } finally {
            try {
                input.close();
            } catch (Exception ignored) {
                // Closing a fully-read asset stream cannot fail the boot.
            }
        }
    }

    private String summarizeBoot(String reportText) {
        try {
            JSONObject report = new JSONObject(reportText);
            JSONObject loader = report.optJSONObject("loader");
            JSONObject execution = report.optJSONObject("execution");
            StringBuilder summary = new StringBuilder();
            summary.append("Loader: ").append(loader != null ? loader.optString("status", "?") : "?");
            if (loader != null) {
                summary.append(" (").append(loader.optInt("resolvedSymbolCount", 0)).append(" resolved, ")
                        .append(loader.optInt("trappedSymbolCount", 0)).append(" trapped, ")
                        .append(loader.optInt("unresolvedSymbolCount", 0)).append(" unresolved)");
            }
            summary.append("\n");
            long executed = 0;
            String executionStatus = "";
            if (execution != null) {
                executed = execution.optLong("instructions", 0);
                executionStatus = execution.optString("status", "");
                summary.append("Executed ").append(executed).append(" guest instruction(s); ");
                summary.append("status ").append(executionStatus.isEmpty() ? "?" : executionStatus).append("\n");
            }
            // A JSON null must never be printed as the literal import name "null":
            // that produced a diagnostic claiming a stop at an unnamed import.
            if (!report.isNull("trappedImport")) {
                String trapped = report.optString("trappedImport", "");
                if (!trapped.isEmpty() && !"null".equals(trapped)) {
                    summary.append("Stopped at unimplemented import: ").append(trapped).append("\n");
                }
            } else if ("STOPPED_AT_TRAP".equals(executionStatus)) {
                summary.append("The guest reached an unimplemented import trap, but the report did not name it.\n");
            }
            JSONArray trappedSymbols = report.optJSONArray("trappedSymbols");
            if (trappedSymbols != null) summary.append("Trapped imports bound: ").append(trappedSymbols.length()).append("\n");
            String budgetNote = budgetStopDescription(executed, executionStatus);
            if (!budgetNote.isEmpty()) summary.append(budgetNote).append("\n");
            String reason = report.optString("reason", "");
            if (!reason.isEmpty()) summary.append(reason);
            return summary.toString();
        } catch (Exception error) {
            return "Unparseable boot report (" + error + "): " + reportText;
        }
    }

    /**
     * Human-readable sentence for a bounded-execution stop in a diagnostic
     * report. The device game path is unlimited, but old/host probe reports can
     * still contain an explicit instruction/time budget, a memory fault, or an
     * exception; each of those is a different statement and must not be reported
     * as if the guest had called an unimplemented import.
     */
    private static String budgetStopDescription(long executed, String status) {
        if ("TIME_LIMIT".equals(status) || "INSTRUCTION_LIMIT".equals(status)) {
            return "The guest was still executing real instructions when its bounded "
                    + ("TIME_LIMIT".equals(status) ? "time" : "instruction")
                    + " budget (" + executed + " instruction(s)) expired. "
                    + "No unimplemented import was reached during this window.";
        }
        if ("MEMORY_FAULT".equals(status)) {
            return "Guest execution stopped on a guest-visible memory fault.";
        }
        if ("GUEST_EXCEPTION_RAISED".equals(status)) {
            return "Guest execution stopped because the guest raised an Objective-C exception.";
        }
        if ("EXECUTION_FAULT".equals(status)) {
            return "Guest execution stopped on an execution fault reported by the CPU backend.";
        }
        if ("BACKEND_UNAVAILABLE".equals(status)) {
            return "The guest CPU backend was not available on this device.";
        }
        return "";
    }

    private String bootStatus(String reportText) {
        try {
            JSONObject execution = new JSONObject(reportText).optJSONObject("execution");
            return execution != null ? execution.optString("status", "") : "";
        } catch (Exception ignored) {
            return "";
        }
    }

    // Package-private so the launcher template's Robolectric test can exercise
    // the same blocked-guest path used after runGameBootAttempt returns.
    void displayBootResult(String reportText) {
        String safeReport = reportText != null ? reportText : "{}";
        appendLine(summarizeBoot(safeReport));
        String status = bootStatus(safeReport);
        String trapped = "";
        try {
            JSONObject report = new JSONObject(safeReport);
            if (!report.isNull("trappedImport")) trapped = report.optString("trappedImport", "");
        } catch (Exception ignored) {
            trapped = "";
        }
        if ("RETURNED".equals(status)) {
            showTerminalState("Guest entry returned", "Guest entry returned without a game lifecycle.");
        } else if ("TIME_LIMIT".equals(status) || "INSTRUCTION_LIMIT".equals(status)) {
            showTerminalState(
                    "Guest boot budget reached",
                    "Guest execution ran its real startup code and then hit the bounded "
                            + ("TIME_LIMIT".equals(status) ? "time" : "instruction")
                            + " budget before reaching an unimplemented import. Nothing crashed.");
        } else if ("MEMORY_FAULT".equals(status) || "EXECUTION_FAULT".equals(status)) {
            showTerminalState("Guest boot faulted", "Guest execution stopped on a memory/execution fault.");
        } else if ("GUEST_EXCEPTION_RAISED".equals(status)) {
            showTerminalState("Guest boot raised a guest exception", "Guest execution stopped on an Objective-C exception.");
        } else if ("BACKEND_UNAVAILABLE".equals(status)) {
            showTerminalState("Guest CPU backend unavailable", "This device build has no working guest CPU backend.");
        } else if (!trapped.isEmpty()) {
            showTerminalState("Guest boot stopped", "Guest execution stopped at the unimplemented import " + trapped + ".");
        } else {
            showTerminalState("Guest boot stopped", "Guest execution stopped at a missing or unimplemented runtime call.");
        }
    }

    /**
     * Fullscreen boot screen. While the guest runs, only the game is visible:
     * the recovered splash covers the display until the guest's own EGL frames
     * take over, and the diagnostic panel stays hidden. No viewport tap is needed
     * or accepted for frame cycling — the splash advances by itself, once per
     * frame. When the attempt stops, {@link #showTerminalState} switches back to
     * portrait and reveals the diagnostics.
     */
    private void applyFullscreenMode() {
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(Color.TRANSPARENT);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attributes);
        }
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !destroyed) applyFullscreenMode();
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        applyFullscreenMode();
        // A guest game runs in landscape; both directions are allowed so the
        // device can be turned left or right. showTerminalState() returns to
        // portrait for the diagnostic log.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        FrameLayout root = new FrameLayout(this);
        // Keep the game viewport genuinely black. The old blue-black value was
        // visible in the bars around the recovered 480x320 splash and made the
        // forced game APK look like a diagnostic shell.
        root.setBackgroundColor(Color.BLACK);

        splashImageView = new ImageView(this);
        splashImageView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        splashImageView.setContentDescription("Recovered game splash screen");
        root.addView(splashImageView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));

        // The guest's frames land on this surface (the runtime attaches the
        // EAGL drawable to it through EGL). It sits above the splash so the game
        // covers the boot screen as soon as it renders.
        gameSurfaceView = new SurfaceView(this);
        gameSurfaceView.setBackgroundColor(Color.BLACK);
        gameSurfaceView.setContentDescription("Guest game surface");
        gameSurfaceView.setZOrderOnTop(true);
        gameSurfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                publishGameSurface(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                publishGameSurface(holder.getSurface());
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                publishGameSurface(null);
            }
        });
        root.addView(gameSurfaceView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.CENTER));

        LinearLayout overlay = new LinearLayout(this);
        overlay.setOrientation(LinearLayout.VERTICAL);
        overlay.setPadding(dp(14), dp(14), dp(14), dp(14));

        titleView = new TextView(this);
        titleView.setText("Game boot attempt");
        titleView.setTextSize(18);
        titleView.setTextColor(Color.WHITE);
        titleView.setTypeface(null, Typeface.BOLD);
        titleView.setGravity(Gravity.CENTER_HORIZONTAL);
        titleView.setShadowLayer(6f, 0f, 0f, Color.BLACK);
        overlay.addView(titleView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView splashHeader = new TextView(this);
        splashHeader.setText("GAME SPLASH SCREEN VIEWPORT");
        splashHeader.setTextSize(10);
        splashHeader.setTypeface(null, Typeface.BOLD);
        splashHeader.setTextColor(Color.rgb(170, 208, 255));
        splashHeader.setGravity(Gravity.CENTER_HORIZONTAL);
        splashHeader.setShadowLayer(6f, 0f, 0f, Color.BLACK);
        overlay.addView(splashHeader, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        overlay.addView(new View(this), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f));

        splashCaptionView = new TextView(this);
        splashCaptionView.setText("Searching packaged bundle for game splash assets…");
        splashCaptionView.setTextSize(10);
        splashCaptionView.setTextColor(Color.rgb(205, 216, 230));
        splashCaptionView.setShadowLayer(6f, 0f, 0f, Color.BLACK);
        splashCaptionView.setPadding(0, 0, 0, dp(6));
        overlay.addView(splashCaptionView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        GradientDrawable logBg = new GradientDrawable();
        logBg.setColor(Color.argb(232, 0, 0, 0));
        logBg.setCornerRadius(dp(10));
        logBg.setStroke(dp(1), Color.rgb(64, 64, 64));

        logView = new TextView(this);
        logView.setTextSize(11);
        logView.setTextColor(Color.rgb(92, 227, 181));
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setPadding(dp(10), dp(10), dp(10), dp(10));
        logView.setTextIsSelectable(true);
        scroller = new ScrollView(this);
        scroller.setBackground(logBg);
        scroller.addView(logView, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams scrollerParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.05f);
        overlay.addView(scroller, scrollerParams);

        root.addView(overlay, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        overlayView = overlay;
        // While the guest runs the user sees only the game; the diagnostics are
        // revealed by showTerminalState() once the attempt stops.
        overlayView.setVisibility(View.GONE);

        setContentView(root);

        preloadSplashFromAssets();

        String appName = "";
        JSONObject metadata = null;
        try {
            byte[] metadataBytes = readAssetBounded(METADATA_ASSET, MAX_METADATA_BYTES);
            metadata = new JSONObject(new String(metadataBytes, StandardCharsets.UTF_8));
            appName = metadata.optString("applicationName", "");
        } catch (Exception error) {
            appendLine("Boot metadata is missing or invalid: " + error);
            showTerminalState("Boot attempt could not start", "The diagnostic screen will remain open so this error can be reviewed.");
            return;
        }
        final JSONObject finalMetadata = metadata;
        final String displayName = appName.isEmpty() ? "embedded game" : appName;
        appendLine("Booting " + displayName + " (game-runtime-v1)...");
        appendLine("Loading the compatibility runtime library...");

        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    List<SplashFrame> discovered = discoverSplashFramesFromInventory(getAssets(), finalMetadata);
                    if (!discovered.isEmpty()) {
                        mainHandler.post(new Runnable() {
                            @Override
                            public void run() {
                                if (!destroyed && (activeSplashFrames.isEmpty()
                                        || discovered.size() > activeSplashFrames.size())) {
                                    installSplashFrames(discovered);
                                }
                            }
                        });
                    }
                } catch (Throwable ignored) {
                    // Splash discovery is non-fatal to the guest boot attempt.
                }

                try {
                    try {
                        System.loadLibrary("unicorn");
                    } catch (Throwable ignored) {
                        // libcompat_runtime_v1 may be linked directly or carry its own dependency.
                    }
                    System.loadLibrary(RUNTIME_LIBRARY);
                    // surfaceCreated/surfaceChanged can both have fired before
                    // the library was loaded. Re-publish the retained surface
                    // now, before the guest creates its EAGL drawable.
                    publishGameSurface(latestGameSurface);
                } catch (Throwable error) {
                    appendLine("Runtime library failed to load: " + error);
                    showTerminalState("Runtime library unavailable", "Check that the APK includes every native dependency. The diagnostic screen will remain open.");
                    return;
                }
                appendLine("Runtime library loaded.");
                final byte[] executable;
                try {
                    executable = readAssetBounded(EXECUTABLE_ASSET, MAX_EXECUTABLE_BYTES);
                } catch (Throwable error) {
                    appendLine("Embedded executable could not be read: " + error);
                    showTerminalState("Executable unavailable", "The boot attempt could not continue; the diagnostic screen will remain open.");
                    return;
                }
                appendLine("Executable loaded: " + executable.length + " byte(s). Mapping and binding traps...");
                final String payloadDirectory = ensurePayloadExtracted(executable);
                final String reportText;
                try {
                    reportText = runGameBootAttempt(executable, payloadDirectory, true);
                } catch (Throwable error) {
                    appendLine("Boot attempt failed inside the runtime: " + error);
                    showTerminalState("Guest boot failed", "The runtime could not complete the boot attempt; diagnostics will remain visible.");
                    return;
                }
                displayBootResult(reportText);
            }
        }, "game-boot").start();
    }

    /**
     * Extracts the bundled game payload (`assets/bundle/**`) into the app's files
     * directory so the runtime can serve the guest's own file reads from it, and
     * returns the app directory — or null when this artifact has no payload.
     * The writable application directories the runtime mounts are created next to
     * it. Extraction is versioned by the embedded executable, so a new artifact
     * never reuses a stale payload directory.
     */
    private String ensurePayloadExtracted(byte[] executable) {
        try {
            AssetManager assets = getAssets();
            String[] root = assets.list(PAYLOAD_ASSET_ROOT);
            if (root == null || root.length == 0) return null;
            File bundleRoot = new File(getFilesDir(), "bundle");
            File appDirectory = new File(bundleRoot, "App.app");
            File home = new File(bundleRoot, "radek-home");
            File marker = new File(bundleRoot, PAYLOAD_MARKER_PREFIX + payloadVersion(executable));
            mkdirsOrThrow(new File(home, "Documents"));
            mkdirsOrThrow(new File(home, "Library"));
            if (!marker.isFile()) {
                int files = extractAssetTree(assets, PAYLOAD_ASSET_ROOT, appDirectory, 0);
                if (files <= 0) return null;
                if (!marker.exists() && !marker.createNewFile()) {
                    appendLine("Payload marker could not be written; the payload will be re-extracted.");
                }
                appendLine("Game payload extracted: " + files + " file(s) from the APK.");
            }
            return appDirectory.isDirectory() ? appDirectory.getAbsolutePath() : null;
        } catch (Throwable error) {
            appendLine("Game payload extraction failed: " + error);
            return null;
        }
    }

    private static String payloadVersion(byte[] executable) {
        CRC32 crc = new CRC32();
        if (executable != null) crc.update(executable);
        return (executable != null ? executable.length : 0) + "-" + crc.getValue();
    }

    private static void mkdirsOrThrow(File directory) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs() && !directory.isDirectory())
            throw new IOException("cannot create " + directory);
    }

    private int extractAssetTree(AssetManager assets, String assetPath, File target, int depth)
            throws IOException {
        if (depth > MAX_PAYLOAD_DEPTH)
            throw new IOException("payload nesting is too deep at " + assetPath);
        String[] children = assets.list(assetPath);
        if (children == null || children.length == 0) return 0;
        int extracted = 0;
        for (String child : children) {
            String childPath = assetPath + "/" + child;
            String[] grandChildren = assets.list(childPath);
            if (grandChildren != null && grandChildren.length > 0) {
                extracted += extractAssetTree(assets, childPath, new File(target, child), depth + 1);
                continue;
            }
            File file = new File(target, child);
            mkdirsOrThrow(target);
            try {
                InputStream input = assets.open(childPath);
                try {
                    OutputStream output = new FileOutputStream(file);
                    try {
                        byte[] buffer = new byte[64 * 1024];
                        long total = 0;
                        int read;
                        while ((read = input.read(buffer)) > 0) {
                            total += read;
                            if (total > MAX_PAYLOAD_FILE_BYTES)
                                throw new IOException("payload file is too large: " + childPath);
                            output.write(buffer, 0, read);
                        }
                    } finally {
                        output.close();
                    }
                } finally {
                    input.close();
                }
                extracted++;
            } catch (FileNotFoundException directory) {
                // An empty directory in the payload: keep the directory itself.
                mkdirsOrThrow(file);
            }
        }
        return extracted;
    }

    private void preloadSplashFromAssets() {
        try {
            List<SplashFrame> preloaded = discoverSplashFramesFromAssets(getAssets());
            if (!preloaded.isEmpty()) {
                installSplashFrames(preloaded);
            }
        } catch (Throwable ignored) {
            // Non-fatal; worker thread will also scan resourceInventory.
        }
    }

    /**
     * Install recovered splash frames and start the automatic boot animation.
     * The animation is driven only by the launcher: touches never cycle frames,
     * and the sequence stops on a stable frame when the boot attempt ends.
     */
    private void installSplashFrames(List<SplashFrame> frames) {
        if (frames == null || frames.isEmpty() || destroyed) return;
        boolean wasEmpty = activeSplashFrames.isEmpty();
        activeSplashFrames.clear();
        activeSplashFrames.addAll(frames);
        currentSplashIndex = 0;
        showSplashFrame(0);
        if (wasEmpty && !bootFinished && activeSplashFrames.size() > 1 && !splashAnimationRunning) {
            splashAnimationRunning = true;
            mainHandler.postDelayed(splashAdvance, SPLASH_FRAME_INTERVAL_MS);
        }
    }

    /** Test hook: number of recovered splash frames currently installed. */
    int installedSplashFrameCount() {
        return activeSplashFrames.size();
    }

    /** Test hook: index of the splash frame the boot screen currently shows. */
    int currentSplashFrameIndex() {
        return currentSplashIndex;
    }

    /** Test hook: true while the diagnostic panel is on screen. */
    boolean diagnosticsOverlayVisible() {
        return overlayView != null && overlayView.getVisibility() == View.VISIBLE;
    }

    /**
     * Test hook: install synthetic frames and run the launcher's once-through
     * advance rule. Unit tests run without assets, so the launcher has already
     * reached its terminal state; that flag is cleared here so the boot-screen
     * advance can be exercised.
     */
    void installSyntheticSplashFramesForTest(int count) {
        bootFinished = false;
        mainHandler.removeCallbacks(splashAdvance);
        splashAnimationRunning = false;
        List<SplashFrame> frames = new ArrayList<>();
        for (int index = 0; index < count; index++) {
            frames.add(new SplashFrame("frame " + index, "test",
                    Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888), 8, 8));
        }
        installSplashFrames(frames);
    }

    private void showSplashFrame(int index) {
        if (activeSplashFrames.isEmpty() || splashImageView == null || splashCaptionView == null) return;
        // Clamped, never wrapped: the last frame stays on screen.
        currentSplashIndex = Math.max(0, Math.min(index, activeSplashFrames.size() - 1));
        SplashFrame frame = activeSplashFrames.get(currentSplashIndex);
        splashImageView.setImageBitmap(frame.bitmap);
        String advanceHint = activeSplashFrames.size() > 1
                ? " · Frame " + (currentSplashIndex + 1) + "/" + activeSplashFrames.size()
                        + (bootFinished ? " · boot finished" : " · advancing automatically")
                : "";
        splashCaptionView.setText(
                "Rendered splash: " + frame.sourcePath + " [" + frame.label + " "
                        + frame.width + "×" + frame.height + "]" + advanceHint);
    }

    /**
     * Parses a binary sprite-sheet descriptor (`SPLASHES.dat` format: big-endian length-prefixed
     * texture filename, big-endian entry count, followed by length-prefixed sprite names and
     * 6 big-endian uint16 coordinates: x, y, width, height, pivotX, pivotY).
     */
    public static SplashSheetDescriptor parseSplashSheetDescriptor(byte[] data) {
        if (data == null || data.length < 6) return null;
        int offset = 0;
        int sheetNameLen = readU16Be(data, offset);
        offset += 2;
        if (sheetNameLen <= 0 || sheetNameLen > 256 || offset + sheetNameLen + 2 > data.length) {
            return null;
        }
        String sheetName = new String(data, offset, sheetNameLen, StandardCharsets.US_ASCII);
        offset += sheetNameLen;
        int entryCount = readU16Be(data, offset);
        offset += 2;
        if (entryCount <= 0 || entryCount > 512) return null;

        List<SplashSpriteEntry> entries = new ArrayList<>(entryCount);
        for (int i = 0; i < entryCount; i++) {
            if (offset + 2 > data.length) return null;
            int nameLen = readU16Be(data, offset);
            offset += 2;
            if (nameLen <= 0 || nameLen > 256 || offset + nameLen + 12 > data.length) {
                return null;
            }
            String name = new String(data, offset, nameLen, StandardCharsets.US_ASCII);
            offset += nameLen;
            int x = readU16Be(data, offset);
            int y = readU16Be(data, offset + 2);
            int width = readU16Be(data, offset + 4);
            int height = readU16Be(data, offset + 6);
            int pivotX = readU16Be(data, offset + 8);
            int pivotY = readU16Be(data, offset + 10);
            offset += 12;
            if (width <= 0 || height <= 0 || width > MAX_SPLASH_DIMENSION || height > MAX_SPLASH_DIMENSION) {
                return null;
            }
            entries.add(new SplashSpriteEntry(name, x, y, width, height, pivotX, pivotY));
        }
        return new SplashSheetDescriptor(sheetName, entries);
    }

    /** Returns true if the byte array is an Apple CgBI PNG image. */
    public static boolean isCgbiPng(byte[] data) {
        if (data == null || data.length < 33 || data.length > MAX_SPLASH_IMAGE_BYTES) return false;
        for (int i = 0; i < PNG_SIGNATURE.length; i++) {
            if (data[i] != PNG_SIGNATURE[i]) return false;
        }
        int limit = Math.min(data.length, 512) - CGBI_CHUNK_TYPE.length;
        for (int i = 8; i <= limit; i++) {
            boolean match = true;
            for (int j = 0; j < CGBI_CHUNK_TYPE.length; j++) {
                if (data[i + j] != CGBI_CHUNK_TYPE[j]) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    /**
     * Decodes an Apple CgBI 8-bit RGB/RGBA PNG into straight-alpha ARGB_8888 pixels.
     * Writes {width, height} into outDimensions (length >= 2).
     */
    public static int[] decodeCgbiRgbaPixels(byte[] data, int[] outDimensions) {
        if (!isCgbiPng(data) || outDimensions == null || outDimensions.length < 2) {
            throw new IllegalArgumentException("Invalid CgBI PNG input");
        }
        int p = PNG_SIGNATURE.length;
        int width = 0;
        int height = 0;
        int depth = 0;
        int colorType = -1;
        boolean sawHeader = false;
        boolean sawEnd = false;
        ByteArrayOutputStream compressed = new ByteArrayOutputStream();

        while (p < data.length) {
            if (data.length - p < 12) throw new IllegalArgumentException("Truncated PNG chunk");
            long chunkLenLong = readU32Be(data, p);
            if (chunkLenLong < 0 || chunkLenLong > data.length - p - 12L) {
                throw new IllegalArgumentException("PNG chunk out of bounds");
            }
            int chunkLen = (int) chunkLenLong;
            int typeOffset = p + 4;
            int payloadOffset = p + 8;
            int crcOffset = payloadOffset + chunkLen;
            String type = new String(data, typeOffset, 4, StandardCharsets.US_ASCII);
            if (!"CgBI".equals(type)) {
                CRC32 crc = new CRC32();
                crc.update(data, typeOffset, 4 + chunkLen);
                if (crc.getValue() != readU32Be(data, crcOffset)) {
                    throw new IllegalArgumentException("Invalid PNG chunk CRC for " + type);
                }
            }
            if ("IHDR".equals(type)) {
                if (sawHeader || chunkLen != 13) throw new IllegalArgumentException("Invalid IHDR");
                width = (int) readU32Be(data, payloadOffset);
                height = (int) readU32Be(data, payloadOffset + 4);
                depth = data[payloadOffset + 8] & 0xff;
                colorType = data[payloadOffset + 9] & 0xff;
                int interlace = data[payloadOffset + 12] & 0xff;
                if (width <= 0 || height <= 0 || width > MAX_SPLASH_DIMENSION || height > MAX_SPLASH_DIMENSION) {
                    throw new IllegalArgumentException("Invalid CgBI dimensions");
                }
                if (depth != 8 || (colorType != 2 && colorType != 6) || interlace != 0) {
                    throw new IllegalArgumentException("Unsupported CgBI color format");
                }
                sawHeader = true;
            } else if ("IDAT".equals(type)) {
                if (!sawHeader || sawEnd) throw new IllegalArgumentException("Unexpected IDAT");
                compressed.write(data, payloadOffset, chunkLen);
            } else if ("IEND".equals(type)) {
                sawEnd = true;
                break;
            }
            p = crcOffset + 4;
        }
        if (!sawHeader || !sawEnd || compressed.size() == 0) {
            throw new IllegalArgumentException("Incomplete CgBI image");
        }
        int channels = (colorType == 6) ? 4 : 3;
        long strideLong = (long) width * channels;
        long expectedLong = (long) height * (strideLong + 1L);
        if (expectedLong <= 0 || expectedLong > MAX_CGBI_INFLATED_BYTES) {
            throw new IllegalArgumentException("CgBI image exceeds inflation limit");
        }
        byte[] rows = inflateCgbiPayload(compressed.toByteArray(), (int) expectedLong);
        int stride = (int) strideLong;
        byte[] previous = new byte[stride];
        byte[] row = new byte[stride];
        int[] colors = new int[width * height];
        int src = 0;
        for (int y = 0; y < height; y++) {
            int filter = rows[src++] & 0xff;
            for (int x = 0; x < stride; x++) {
                int raw = rows[src++] & 0xff;
                int left = (x >= channels) ? (row[x - channels] & 0xff) : 0;
                int above = previous[x] & 0xff;
                int upperLeft = (x >= channels) ? (previous[x - channels] & 0xff) : 0;
                int predictor;
                switch (filter) {
                    case 0: predictor = 0; break;
                    case 1: predictor = left; break;
                    case 2: predictor = above; break;
                    case 3: predictor = (left + above) >>> 1; break;
                    case 4: predictor = paeth(left, above, upperLeft); break;
                    default: throw new IllegalArgumentException("Invalid PNG filter: " + filter);
                }
                row[x] = (byte) ((raw + predictor) & 0xff);
            }
            int rowBase = y * width;
            for (int x = 0; x < width; x++) {
                int idx = x * channels;
                int blue = row[idx] & 0xff;
                int green = row[idx + 1] & 0xff;
                int red = row[idx + 2] & 0xff;
                int alpha = (channels == 4) ? (row[idx + 3] & 0xff) : 255;
                if (alpha > 0 && alpha < 255) {
                    red = Math.min(255, (red * 255 + (alpha >>> 1)) / alpha);
                    green = Math.min(255, (green * 255 + (alpha >>> 1)) / alpha);
                    blue = Math.min(255, (blue * 255 + (alpha >>> 1)) / alpha);
                } else if (alpha == 0) {
                    red = 0;
                    green = 0;
                    blue = 0;
                }
                colors[rowBase + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
            System.arraycopy(row, 0, previous, 0, stride);
        }
        outDimensions[0] = width;
        outDimensions[1] = height;
        return colors;
    }

    private static byte[] inflateCgbiPayload(byte[] payload, int expected) {
        boolean[] modes = new boolean[] { true, false };
        for (boolean raw : modes) {
            Inflater inflater = new Inflater(raw);
            try {
                inflater.setInput(payload);
                byte[] output = new byte[expected];
                int written = 0;
                while (!inflater.finished() && written < expected) {
                    int count = inflater.inflate(output, written, expected - written);
                    if (count == 0) {
                        if (inflater.needsInput() || inflater.needsDictionary()) break;
                        throw new DataFormatException("Inflater stalled");
                    }
                    written += count;
                }
                if (written == expected) return output;
            } catch (DataFormatException ignored) {
                // Fall back to alternate header mode.
            } finally {
                inflater.end();
            }
        }
        throw new IllegalArgumentException("Could not inflate CgBI IDAT stream");
    }

    private static int paeth(int a, int b, int c) {
        int p = a + b - c;
        int pa = Math.abs(p - a);
        int pb = Math.abs(p - b);
        int pc = Math.abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        return (pb <= pc) ? b : c;
    }

    private static int readU16Be(byte[] data, int offset) {
        return ((data[offset] & 0xff) << 8) | (data[offset + 1] & 0xff);
    }

    private static long readU32Be(byte[] data, int offset) {
        return ((long) (data[offset] & 0xff) << 24)
                | ((long) (data[offset + 1] & 0xff) << 16)
                | ((long) (data[offset + 2] & 0xff) << 8)
                | ((long) (data[offset + 3] & 0xff));
    }

    private static Bitmap decodeBitmapOrCgbi(byte[] bytes) {
        if (bytes == null || bytes.length == 0 || bytes.length > MAX_SPLASH_IMAGE_BYTES) {
            return null;
        }
        if (isCgbiPng(bytes)) {
            try {
                int[] dims = new int[2];
                int[] colors = decodeCgbiRgbaPixels(bytes, dims);
                return Bitmap.createBitmap(colors, dims[0], dims[1], Bitmap.Config.ARGB_8888);
            } catch (Throwable ignored) {
                return null;
            }
        }
        try {
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            return BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static Bitmap composeOnViewport(Bitmap sprite, int viewportWidth, int viewportHeight) {
        if (sprite == null) return null;
        if (sprite.getWidth() >= viewportWidth && sprite.getHeight() >= viewportHeight) {
            return sprite;
        }
        int targetW = Math.max(viewportWidth, sprite.getWidth());
        int targetH = Math.max(viewportHeight, sprite.getHeight());
        Bitmap canvasBitmap = Bitmap.createBitmap(targetW, targetH, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(canvasBitmap);
        canvas.drawColor(Color.BLACK);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        float left = (targetW - sprite.getWidth()) * 0.5f;
        float top = (targetH - sprite.getHeight()) * 0.5f;
        canvas.drawBitmap(sprite, left, top, paint);
        return canvasBitmap;
    }

    private static List<SplashFrame> extractFramesFromSheet(
            String sheetSourcePath,
            Bitmap sheet,
            SplashSheetDescriptor descriptor) {
        if (sheet == null || descriptor == null || descriptor.entries.isEmpty()) {
            return Collections.emptyList();
        }
        List<SplashSpriteEntry> sorted = new ArrayList<>(descriptor.entries);
        Collections.sort(sorted, new Comparator<SplashSpriteEntry>() {
            @Override
            public int compare(SplashSpriteEntry a, SplashSpriteEntry b) {
                return Integer.compare(scoreSplashEntry(b), scoreSplashEntry(a));
            }
        });
        List<SplashFrame> result = new ArrayList<>();
        for (SplashSpriteEntry entry : sorted) {
            if (entry.x < 0 || entry.y < 0
                    || entry.x + entry.width > sheet.getWidth()
                    || entry.y + entry.height > sheet.getHeight()) {
                continue;
            }
            Bitmap cropped = Bitmap.createBitmap(sheet, entry.x, entry.y, entry.width, entry.height);
            Bitmap viewportBitmap = composeOnViewport(
                    cropped,
                    DEFAULT_VIEWPORT_WIDTH,
                    DEFAULT_VIEWPORT_HEIGHT);
            result.add(new SplashFrame(
                    entry.name,
                    sheetSourcePath,
                    viewportBitmap,
                    entry.width,
                    entry.height));
        }
        return result;
    }

    private static int scoreSplashEntry(SplashSpriteEntry entry) {
        String upper = entry.name.toUpperCase(Locale.US);
        int bonus = 0;
        if (upper.contains("ANGRY") || upper.contains("GAME") || upper.contains("TITLE")
                || upper.contains("MAIN")) {
            bonus += 1_000_000;
        } else if (upper.contains("ROVIO")) {
            bonus += 500_000;
        }
        return bonus + (entry.width * entry.height);
    }

    private List<SplashFrame> discoverSplashFramesFromAssets(AssetManager assets) {
        List<SplashFrame> frames = new ArrayList<>();
        String[] datCandidates = new String[] {
            "bundle/data/SPLASHES.dat",
            "bundle/SPLASHES.dat"
        };
        for (String datAsset : datCandidates) {
            byte[] datBytes = readOptionalAssetBytes(assets, datAsset, 256 * 1024);
            if (datBytes == null) continue;
            SplashSheetDescriptor descriptor = parseSplashSheetDescriptor(datBytes);
            if (descriptor == null) continue;
            String dirPrefix = datAsset.contains("/")
                    ? datAsset.substring(0, datAsset.lastIndexOf('/') + 1)
                    : "";
            String pngAsset = dirPrefix + descriptor.sheetName;
            byte[] pngBytes = readOptionalAssetBytes(assets, pngAsset, MAX_SPLASH_IMAGE_BYTES);
            Bitmap sheet = decodeBitmapOrCgbi(pngBytes);
            if (sheet != null) {
                String relPath = pngAsset.startsWith("bundle/")
                        ? pngAsset.substring("bundle/".length())
                        : pngAsset;
                frames.addAll(extractFramesFromSheet(relPath, sheet, descriptor));
            }
        }
        String[] launchCandidates = new String[] {
            "bundle/Default-Landscape.png",
            "bundle/Default-Landscape@2x.png",
            "bundle/Default-568h@2x.png",
            "bundle/Default@2x.png",
            "bundle/Default.png",
            "bundle/LaunchImage.png",
            "bundle/Splash.png",
            "bundle/splash.png",
            "bundle/data/MENU.png"
        };
        for (String candidate : launchCandidates) {
            byte[] bytes = readOptionalAssetBytes(assets, candidate, MAX_SPLASH_IMAGE_BYTES);
            Bitmap bmp = decodeBitmapOrCgbi(bytes);
            if (bmp != null) {
                String rel = candidate.startsWith("bundle/")
                        ? candidate.substring("bundle/".length())
                        : candidate;
                frames.add(new SplashFrame(
                        new File(rel).getName(),
                        rel,
                        bmp,
                        bmp.getWidth(),
                        bmp.getHeight()));
            }
        }
        if (frames.isEmpty()) {
            byte[] iconBytes = readOptionalAssetBytes(assets, "ipa-icon.png", MAX_SPLASH_IMAGE_BYTES);
            Bitmap icon = decodeBitmapOrCgbi(iconBytes);
            if (icon != null) {
                Bitmap composed = composeOnViewport(icon, DEFAULT_VIEWPORT_WIDTH, DEFAULT_VIEWPORT_HEIGHT);
                frames.add(new SplashFrame(
                        "ipa-icon.png",
                        "assets/ipa-icon.png",
                        composed,
                        icon.getWidth(),
                        icon.getHeight()));
            }
        }
        return frames;
    }

    private List<SplashFrame> discoverSplashFramesFromInventory(AssetManager assets, JSONObject manifest) {
        List<SplashFrame> frames = discoverSplashFramesFromAssets(assets);
        if (manifest == null) return frames;
        JSONArray inventory = manifest.optJSONArray("resourceInventory");
        if (inventory == null) return frames;
        for (int i = 0; i < inventory.length(); i++) {
            JSONObject item = inventory.optJSONObject(i);
            if (item == null) continue;
            String rel = item.optString("path", "");
            String lower = rel.toLowerCase(Locale.US);
            if (lower.endsWith(".dat") && lower.contains("splash")) {
                byte[] datBytes = readOptionalAssetBytes(assets, "bundle/" + rel, 256 * 1024);
                SplashSheetDescriptor descriptor = parseSplashSheetDescriptor(datBytes);
                if (descriptor == null) continue;
                String dirPrefix = rel.contains("/")
                        ? rel.substring(0, rel.lastIndexOf('/') + 1)
                        : "";
                String sheetRel = dirPrefix + descriptor.sheetName;
                Bitmap sheet = decodeBitmapOrCgbi(
                        readOptionalAssetBytes(assets, "bundle/" + sheetRel, MAX_SPLASH_IMAGE_BYTES));
                if (sheet != null && frames.isEmpty()) {
                    frames.addAll(extractFramesFromSheet(sheetRel, sheet, descriptor));
                }
            }
        }
        return frames;
    }

    // Retained for bundle-tree splash discovery and tests.
    List<SplashFrame> discoverSplashFramesFromBundle(File bundleDir) {
        return discoverSplashFramesFromAssets(getAssets());
    }

    static String sanitizeRelativePath(String rawPath) throws IOException {
        if (rawPath == null) throw new IOException("Asset path is null");
        String normalized = rawPath.replace('\\', '/').trim();
        if (normalized.isEmpty() || normalized.startsWith("/") || normalized.contains("\0")) {
            throw new IOException("Rejected unsafe asset path: " + rawPath);
        }
        String[] parts = normalized.split("/");
        StringBuilder clean = new StringBuilder();
        for (String part : parts) {
            if (part.isEmpty() || ".".equals(part) || "..".equals(part)) {
                throw new IOException("Rejected relative traversal segment in: " + rawPath);
            }
            if (clean.length() > 0) clean.append('/');
            clean.append(part);
        }
        return clean.toString();
    }

    private static byte[] readOptionalAssetBytes(AssetManager assets, String path, int maxBytes) {
        try (InputStream input = assets.open(path)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            int total = 0;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > maxBytes) return null;
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        } catch (IOException ignored) {
            return null;
        }
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        splashAnimationRunning = false;
        mainHandler.removeCallbacks(splashAdvance);
        super.onDestroy();
    }
}
