package dev.radek.generated;

import android.app.Activity;
import android.app.ActivityManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Minimal, source-free launcher included only in explicitly forced placeholder APKs. */
public final class GeneratedPlaceholderActivity extends Activity {
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

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.rgb(11, 16, 29));
        getWindow().setNavigationBarColor(Color.rgb(11, 16, 29));

        JSONObject info = readInfo();
        String gameName = info.optString("gameName", "Imported iOS app");
        String bundleId = info.optString("bundleId", "");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(Color.rgb(11, 16, 29));
        root.setPadding(dp(24), dp(36), dp(24), dp(36));

        Bitmap iconBitmap = readIcon();
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
        setContentView(root);
        try {
            setTaskDescription(new ActivityManager.TaskDescription(gameName, iconBitmap, Color.rgb(11, 16, 29)));
        } catch (Exception ignored) {
            // The launcher's package icon remains available even when recents icon metadata is unsupported.
        }
    }
}
