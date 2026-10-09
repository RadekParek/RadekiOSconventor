package dev.radek.conventor

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.charset.StandardCharsets
import java.util.Locale
import java.util.zip.ZipFile

/**
 * Extracts and normalizes an iOS game's primary splash screen into a standard
 * Android-decodable PNG byte array.
 *
 * Supports:
 * 1. Sprite-sheet splash descriptors (`SPLASHES.dat` + `SPLASHES.png`, used by
 *    Angry Birds 1.0.0 and other Rovio/Chillingo/Clickgamer titles).
 * 2. Standard and Apple CgBI-encoded iOS launch/splash images (`Default*.png`,
 *    `LaunchImage*.png`, `Splash*.png`, `MENU.png`).
 */
internal object SplashExtractor {
    private const val MAX_ENTRY_BYTES = 16 * 1024 * 1024
    private const val MAX_SPLASH_FRAMES = 3
    private const val VIEWPORT_WIDTH = 480
    private const val VIEWPORT_HEIGHT = 320

    internal data class SpriteEntry(
        val name: String,
        val x: Int,
        val y: Int,
        val width: Int,
        val height: Int,
        val pivotX: Int,
        val pivotY: Int,
    )

    internal data class SheetDescriptor(
        val sheetName: String,
        val entries: List<SpriteEntry>,
    )

    fun extractSplashPng(ipaFile: File): ByteArray? = extractSplashPngs(ipaFile, 1).firstOrNull()

    /** Extracts up to three distinct launch/sprite frames, preserving the old single-frame API. */
    fun extractSplashPngs(ipaFile: File, maxFrames: Int = MAX_SPLASH_FRAMES): List<ByteArray> {
        if (!ipaFile.isFile || maxFrames <= 0) return emptyList()
        val limit = maxFrames.coerceAtMost(MAX_SPLASH_FRAMES)
        return try {
            ZipFile(ipaFile).use { zip ->
                val entriesByPath = LinkedHashMap<String, java.util.zip.ZipEntry>()
                val enumeration = zip.entries()
                while (enumeration.hasMoreElements()) {
                    val entry = enumeration.nextElement()
                    if (!entry.isDirectory && entry.size in 1..MAX_ENTRY_BYTES.toLong()) {
                        entriesByPath[entry.name] = entry
                    }
                }
                // Sprite sheets hold several separately designed publisher/game
                // splashes. Keep their reviewed priority and emit at most three.
                for ((path, datEntry) in entriesByPath) {
                    val lower = path.lowercase(Locale.US)
                    if (!lower.startsWith("payload/") || !lower.endsWith(".dat") ||
                        !lower.contains("splash") || datEntry.size > 256 * 1024
                    ) continue
                    val datBytes = zip.getInputStream(datEntry).use { it.readBytes() }
                    val descriptor = parseSplashSheetDescriptor(datBytes) ?: continue
                    val parentDir = path.substringBeforeLast('/', "")
                    val sheetPath = if (parentDir.isEmpty()) descriptor.sheetName else "$parentDir/${descriptor.sheetName}"
                    val sheetEntry = entriesByPath[sheetPath] ?: continue
                    val sheetBytes = zip.getInputStream(sheetEntry).use { it.readBytes() }
                    val sheetBitmap = IconDecoder.decode(sheetBytes, 2048) ?: continue
                    val frames = cropSplashSprites(sheetBitmap, descriptor, limit)
                        .mapNotNull(::encodePng)
                    if (frames.isNotEmpty()) return frames
                }

                // Standard launch images are distinct frame candidates. Keep
                // Apple CgBI normalization in IconDecoder and avoid duplicate paths.
                val preferredNames = listOf(
                    "default-landscape.png",
                    "default-landscape@2x.png",
                    "default-568h@2x.png",
                    "default@2x.png",
                    "default.png",
                    "launchimage.png",
                    "splash.png",
                    "menu.png",
                )
                val seenPaths = HashSet<String>()
                val frames = ArrayList<ByteArray>(limit)
                for (targetName in preferredNames) {
                    val matched = entriesByPath.entries.firstOrNull { (path, _) ->
                        path.lowercase(Locale.US).startsWith("payload/") &&
                            path.substringAfterLast('/').lowercase(Locale.US) == targetName
                    } ?: continue
                    if (!seenPaths.add(matched.key)) continue
                    val raw = zip.getInputStream(matched.value).use { it.readBytes() }
                    val decoded = IconDecoder.decode(raw, 1024) ?: continue
                    encodePng(decoded)?.let(frames::add)
                    if (frames.size >= limit) break
                }
                frames
            }
        } catch (_: Throwable) {
            emptyList()
        }
    }

