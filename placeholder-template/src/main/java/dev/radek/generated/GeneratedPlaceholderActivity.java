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

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** Minimal, source-free launcher included only in explicitly forced placeholder APKs. */
public final class GeneratedPlaceholderActivity extends Activity {
    private static final String LOG_TAG = "RadekPlaceholder";
    private static final int MAX_SPLASH_FRAMES = 3;
    private static final long SPLASH_INTERVAL_MS = 900L;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final List<Bitmap> splashFrames = new ArrayList<>();
    private int splashIndex = 0;
    private boolean destroyed = false;
    private File appDataDirectory;
    private File persistentLog;
    private File obbDirectory;
    private String gameName = "Imported iOS app";
    private String bundleId = "";
    private Bitmap iconBitmap;
    private ImageView splashView;

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private JSONObject readInfo() {
        try (InputStream input = getAssets().open("placeholder-info.json")) {
            java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            int total = 0;
            while (true) {
                int count = input.read(buffer);
                if (count < 0) break;
                total += count;
                if (total > 8192) return new JSONObject();
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
                // A placeholder may have no recovered launch artwork.
            }
            if (frames.size() >= MAX_SPLASH_FRAMES) break;
        }
        return frames;
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
                ? "OBB directory unavailable; this preview has no expansion payload."
                : "OBB directory created: " + obbDirectory.getAbsolutePath()
                        + " (no expansion OBB is required for this preview).");
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
        splashView.setContentDescription(gameName + " splash " + (splashIndex + 1)
                + " of " + splashFrames.size());
    }

    private final Runnable splashAdvance = new Runnable() {
        @Override
        public void run() {
            if (destroyed) return;
            if (splashIndex + 1 < splashFrames.size()) {
                splashIndex++;
                showSplashFrame();
                mainHandler.postDelayed(this, SPLASH_INTERVAL_MS);
            } else {
                showPreview();
            }
        }
    };

    private void showSplashSequence() {
        FrameLayout splashRoot = new FrameLayout(this);
        splashRoot.setBackgroundColor(Color.rgb(11, 16, 29));
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

    private TextView label(String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold) view.setTypeface(null, android.graphics.Typeface.BOLD);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(12), dp(8), dp(12), dp(8));
        return view;
    }

    private void showPreview() {
        if (destroyed) return;
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(11, 16, 29));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(Color.rgb(11, 16, 29));
        root.setPadding(dp(24), dp(36), dp(24), dp(36));

        if (iconBitmap != null) {
            ImageView icon = new ImageView(this);
            icon.setImageBitmap(iconBitmap);
            icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
            icon.setContentDescription(gameName + " icon");
            root.addView(icon, new LinearLayout.LayoutParams(dp(128), dp(128)));
        }
        root.addView(label(gameName, 25, Color.WHITE, true),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        if (!bundleId.isEmpty()) {
            root.addView(label(bundleId, 13, Color.rgb(160, 178, 199), false),
                    new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        root.addView(label("Preview shell started", 14, Color.rgb(92, 227, 181), false),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(label("No statically recompiled game code is included in this preview.",
                        12, Color.rgb(160, 178, 199), false),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        scroll.addView(root);
        setContentView(scroll);
        appendLaunchLog("Preview shell shown; no game code was started.");
        try {
            setTaskDescription(new ActivityManager.TaskDescription(
                    gameName, iconBitmap, Color.rgb(11, 16, 29)));
        } catch (Exception ignored) {
            // The launcher's package icon remains available in the package.
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.rgb(11, 16, 29));
        getWindow().setNavigationBarColor(Color.rgb(11, 16, 29));
        createAppStorage();

        JSONObject info = readInfo();
        gameName = info.optString("gameName", "Imported iOS app");
        bundleId = info.optString("bundleId", "");
        iconBitmap = readIcon();
        splashFrames.addAll(readSplashFrames());
        if (splashFrames.isEmpty()) {
            showPreview();
        } else {
            showSplashSequence();
        }
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        mainHandler.removeCallbacks(splashAdvance);
        super.onDestroy();
    }
}
