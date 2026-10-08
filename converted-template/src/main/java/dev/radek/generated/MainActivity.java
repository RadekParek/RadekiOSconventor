package dev.radek.generated;

import android.app.Activity;
import android.app.ActivityManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * Launcher of a bounded complete-game conversion built on-device.
 * It renders the recovered bundle splash screen alongside the message recovered
 * from the IPA and runs the statically recompiled native entry through libconverted.so.
 */
public final class MainActivity extends Activity {
    private static boolean nativeReady = false;

    static {
        try {
            System.loadLibrary("converted");
            nativeReady = true;
        } catch (Throwable ignored) {
            // The launcher still shows the recovered message below.
        }
    }

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

    private Bitmap readSplash() {
        String[] candidates = new String[] {
            "splash.png",
            "bundle/Default-Landscape.png",
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
                // Try next splash candidate.
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

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // Generated game APKs use true black behind the recovered splash and
        // guest output. The previous blue-black palette was visible around
        // letterboxed 480x320 assets on modern displays.
        getWindow().setStatusBarColor(Color.BLACK);
        getWindow().setNavigationBarColor(Color.BLACK);

        JSONObject metadata = readMetadata();
        String message = metadata.optString("launchMessage", "Native entry started.");
        String appName = metadata.optString("applicationName", "");

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.BLACK);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(Color.BLACK);
        root.setPadding(dp(24), dp(48), dp(24), dp(48));

        Bitmap splashBitmap = readSplash();
        if (splashBitmap != null) {
            LinearLayout splashCard = new LinearLayout(this);
            splashCard.setOrientation(LinearLayout.VERTICAL);
            splashCard.setGravity(Gravity.CENTER_HORIZONTAL);
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(Color.BLACK);
            bg.setCornerRadius(dp(12));
            bg.setStroke(dp(1), Color.rgb(64, 64, 64));
            splashCard.setBackground(bg);
            splashCard.setPadding(dp(10), dp(10), dp(10), dp(10));

            ImageView splashView = new ImageView(this);
            splashView.setImageBitmap(splashBitmap);
            splashView.setAdjustViewBounds(true);
            splashView.setMaxHeight(dp(240));
            splashView.setScaleType(ImageView.ScaleType.FIT_CENTER);
            splashCard.addView(
                    splashView,
                    new LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.WRAP_CONTENT));

            LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            cardParams.bottomMargin = dp(16);
            root.addView(splashCard, cardParams);
        }

        Bitmap iconBitmap = readIcon();
        if (iconBitmap != null) {
            ImageView icon = new ImageView(this);
            icon.setImageBitmap(iconBitmap);
            icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
            int iconSize = (splashBitmap != null) ? dp(72) : dp(96);
            root.addView(icon, new LinearLayout.LayoutParams(iconSize, iconSize));
        }

        root.addView(label(message, 26, Color.WHITE, true),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        if (!appName.isEmpty()) {
            root.addView(label(appName, 14, Color.rgb(160, 178, 199), false),
                    new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }

        String nativeLine;
        if (nativeReady) {
            try {
                nativeLine = "Native entry returned: " + runNative();
            } catch (Throwable error) {
                nativeLine = "Native entry failed: " + error;
            }
        } else {
            nativeLine = "Native entry library unavailable on this device.";
        }
        root.addView(label(nativeLine, 13, Color.rgb(92, 227, 181), false),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        scroll.addView(root);
        setContentView(scroll);
        try {
            setTaskDescription(new ActivityManager.TaskDescription(appName.isEmpty() ? "Converted IPA" : appName,
                    iconBitmap, Color.BLACK));
        } catch (Exception ignored) {
            // The launcher's package icon remains available even when recents icon metadata is unsupported.
        }
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
