package dev.radek.gameruntime;

import android.content.pm.ActivityInfo;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowLooper;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 28, manifest = Config.NONE)
public final class GameBootActivityTest {
    @Test
    public void missingMetadataLeavesTheDiagnosticScreenOpen() {
        GameBootActivity activity = Robolectric.buildActivity(GameBootActivity.class).create().get();
        ShadowLooper mainLooper = Shadows.shadowOf(Looper.getMainLooper());
        mainLooper.idle();

        assertFalse(activity.isFinishing());
        String screenText = textIn(activity.getWindow().getDecorView());
        assertTrue(screenText.contains("Boot metadata is missing or invalid"));
        assertTrue(screenText.contains("diagnostic screen will remain open"));
        assertTrue(screenText.contains("GAME SPLASH SCREEN VIEWPORT"));

        // The old launcher posted a delayed RuntimeException after displaying
        // this same error. Advancing past that delay must not crash the activity.
        mainLooper.idleFor(2, TimeUnit.SECONDS);
        assertFalse(activity.isFinishing());
    }

    @Test
    public void blockedGuestBootShowsTheImportAndDoesNotCrash() {
        GameBootActivity activity = Robolectric.buildActivity(GameBootActivity.class).create().get();
        activity.displayBootResult(
                "{\"loader\":{\"status\":\"LOADED_WITH_TRAPS\",\"resolvedSymbolCount\":39," +
                        "\"trappedSymbolCount\":539,\"unresolvedSymbolCount\":0}," +
                        "\"execution\":{\"status\":\"STOPPED_AT_TRAP\",\"instructions\":34}," +
                        "\"trappedImport\":\"_UIApplicationMain\",\"reason\":\"unimplemented import\"}"
        );
        ShadowLooper mainLooper = Shadows.shadowOf(Looper.getMainLooper());
        mainLooper.idle();

        String screenText = textIn(activity.getWindow().getDecorView());
        assertTrue(screenText.contains("_UIApplicationMain"));
        assertTrue(screenText.contains("Guest boot stopped"));
        assertTrue(screenText.contains("This APK is not a playable conversion"));
        assertFalse(activity.isFinishing());

        mainLooper.idleFor(2, TimeUnit.SECONDS);
        assertFalse(activity.isFinishing());
    }

    @Test
    public void budgetStopIsReportedAsABudgetAndNeverAsAnImportNamedNull() {
        GameBootActivity activity = Robolectric.buildActivity(GameBootActivity.class).create().get();
        activity.displayBootResult(
                "{\"loader\":{\"status\":\"LOADED_WITH_TRAPS\",\"resolvedSymbolCount\":60," +
                        "\"trappedSymbolCount\":518,\"unresolvedSymbolCount\":0}," +
                        "\"execution\":{\"status\":\"TIME_LIMIT\",\"instructions\":338936}," +
                        "\"trappedImport\":null,\"reason\":\"guest function reached its time limit\"}"
        );
        ShadowLooper mainLooper = Shadows.shadowOf(Looper.getMainLooper());
        mainLooper.idle();

        String screenText = textIn(activity.getWindow().getDecorView());
        // A JSON null trap name was previously printed as an import called "null".
        assertFalse(screenText.contains("import: null"));
        assertTrue(screenText.contains("338936"));
        assertTrue(screenText.contains("TIME_LIMIT"));
        assertTrue(screenText.contains("Guest boot budget reached"));
        assertTrue(screenText.contains("No unimplemented import was reached during this window."));
        assertTrue(screenText.contains("This APK is not a playable conversion"));
        assertFalse(activity.isFinishing());

        mainLooper.idleFor(3, TimeUnit.SECONDS);
        assertFalse(activity.isFinishing());
    }

    @Test
    public void gameRunsLandscapeAndTheStopScreenReturnsToPortrait() {
        GameBootActivity activity = Robolectric.buildActivity(GameBootActivity.class).create().get();

        // While the guest runs the launcher is landscape (either direction) with
        // only the game visible; the diagnostic panel is hidden.
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
                activity.getRequestedOrientation());
        assertFalse(activity.diagnosticsOverlayVisible());

