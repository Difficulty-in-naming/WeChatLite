package local.wechatlite.mobile.ui

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.graphics.drawable.Drawable
import android.os.Build
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.painter.BitmapPainter
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import java.io.File
import java.util.Locale
import kotlin.math.max
import kotlin.math.min
import local.wechatlite.mobile.data.Media

/**
 * In-chat attachment previews, mirroring `client/messages.cpp` / `client/window.cpp`:
 *
 * - images and GIFs draw a scaled thumbnail (image up to 240dp, GIF up to 134dp) and fall back to a
 *   rounded "图片 · 点击下载" card while the file is missing;
 * - a video never downloads automatically, it shows the 240x144 poster the server generates, or the
 *   dark card with a play triangle when there is no poster yet;
 * - files and voice messages stay compact cards.
 *
 * Tapping always opens the full attachment; drawing never blocks the message list.
 */
@Composable
fun AttachmentPreview(
    media: Media,
    file: File?,
    poster: File?,
    loading: Boolean,
    onOpen: () -> Unit,
    onLongPress: () -> Unit,
) {
    when {
        media.isVoice -> VoiceCard(media, onOpen = onOpen, onLongPress = onLongPress)
        media.isVideo -> VideoCard(media, poster, loading, onOpen, onLongPress)
        media.isImage || media.isGif -> ThumbnailCard(media, file, loading, onOpen, onLongPress)
        else -> FileCard(media, loading, onOpen, onLongPress)
    }
}

/** Previews open on a tap and keep the bubble's action sheet reachable on a long press. */
@OptIn(ExperimentalFoundationApi::class)
private fun Modifier.previewGestures(onOpen: () -> Unit, onLongPress: () -> Unit): Modifier =
    this.combinedClickable(onClick = onOpen, onLongClick = onLongPress)

@Composable
private fun ThumbnailCard(media: Media, file: File?, loading: Boolean, onOpen: () -> Unit, onLongPress: () -> Unit) {
    val maxSize = if (media.isGif) GifMaxSize else ImageMaxSize
    if (file != null && media.isGif && Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        val animated by animatedGif(file, maxSize)
        val drawable = animated
        if (drawable != null) {
            AnimatedGifBox(drawable, maxSize, onOpen, onLongPress)
            return
        }
    }
    val bitmap by rememberBitmap(file)
    val image = bitmap
    if (image != null) {
        val (width, height) = fitWithin(image.width, image.height, maxSize)
        Box(Modifier.clip(RoundedCornerShape(4.dp)).previewGestures(onOpen, onLongPress)) {
            Image(
                painter = BitmapPainter(image.asImageBitmap()),
                contentDescription = media.name,
                modifier = Modifier.size(width = width, height = height),
                contentScale = ContentScale.FillBounds,
            )
        }
    } else {
        MissingPreview(
            label = if (media.isGif) "GIF · 点击下载" else "图片 · 点击下载",
            loading = loading,
            onOpen = onOpen,
            onLongPress = onLongPress,
        )
    }
}

/**
 * Hosts the animated drawable in a plain [android.widget.ImageView], because
 * [AnimatedImageDrawable] invalidates itself on every frame and only a real View turns that into a
 * repaint. Leaving the composition (a row scrolled off screen) stops the animation, which keeps the
 * number of running GIFs close to the desktop client's limit.
 */
@Composable
private fun AnimatedGifBox(drawable: Drawable, maxSize: Dp, onOpen: () -> Unit, onLongPress: () -> Unit) {
    val (width, height) = fitWithin(drawable.intrinsicWidth, drawable.intrinsicHeight, maxSize)
    AndroidView(
        factory = { context ->
            android.widget.ImageView(context).apply {
                scaleType = android.widget.ImageView.ScaleType.FIT_XY
                setImageDrawable(drawable)
                (drawable as? android.graphics.drawable.Animatable)?.start()
            }
        },
        modifier = Modifier
            .size(width, height)
            .clip(RoundedCornerShape(4.dp))
            .previewGestures(onOpen, onLongPress),
        onRelease = { (drawable as? android.graphics.drawable.Animatable)?.stop() },
    )
}

