package dev.radek.generated;

import android.app.Activity;
import android.app.ActivityManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Launcher of a bounded complete-game conversion built on-device.
 * It shows recovered launch splashes before starting the statically recompiled
 * native entry through libconverted.so.
 */
public final class MainActivity extends Activity {
    private static final String LOG_TAG = "RadekConverted";
    private static final int MAX_SPLASH_FRAMES = 3;
    private static final long SPLASH_INTERVAL_MS = 900L;
    private static boolean nativeReady = false;

    static {
        try {
            System.loadLibrary("converted");
            nativeReady = true;
        } catch (Throwable ignored) {
            // The launcher still shows the recovered message below.
        }
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final List<Bitmap> splashFrames = new ArrayList<>();
    private int splashIndex = 0;
    private boolean destroyed = false;
    private File appDataDirectory;
    private File obbDirectory;
    private File persistentLog;
    private ImageView splashView;
    private TextView nativeLineView;
    private String launchMessage = "Native entry started.";
    private String appName = "";
    private Bitmap iconBitmap;

    private static native int runNative();

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private JSONObject readMetadata() {
        try (InputStream input = getAssets().open("conversion.json")) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            int total = 0;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > 1048576) return new JSONObject();
                output.write(buffer, 0, count);
            }
            return new JSONObject(new String(output.toByteArray(), StandardCharsets.UTF_8));
        } catch (Exception ignored) {
            return new JSONObject();
        }
    }

    private Bitmap readIcon() {
        try (InputStream input = getAssets().open("ipa-icon.png")) {
            return BitmapFactory.decodeStream(input);
        } catch (Exception ignored) {
            return null;
        }
    }

    private List<Bitmap> readSplashFrames() {
        List<Bitmap> frames = new ArrayList<>();
        for (String path : new String[] { "splash.png", "splash-2.png", "splash-3.png" }) {
            try (InputStream input = getAssets().open(path)) {
                Bitmap decoded = BitmapFactory.decodeStream(input);
                if (decoded != null) frames.add(decoded);
            } catch (Exception ignored) {
                // Older APKs may contain only a single named splash or bundle image.
            }
            if (frames.size() >= MAX_SPLASH_FRAMES) break;
        }
        if (frames.isEmpty()) {
            Bitmap fallback = readBundleSplash();
            if (fallback != null) frames.add(fallback);
        }
        return frames;
    }

    private Bitmap readBundleSplash() {
        String[] candidates = new String[] {
            "bundle/Default-Landscape.png",
            "bundle/Default-Landscape@2x.png",
            "bundle/Default.png",
            "bundle/LaunchImage.png",
            "bundle/Splash.png",
            "bundle/data/SPLASHES.png",
            "bundle/data/MENU.png"
        };
        for (String path : candidates) {
            try (InputStream input = getAssets().open(path)) {
                Bitmap decoded = BitmapFactory.decodeStream(input);
                if (decoded != null) return decoded;
            } catch (Exception ignored) {
                // Try the next standard asset.
            }
        }
        return null;
    }

    private TextView label(String value, float size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setGravity(Gravity.CENTER);
        if (bold) view.setTypeface(null, android.graphics.Typeface.BOLD);
        view.setPadding(dp(12), dp(8), dp(12), dp(8));
        return view;
    }

    private void createAppStorage() {
        File external = getExternalFilesDir(null);
        File root = external != null ? external : new File(getFilesDir(), "game-data");
        try {
            ensureDirectory(root);
            for (String child : new String[] {
                    "Documents", "Library", "Library/Caches", "tmp", "diagnostics" }) {
                ensureDirectory(new File(root, child));
            }
        } catch (Exception error) {
            Log.e(LOG_TAG, "External app-specific storage unavailable; using private storage", error);
            root = new File(getFilesDir(), "game-data");
            try {
                ensureDirectory(root);
                for (String child : new String[] {
                        "Documents", "Library", "Library/Caches", "tmp", "diagnostics" }) {
                    ensureDirectory(new File(root, child));
                }
            } catch (Exception fallbackError) {
                Log.e(LOG_TAG, "Could not create private app data", fallbackError);
            }
        }
        appDataDirectory = root;
        persistentLog = new File(new File(root, "diagnostics"), "launch.log");
        try {
            obbDirectory = getObbDir();
            if (obbDirectory != null) ensureDirectory(obbDirectory);
        } catch (Throwable error) {
            obbDirectory = null;
            Log.w(LOG_TAG, "App-specific OBB directory is unavailable", error);
        }
        try {
            JSONObject storage = new JSONObject()
                    .put("appDataDirectory", root.getAbsolutePath())
                    .put("obbDirectory", obbDirectory != null ? obbDirectory.getAbsolutePath() : JSONObject.NULL)
                    .put("source", "Context.getExternalFilesDir(null), with private-storage fallback")
                    .put("directories", new JSONArray().put("Documents").put("Library")
                            .put("Library/Caches").put("tmp").put("diagnostics"))
                    .put("bundleAssetsInApk", true)
                    .put("expansionObbRequired", false);
            try (FileOutputStream output = new FileOutputStream(
                    new File(new File(root, "diagnostics"), "storage.json"), false)) {
                output.write(storage.toString(2).getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception error) {
            Log.w(LOG_TAG, "Could not save app storage manifest", error);
        }
        appendLaunchLog("App data directory: " + root.getAbsolutePath());
        appendLaunchLog(obbDirectory == null
                ? "OBB directory unavailable; bundle is packaged in the APK."
                : "OBB directory created: " + obbDirectory.getAbsolutePath()
                        + " (no expansion OBB is required for this package).");
    }

    private static void ensureDirectory(File directory) throws Exception {
        if (!directory.isDirectory() && !directory.mkdirs() && !directory.isDirectory()) {
            throw new java.io.IOException("cannot create " + directory);
        }
    }

    private void appendLaunchLog(String line) {
        Log.i(LOG_TAG, line);
        if (persistentLog == null) return;
        synchronized (persistentLog) {
            try (FileOutputStream output = new FileOutputStream(persistentLog, true)) {
                output.write((System.currentTimeMillis() + " " + line + "\n")
                        .getBytes(StandardCharsets.UTF_8));
            } catch (Exception error) {
                Log.w(LOG_TAG, "Could not write persistent launch log", error);
            }
        }
    }

    private void showSplashFrame() {
        if (splashView == null || splashFrames.isEmpty()) return;
        splashView.setImageBitmap(splashFrames.get(splashIndex));
        splashView.setContentDescription(
                appName + " launch splash " + (splashIndex + 1) + " of " + splashFrames.size());
    }

    private void showLaunchSplash() {
        FrameLayout splashRoot = new FrameLayout(this);
        splashRoot.setBackgroundColor(Color.BLACK);
        splashView = new ImageView(this);
        splashView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        splashRoot.addView(splashView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.CENTER));
        showSplashFrame();
        setContentView(splashRoot);
        appendLaunchLog("Showing " + splashFrames.size() + " recovered splash frame(s), 900 ms each.");
        mainHandler.postDelayed(splashAdvance, SPLASH_INTERVAL_MS);
    }

    private final Runnable splashAdvance = new Runnable() {
        @Override
        public void run() {
            if (destroyed) return;
            if (splashIndex + 1 < splashFrames.size()) {
                splashIndex++;
                showSplashFrame();
                mainHandler.postDelayed(this, SPLASH_INTERVAL_MS);
                return;
            }
            showLaunchContent();
            startNativeEntry();
        }
    };

    private void showLaunchContent() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.BLACK);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(Color.BLACK);
        root.setPadding(dp(24), dp(48), dp(24), dp(48));

        if (iconBitmap != null) {
            ImageView icon = new ImageView(this);
            icon.setImageBitmap(iconBitmap);
            icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
            root.addView(icon, new LinearLayout.LayoutParams(dp(96), dp(96)));
        }
        root.addView(label(launchMessage, 26, Color.WHITE, true),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        if (!appName.isEmpty()) {
            root.addView(label(appName, 14, Color.rgb(160, 178, 199), false),
                    new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        nativeLineView = label("Native entry will start after the launch splash.",
                13, Color.rgb(92, 227, 181), false);
        root.addView(nativeLineView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        scroll.addView(root);
        setContentView(scroll);
        try {
            setTaskDescription(new ActivityManager.TaskDescription(
                    appName.isEmpty() ? "Converted IPA" : appName, iconBitmap, Color.BLACK));
        } catch (Exception ignored) {
            // The launcher's package icon remains available in the package.
        }
    }

    private String runNativeEntryWithStatus() {
        try {
            return "Native entry returned: " + runNative();
        } catch (Throwable error) {
            return "Native entry failed: " + error;
        }
    }

    private void startNativeEntry() {
        if (nativeLineView == null || destroyed) return;
        if (!nativeReady) {
            nativeLineView.setText("Native entry library unavailable on this device.");
            appendLaunchLog("Native entry library unavailable.");
            return;
        }
        nativeLineView.setText("Native entry running…");
        appendLaunchLog("Starting the statically recompiled native entry.");
        new Thread(new Runnable() {
            @Override
            public void run() {
                final String result = runNativeEntryWithStatus();
                appendLaunchLog(result);
                mainHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (!destroyed && nativeLineView != null) nativeLineView.setText(result);
                    }
                });
            }
        }, "converted-native-entry").start();
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.BLACK);
        getWindow().setNavigationBarColor(Color.BLACK);
        createAppStorage();

        JSONObject metadata = readMetadata();
        launchMessage = metadata.optString("launchMessage", "Native entry started.");
        appName = metadata.optString("applicationName", "");
        iconBitmap = readIcon();
        splashFrames.addAll(readSplashFrames());
        if (splashFrames.isEmpty()) {
            showLaunchContent();
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    startNativeEntry();
                }
            });
        } else {
            showLaunchSplash();
        }
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        mainHandler.removeCallbacks(splashAdvance);
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        FrameClockBridge.resume();
    }

    @Override
    protected void onPause() {
        FrameClockBridge.pause();
        super.onPause();
    }
}
