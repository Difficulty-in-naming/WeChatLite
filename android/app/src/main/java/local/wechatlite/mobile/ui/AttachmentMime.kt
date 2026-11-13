package local.wechatlite.mobile.ui

import java.util.Locale

/**
 * Concrete MIME type for a downloaded attachment, derived from its file extension.
 *
 * Used only when `android.webkit.MimeTypeMap` has no answer. The type must never be a wildcard:
 * launching `ACTION_VIEW` with a wildcard type matches no viewer on a normal phone, which made
 * every document look as if tapping it did nothing while images and videos opened normally.
 */
fun fallbackMimeFor(extension: String): String = when (extension.lowercase(Locale.ROOT)) {
    "jpg", "jpeg", "png", "webp", "gif", "bmp", "heic" -> "image/*"
    "mp4", "mov", "m4v", "webm", "mkv", "3gp" -> "video/*"
    "mp3", "wav", "ogg", "opus", "m4a", "amr", "aac", "flac" -> "audio/*"
    "pdf" -> "application/pdf"
    "zip" -> "application/zip"
    "rar" -> "application/vnd.rar"
    "7z" -> "application/x-7z-compressed"
    "apk" -> "application/vnd.android.package-archive"
    "txt", "md", "log", "json", "xml", "csv", "ini", "yaml", "yml" -> "text/plain"
    "doc" -> "application/msword"
    "docx" -> "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
    "xls" -> "application/vnd.ms-excel"
    "xlsx" -> "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
    "ppt" -> "application/vnd.ms-powerpoint"
    "pptx" -> "application/vnd.openxmlformats-officedocument.presentationml.presentation"
    // A concrete generic type beats a wildcard: file managers and "open with" handlers do
    // advertise this one, so an unknown document still has a chance to open.
    else -> "application/octet-stream"
}