@Composable
private fun VideoCard(media: Media, poster: File?, loading: Boolean, onOpen: () -> Unit, onLongPress: () -> Unit) {
    val bitmap by rememberBitmap(poster)
    val preview = bitmap
    Box(
        modifier = Modifier
            .size(width = VideoWidth, height = VideoHeight)
            .clip(RoundedCornerShape(4.dp))
            .background(VideoFill)
            .previewGestures(onOpen, onLongPress),
    ) {
        if (preview != null) {
            Image(
                painter = BitmapPainter(preview.asImageBitmap()),
                contentDescription = media.name,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
            // Desktop overlay: a translucent circle behind the play glyph plus a caption band.
            Canvas(Modifier.fillMaxSize()) {
                drawCircle(
                    color = Color.Black.copy(alpha = 0.31f),
                    radius = 16.dp.toPx(),
                    center = center,
                )
                val band = 25.dp.toPx()
                drawRect(
                    color = Color.Black.copy(alpha = 0.31f),
                    topLeft = Offset(0f, size.height - band),
                    size = Size(size.width, band),
                )
            }
        } else if (loading) {
            CircularProgressIndicator(
                modifier = Modifier.align(Alignment.Center).size(20.dp),
                strokeWidth = 2.dp,
            )
        }
        Canvas(Modifier.fillMaxSize()) {
            val path = Path().apply {
                moveTo(center.x - 5.dp.toPx(), center.y - 9.dp.toPx())
                lineTo(center.x - 5.dp.toPx(), center.y + 9.dp.toPx())
                lineTo(center.x + 10.dp.toPx(), center.y)
                close()
            }
            drawPath(path, Color.White)
        }
        Text(
            media.name.elideMiddle(18),
            color = Color.White,
            fontSize = 11.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(start = 8.dp, end = 8.dp, bottom = 5.dp),
        )
    }
}

@Composable
private fun FileCard(media: Media, loading: Boolean, onOpen: () -> Unit, onLongPress: () -> Unit) {
    val shape = RoundedCornerShape(4.dp)
    val dark = MaterialTheme.colorScheme.surface.luminanceIsDark()
    Row(
        modifier = Modifier
            .width(FileWidth)
            .height(FileHeight)
            .clip(shape)
            .background(if (dark) FileFillDark else FileFill)
            .border(1.dp, if (dark) FileBorderDark else FileBorder, shape)
            .previewGestures(onOpen, onLongPress),
    ) {
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(start = 12.dp, top = 12.dp, end = 4.dp),
        ) {
            Text(
                media.name.elideMiddle(18),
                style = MaterialTheme.typography.bodySmall,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                attachmentSizeLabel(media.size),
                style = MaterialTheme.typography.labelSmall,
                color = if (dark) FileSubTextDark else FileSubText,
                maxLines = 1,
            )
            if (loading) {
                Spacer(Modifier.height(4.dp))
                CircularProgressIndicator(Modifier.size(12.dp), strokeWidth = 2.dp)
            }
        }
        val suffix = media.name.substringAfterLast('.', "").take(4).uppercase(Locale.getDefault())
        Box(
            modifier = Modifier
                .padding(top = 17.dp, end = 18.dp)
                .size(width = 40.dp, height = 45.dp)
                .clip(RoundedCornerShape(3.dp))
                .background(BadgeFill),
            contentAlignment = Alignment.Center,
        ) {
            Text(suffix, color = Color.White, fontSize = 11.sp, maxLines = 1)
        }
    }
}