    internal fun parseSplashSheetDescriptor(data: ByteArray): SheetDescriptor? {
        if (data.size < 6) return null
        var offset = 0
        val sheetNameLen = readU16Be(data, offset)
        offset += 2
        if (sheetNameLen !in 1..256 || offset + sheetNameLen + 2 > data.size) return null
        val sheetName = String(data, offset, sheetNameLen, StandardCharsets.US_ASCII)
        offset += sheetNameLen
        val count = readU16Be(data, offset)
        offset += 2
        if (count !in 1..512) return null
        val entries = ArrayList<SpriteEntry>(count)
        repeat(count) {
            if (offset + 2 > data.size) return null
            val nameLen = readU16Be(data, offset)
            offset += 2
            if (nameLen !in 1..256 || offset + nameLen + 12 > data.size) return null
            val name = String(data, offset, nameLen, StandardCharsets.US_ASCII)
            offset += nameLen
            val x = readU16Be(data, offset)
            val y = readU16Be(data, offset + 2)
            val width = readU16Be(data, offset + 4)
            val height = readU16Be(data, offset + 6)
            val pivotX = readU16Be(data, offset + 8)
            val pivotY = readU16Be(data, offset + 10)
            offset += 12
            if (width !in 1..4096 || height !in 1..4096) return null
            entries += SpriteEntry(name, x, y, width, height, pivotX, pivotY)
        }
        return SheetDescriptor(sheetName, entries)
    }

    private fun cropSplashSprites(
        sheet: Bitmap,
        descriptor: SheetDescriptor,
        limit: Int,
    ): List<Bitmap> {
        val selected = descriptor.entries
            .filter {
                it.x >= 0 && it.y >= 0 &&
                    it.x + it.width <= sheet.width &&
                    it.y + it.height <= sheet.height
            }
            .sortedByDescending { entry ->
                val upper = entry.name.uppercase(Locale.US)
                val bonus = when {
                    "ANGRY" in upper || "GAME" in upper || "TITLE" in upper || "MAIN" in upper -> 1_000_000
                    "ROVIO" in upper -> 500_000
                    else -> 0
                }
                bonus + entry.width * entry.height
            }
            .take(limit)
        return selected.map { entry ->
            val cropped = Bitmap.createBitmap(sheet, entry.x, entry.y, entry.width, entry.height)
            if (cropped.width >= VIEWPORT_WIDTH && cropped.height >= VIEWPORT_HEIGHT) {
                cropped
            } else {
                val targetW = maxOf(VIEWPORT_WIDTH, cropped.width)
                val targetH = maxOf(VIEWPORT_HEIGHT, cropped.height)
                val composed = Bitmap.createBitmap(targetW, targetH, Bitmap.Config.ARGB_8888)
                val canvas = Canvas(composed)
                canvas.drawColor(Color.BLACK)
                val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
                canvas.drawBitmap(
                    cropped,
                    (targetW - cropped.width) * 0.5f,
                    (targetH - cropped.height) * 0.5f,
                    paint,
                )
                composed
            }
        }
    }

    private fun encodePng(bitmap: Bitmap): ByteArray? {
        val output = ByteArrayOutputStream()
        if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)) return null
        return output.toByteArray().takeIf { it.isNotEmpty() }
    }

    private fun readU16Be(data: ByteArray, offset: Int): Int =
        ((data[offset].toInt() and 0xff) shl 8) or (data[offset + 1].toInt() and 0xff)
}
