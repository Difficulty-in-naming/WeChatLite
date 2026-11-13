package local.wechatlite.mobile.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

/**
 * The MIME fallback exists because a wildcard type silently killed every document tap. These cases
 * pin that down: no answer may be a wildcard, and common office/archive types must map to the exact
 * type their viewers declare.
 */
class AttachmentMimeTest {

    @Test
    fun officeDocumentsUseTheirLongFormTypes() {
        assertEquals(
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
            fallbackMimeFor("docx"),
        )
        assertEquals(
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            fallbackMimeFor("xlsx"),
        )
        assertEquals(
            "application/vnd.openxmlformats-officedocument.presentationml.presentation",
            fallbackMimeFor("pptx"),
        )
        assertEquals("application/pdf", fallbackMimeFor("pdf"))
    }

    @Test
    fun archivesAndPlainTextAreConcrete() {
        assertEquals("application/zip", fallbackMimeFor("zip"))
        assertEquals("text/plain", fallbackMimeFor("txt"))
        assertEquals("application/octet-stream", fallbackMimeFor(""))
    }

    @Test
    fun extensionMatchingIgnoresCase() {
        assertEquals("application/pdf", fallbackMimeFor("PDF"))
        assertEquals("image/*", fallbackMimeFor("JPG"))
    }

    @Test
    fun noAnswerIsEverAWildcard() {
        for (extension in listOf("", "xyz", "bin", "dat", "weird", "noext")) {
            val mime = fallbackMimeFor(extension)
            assertFalse("$extension must not fall back to a wildcard", mime == "*/*")
        }
    }
}