@Composable
private fun VoiceCard(media: Media, onOpen: () -> Unit, onLongPress: () -> Unit) {
    Row(
        modifier = Modifier
            .widthIn(min = VoiceWidth)
            .clip(RoundedCornerShape(4.dp))
            .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.75f))
            .previewGestures(onOpen, onLongPress)
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PlayGlyph(Modifier.size(16.dp))
        Spacer(Modifier.width(6.dp))
        Column {
            Text(media.name, style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                "语音 · ${attachmentSizeLabel(media.size)}",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** The rounded "点击下载" card the desktop client paints while a thumbnail is unavailable. */
@Composable
private fun MissingPreview(label: String, loading: Boolean, onOpen: () -> Unit, onLongPress: () -> Unit) {
    val dark = MaterialTheme.colorScheme.surface.luminanceIsDark()
    Box(
        modifier = Modifier
            .size(PlaceholderSize)
            .clip(RoundedCornerShape(4.dp))
            .background(if (dark) PlaceholderFillDark else PlaceholderFill)
            .previewGestures(onOpen, onLongPress),
        contentAlignment = Alignment.Center,
    ) {
        if (loading) {
            CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
        } else {
            Text(
                label,
                color = if (dark) PlaceholderTextDark else PlaceholderText,
                style = MaterialTheme.typography.labelSmall,
            )
        }
    }
}

@Composable
private fun PlayGlyph(modifier: Modifier = Modifier) {
    val tint = MaterialTheme.colorScheme.onSurfaceVariant
    Canvas(modifier) {
        val path = Path().apply {
            moveTo(size.width * 0.2f, 0f)
            lineTo(size.width * 0.2f, size.height)
            lineTo(size.width, size.height / 2f)
            close()
        }
        drawPath(path, tint)
    }
}

/**
 * Decodes a still image once per file. [rememberBitmap] reuses the in-memory copy while the row stays
 * composed and the shared cache keeps recently scrolled rows from decoding again.
 */
@Composable
private fun rememberBitmap(file: File?): androidx.compose.runtime.State<Bitmap?> {
    // Reading the timestamp once per file keeps the stat call out of every recomposition.
    val stamp = remember(file) { file?.lastModified() }
    return produceState<Bitmap?>(
        initialValue = file?.let { ThumbnailCache.peek(it) },
        key1 = file?.absolutePath,
        key2 = stamp,
    ) {
        if (file == null) {
            value = null
            return@produceState
        }
        ThumbnailCache.peek(file)?.let {
            value = it
            return@produceState
        }
        value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            decodeStill(file)?.also { ThumbnailCache.put(file, it) }
        }
    }
}

@Composable
private fun animatedGif(file: File, maxSize: Dp): androidx.compose.runtime.State<Drawable?> {
    val limitPx = with(LocalDensity.current) { maxSize.roundToPx() }
    val stamp = remember(file) { file.lastModified() }
    return produceState<Drawable?>(initialValue = null, key1 = file.absolutePath, key2 = stamp) {
        value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            // The version check stays next to the call so the API-28 requirement is obvious.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) decodeAnimated(file, limitPx) else null
        }
    }
}

/**
 * The GIF is decoded whole because a partial frame would freeze the animation; the target size keeps
 * the allocation bounded regardless of the original resolution.
 */
@androidx.annotation.RequiresApi(Build.VERSION_CODES.P)
private fun decodeAnimated(file: File, limitPx: Int): Drawable? = runCatching {
    val source = ImageDecoder.createSource(file)
    ImageDecoder.decodeDrawable(source) { decoder, info, _ ->
        val width = info.size.width
        val height = info.size.height
        val largest = max(width, height)
        if (largest > limitPx && largest > 0) {
            val scale = limitPx.toFloat() / largest
            decoder.setTargetSize(max(1, (width * scale).toInt()), max(1, (height * scale).toInt()))
        }
    }
}.getOrNull()

/** Decodes a still image (or the first frame of a GIF below API 28), honouring EXIF orientation. */
private fun decodeStill(file: File): Bitmap? = runCatching {
    if (file.length() <= 0) return null
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        val source = ImageDecoder.createSource(file)
        ImageDecoder.decodeBitmap(source) { decoder, info, _ ->
            decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
            decoder.isMutableRequired = false
            val width = info.size.width
            val height = info.size.height
            val largest = max(width, height)
            if (largest > DecodeLimitPx && largest > 0) {
                val scale = DecodeLimitPx.toFloat() / largest
                decoder.setTargetSize(max(1, (width * scale).toInt()), max(1, (height * scale).toInt()))
            }
        }
    } else {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.absolutePath, bounds)
        val decoded = BitmapFactory.decodeFile(
            file.absolutePath,
            BitmapFactory.Options().apply {
                inSampleSize = sampleSize(bounds.outWidth, bounds.outHeight)
                inPreferredConfig = Bitmap.Config.ARGB_8888
            },
        )
        // Below API 28 there is no ImageDecoder, so the orientation tag has to be applied by hand.
        decoded?.let { rotateForExif(it, file) }
    }
}.getOrNull()

