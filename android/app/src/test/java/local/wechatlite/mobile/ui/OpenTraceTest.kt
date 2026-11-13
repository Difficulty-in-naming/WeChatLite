package local.wechatlite.mobile.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The trace is what the settings screen prints and what logcat repeats, so its arithmetic and its
 * formatting must not drift. A fake clock keeps the assertions free of real waiting.
 */
class OpenTraceTest {

    private class FakeClock {
        var now = 0L
        fun tick(ms: Long) { now += ms }
    }

    @Test
    fun marksRecordCumulativeMillisecondsFromTheTap() {
        val clock = FakeClock()
        val trace = OpenTrace("g", clock = { clock.now })
        clock.tick(120)
        trace.mark("activate")
        clock.tick(80)
        trace.mark("members")
        clock.tick(400)
        trace.mark("history")

        assertEquals(120L, trace.millisOf("activate"))
        assertEquals(200L, trace.millisOf("members"))
        assertEquals(600L, trace.millisOf("history"))
        assertEquals(600L, trace.elapsed())
    }

    @Test
    fun aRepeatedMarkKeepsTheFirstTiming() {
        val clock = FakeClock()
        val trace = OpenTrace("g", clock = { clock.now })
        clock.tick(50)
        trace.mark("rows")
        clock.tick(500)
        trace.mark("rows")
        assertEquals(50L, trace.millisOf("rows"))
    }

    @Test
    fun reportUsesTheLabelsTheSettingsScreenShows() {
        val clock = FakeClock()
        val trace = OpenTrace("jx程序组", clock = { clock.now })
        clock.tick(120)
        trace.mark("activate")
        clock.tick(30)
        trace.mark("rows")

        val report = trace.report()
        assertEquals("jx程序组：激活 120ms · 出首屏 150ms", report)
    }

    @Test
    fun anEmptyTraceReportsNothingSoTheScreenCanShowItsPlaceholder() {
        val trace = OpenTrace("g", clock = { 0L })
        assertEquals("", trace.report())
        assertFalse(trace.hasRows())
    }

    @Test
    fun rowsMarksTheEndOfTheBlankPeriod() {
        val clock = FakeClock()
        val trace = OpenTrace("g", clock = { clock.now })
        assertFalse(trace.hasRows())
        clock.tick(10)
        trace.mark("rows")
        assertTrue(trace.hasRows())
    }

    @Test
    fun theLabelOfEveryMilestoneIsTranslatedButUnknownOnesPassThrough() {
        val clock = FakeClock()
        val trace = OpenTrace("g", clock = { clock.now })
        for (name in listOf("activate", "members", "history", "rows", "preview", "previewAll", "extra")) {
            trace.mark(name)
        }
        val report = trace.report()
        for (label in listOf("激活", "成员", "历史", "出首屏", "首图", "预览完", "extra")) {
            assertTrue("missing $label in $report", report.contains(label))
        }
        // The raw English names must not leak into a user-visible string.
        assertFalse(report.contains("activate"))
    }
}