        ShadowLooper mainLooper = Shadows.shadowOf(Looper.getMainLooper());
        mainLooper.idle();
        activity.displayBootResult(
                "{\"loader\":{\"status\":\"LOADED_WITH_TRAPS\",\"resolvedSymbolCount\":138,"
                        + "\"trappedSymbolCount\":440,\"unresolvedSymbolCount\":0},"
                        + "\"execution\":{\"status\":\"INSTRUCTION_LIMIT\",\"instructions\":2000000},"
                        + "\"reason\":\"guest function reached its instruction limit.\"}");
        mainLooper.idle();

        // The attempt is over: back to portrait with the diagnostics on screen.
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT, activity.getRequestedOrientation());
        assertTrue(activity.diagnosticsOverlayVisible());
        String screenText = textIn(activity.getWindow().getDecorView());
        assertTrue(screenText.contains("Guest boot budget reached"));
        assertTrue(screenText.contains("This APK is not a playable conversion"));
        assertFalse(activity.isFinishing());
    }

    @Test
    public void splashFramesAdvanceEvery900MillisThenRevealTheGameSurface() {
        GameBootActivity activity = Robolectric.buildActivity(GameBootActivity.class).create().get();
        ShadowLooper mainLooper = Shadows.shadowOf(Looper.getMainLooper());
        mainLooper.idle();

        activity.installSyntheticSplashFramesForTest(5);
        assertEquals("the launcher caps recovered frames at three", 3, activity.installedSplashFrameCount());
        assertEquals(0, activity.currentSplashFrameIndex());
        assertFalse(activity.gameSurfaceVisibleForTest());
        assertFalse(activity.splashSequenceCompleteForTest());

        mainLooper.idleFor(899, TimeUnit.MILLISECONDS);
        assertEquals(0, activity.currentSplashFrameIndex());
        mainLooper.idleFor(1, TimeUnit.MILLISECONDS);
        assertEquals(1, activity.currentSplashFrameIndex());

        mainLooper.idleFor(899, TimeUnit.MILLISECONDS);
        assertEquals(1, activity.currentSplashFrameIndex());
        mainLooper.idleFor(1, TimeUnit.MILLISECONDS);
        assertEquals(2, activity.currentSplashFrameIndex());
        assertFalse(activity.gameSurfaceVisibleForTest());

        // The final frame also receives its full 900 ms; guest rendering is
        // released only after that interval, rather than running under a black
        // SurfaceView while the splash is on screen.
        mainLooper.idleFor(899, TimeUnit.MILLISECONDS);
        assertFalse(activity.splashSequenceCompleteForTest());
        mainLooper.idleFor(1, TimeUnit.MILLISECONDS);
        assertTrue(activity.splashSequenceCompleteForTest());
        assertTrue(activity.gameSurfaceVisibleForTest());

        // The sequence is once-through and never wraps to the first frame.
        mainLooper.idleFor(5, TimeUnit.SECONDS);
        assertEquals(2, activity.currentSplashFrameIndex());
        assertFalse(activity.isFinishing());
    }

    @Test
    public void splashNeverAsksForTouchCycling() {
        GameBootActivity activity = Robolectric.buildActivity(GameBootActivity.class).create().get();
        ShadowLooper mainLooper = Shadows.shadowOf(Looper.getMainLooper());
        mainLooper.idle();

        String screenText = textIn(activity.getWindow().getDecorView());
        assertFalse(screenText.contains("tap viewport to cycle"));
    }

    @Test
    public void parsesAngryBirdsSplashSheetDescriptor() throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        writeU16String(out, "SPLASHES.png");
        writeU16Be(out, 3);
        writeSpriteEntry(out, "SPLASH_ROVIO", 1, 321, 121, 190, 60, 95);
        writeSpriteEntry(out, "SPLASH_ANGRY_BIRDS", 1, 511, 480, 320, 240, 160);
        writeSpriteEntry(out, "SPLASH_CLICKGAMER", 1, 1, 480, 320, 240, 160);

        GameBootActivity.SplashSheetDescriptor descriptor =
                GameBootActivity.parseSplashSheetDescriptor(out.toByteArray());
        assertNotNull(descriptor);
        assertEquals("SPLASHES.png", descriptor.sheetName);
        assertEquals(3, descriptor.entries.size());
        assertEquals("SPLASH_ANGRY_BIRDS", descriptor.entries.get(1).name);
        assertEquals(480, descriptor.entries.get(1).width);
        assertEquals(320, descriptor.entries.get(1).height);
        assertNull(GameBootActivity.parseSplashSheetDescriptor(new byte[] { 0x00, 0x01 }));
    }

    @Test
    public void decodesAppleCgbiPngWithBgraPremultipliedSamples() throws Exception {
        byte[] cgbiPng = buildSinglePixelCgbiPng(40, 80, 200, 255);
        assertTrue(GameBootActivity.isCgbiPng(cgbiPng));
        assertFalse(GameBootActivity.isCgbiPng(new byte[] { 1, 2, 3, 4 }));

        int[] dims = new int[2];
        int[] pixels = GameBootActivity.decodeCgbiRgbaPixels(cgbiPng, dims);
        assertEquals(1, dims[0]);
        assertEquals(1, dims[1]);
        assertEquals(1, pixels.length);
        int argb = pixels[0];
        assertEquals(255, (argb >>> 24) & 0xff);
        assertEquals(200, (argb >>> 16) & 0xff);
        assertEquals(80, (argb >>> 8) & 0xff);
        assertEquals(40, argb & 0xff);
    }

    private static String textIn(View view) {
        StringBuilder result = new StringBuilder();
        if (view instanceof TextView) result.append(((TextView) view).getText()).append('\n');
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++) {
                result.append(textIn(group.getChildAt(index)));
            }
        }
        return result.toString();
    }

    private static void writeU16Be(ByteArrayOutputStream out, int value) {
        out.write((value >>> 8) & 0xff);
        out.write(value & 0xff);
    }

    private static void writeU16String(ByteArrayOutputStream out, String value) throws IOException {
        byte[] ascii = value.getBytes(StandardCharsets.US_ASCII);
        writeU16Be(out, ascii.length);
        out.write(ascii);
    }

    private static void writeSpriteEntry(
            ByteArrayOutputStream out,
            String name,
            int x,
            int y,
            int width,
            int height,
            int pivotX,
            int pivotY) throws IOException {
        writeU16String(out, name);
        writeU16Be(out, x);
        writeU16Be(out, y);
        writeU16Be(out, width);
        writeU16Be(out, height);
        writeU16Be(out, pivotX);
        writeU16Be(out, pivotY);
    }

    private static byte[] buildSinglePixelCgbiPng(int b, int g, int r, int a) throws IOException {
        ByteArrayOutputStream png = new ByteArrayOutputStream();
        png.write(new byte[] { (byte) 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a });
        writePngChunk(png, "CgBI", new byte[] { 0x50, 0x00, 0x20, 0x06 });
        writePngChunk(png, "IHDR", new byte[] {
            0, 0, 0, 1,
            0, 0, 0, 1,
            8, 6, 0, 0, 0
        });
        byte[] scanline = new byte[] { 0, (byte) b, (byte) g, (byte) r, (byte) a };
        Deflater deflater = new Deflater(Deflater.DEFAULT_COMPRESSION, true);
        deflater.setInput(scanline);
        deflater.finish();
        byte[] compressed = new byte[64];
        int len = deflater.deflate(compressed);
        deflater.end();
        byte[] idat = new byte[len];
        System.arraycopy(compressed, 0, idat, 0, len);
        writePngChunk(png, "IDAT", idat);
        writePngChunk(png, "IEND", new byte[0]);
        return png.toByteArray();
    }

    private static void writePngChunk(ByteArrayOutputStream out, String type, byte[] payload)
            throws IOException {
        int len = payload.length;
        out.write((len >>> 24) & 0xff);
        out.write((len >>> 16) & 0xff);
        out.write((len >>> 8) & 0xff);
        out.write(len & 0xff);
        byte[] typeBytes = type.getBytes(StandardCharsets.US_ASCII);
        out.write(typeBytes);
        out.write(payload);
        CRC32 crc = new CRC32();
        crc.update(typeBytes);
        crc.update(payload);
        long c = crc.getValue();
        out.write((int) ((c >>> 24) & 0xff));
        out.write((int) ((c >>> 16) & 0xff));
        out.write((int) ((c >>> 8) & 0xff));
        out.write((int) (c & 0xff));
    }
}