private fun rotateForExif(bitmap: Bitmap, file: File): Bitmap {
    val degrees = runCatching {
        when (android.media.ExifInterface(file.absolutePath).getAttributeInt(
            android.media.ExifInterface.TAG_ORIENTATION,
            android.media.ExifInterface.ORIENTATION_NORMAL,
        )) {
            android.media.ExifInterface.ORIENTATION_ROTATE_90 -> 90f
            android.media.ExifInterface.ORIENTATION_ROTATE_180 -> 180f
            android.media.ExifInterface.ORIENTATION_ROTATE_270 -> 270f
            else -> 0f
        }
    }.getOrDefault(0f)
    if (degrees == 0f) return bitmap
    val matrix = android.graphics.Matrix().apply { postRotate(degrees) }
    return runCatching {
        Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, matrix, true)
    }.getOrDefault(bitmap)
}

private fun sampleSize(width: Int, height: Int): Int {
    var sample = 1
    while (max(width, height) / sample > DecodeLimitPx) sample *= 2
    return sample
}

/** Small LRU of decoded thumbnails, so scrolling back up does not decode the same JPEG again. */
private object ThumbnailCache {
    private val entries = object : LinkedHashMap<String, Bitmap>(8, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Bitmap>?) = size > 8
    }

    fun peek(file: File): Bitmap? = synchronized(entries) { entries[key(file)] }

    fun put(file: File, bitmap: Bitmap) = synchronized(entries) { entries[key(file)] = bitmap }

    private fun key(file: File): String = "${file.absolutePath}:${file.length()}:${file.lastModified()}"
}

/** Desktop `AttachmentStore::sizeLabel`: "512 B", "1.5 KB", "2.0 MB", "1.0 GB". */
fun attachmentSizeLabel(bytes: Long): String = when {
    bytes < 1024 -> "$bytes B"
    bytes < 1024L * 1024 -> "%.1f KB".format(bytes / 1024.0)
    bytes < 1024L * 1024 * 1024 -> "%.1f MB".format(bytes / 1024.0 / 1024.0)
    else -> "%.1f GB".format(bytes / 1024.0 / 1024.0 / 1024.0)
}

/**
 * Desktop sizing rule: keep the source aspect ratio, cap the longest side at [limit], and never
 * upscale a small image past its natural size.
 */
@Composable
private fun fitWithin(widthPx: Int, heightPx: Int, limit: Dp): Pair<Dp, Dp> {
    if (widthPx <= 0 || heightPx <= 0) return limit to limit
    val density = LocalDensity.current
    val naturalWidth = with(density) { widthPx.toDp() }
    val naturalHeight = with(density) { heightPx.toDp() }
    val largest = max(naturalWidth.value, naturalHeight.value)
    val scale = min(1f, limit.value / largest)
    return (naturalWidth.value * scale).dp to (naturalHeight.value * scale).dp
}

/** Mid-string ellipsis, matching `ElideMiddle` on the desktop file and video cards. */
private fun String.elideMiddle(limit: Int): String {
    if (length <= limit || limit <= 6) return this
    val head = (limit - 3) / 2
    val tail = limit - 3 - head
    return "${take(head)}…${takeLast(tail)}"
}

/** The relay UI is light, but the shared theme may be dark; pick the desktop's matching palette. */
private fun Color.luminanceIsDark(): Boolean = (red * 0.299f + green * 0.587f + blue * 0.114f) < 0.5f

private val ImageMaxSize = 240.dp
private val GifMaxSize = 134.dp

/** The desktop placeholder is a 134x134 square, used for both images and GIFs. */
private val PlaceholderSize = 134.dp
private val VideoWidth = 240.dp
private val VideoHeight = 144.dp
private val FileWidth = 260.dp
private val FileHeight = 82.dp
private val VoiceWidth = 166.dp

private const val DecodeLimitPx = 480

private val VideoFill = Color(0xFF525559)
private val PlaceholderFill = Color(0xFFEEEEF0)
private val PlaceholderFillDark = Color(0xFF303234)
private val PlaceholderText = Color(0xFF9F9FA6)
private val PlaceholderTextDark = Color(0xFF999CA3)
private val FileFill = Color(0xFFFFFFFF)
private val FileFillDark = Color(0xFF292929)
private val FileBorder = Color(0xFFE6E6E6)
private val FileBorderDark = Color(0xFF44474B)
private val FileSubText = Color(0xFF9F9FA6)
private val FileSubTextDark = Color(0xFF999CA3)
private val BadgeFill = Color(0xFFE99538)
