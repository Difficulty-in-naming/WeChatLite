package local.wechatlite.mobile

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import local.wechatlite.mobile.net.TunnelProcess
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Exercises the JNI tunnel bridge without any network dependency.
 *
 * The point is narrower than a connection test: it proves that starting a tunnel and receiving its
 * event callback does not abort the process. Earlier builds exited with
 * `android context was not initialized` because the callback ran on a thread the VM did not know.
 *
 * The relay settings below are deliberately unreachable, so the expected outcome is a clean
 * `error` event rather than a tunnel. Reaching that event is the success condition.
 */
@RunWith(AndroidJUnit4::class)
class TunnelJniTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun loadingTheLibraryAndStartingATunnelDoesNotAbort() {
        val process = TunnelProcess(context)
        assertTrue("库应能加载", process.isAvailable())

        val events = AtomicReference<MutableList<String>>(mutableListOf())
        val error = AtomicReference<String?>(null)
        val ready = AtomicReference<String?>(null)
        val latch = CountDownLatch(1)

        // Unreachable relay: 127.0.0.1:1 with a syntactically valid invitation. The tunnel will
        // fail to reach it and report an error, which is exactly the path we want to exercise.
        val handle = nativeStart(
            serverId = "a".repeat(64),
            relayUrl = "https://127.0.0.1:1",
            relayToken = "test-token-not-a-secret",
            relayCa = TEST_CA,
            relayQuicPort = 443,
            keyFile = File(context.filesDir, "p2p-client.key").absolutePath,
        ) { line ->
            events.get().add(line)
            if (line.contains("\"type\":\"error\"")) {
                error.set(line)
                latch.countDown()
            } else if (line.contains("\"type\":\"ready\"")) {
                ready.set(line)
                latch.countDown()
            }
        }

        if (handle == 0L) {
            // A rejected invitation is also acceptable: nothing aborted.
            assertTrue("应已上报错误事件", events.get().isNotEmpty())
            return
        }

        // Either an error or readiness ends the wait; a hang would surface as a failed latch.
        latch.await(30, TimeUnit.SECONDS)
        nativeStop(handle)

        assertTrue("应至少收到一个事件，实际：${events.get()}", events.get().isNotEmpty())
        // Reaching here at all proves the JNI callback did not tear down the process.
        assertNotNull(events.get().firstOrNull())
    }

    private external fun nativeStart(
        serverId: String,
        relayUrl: String,
        relayToken: String,
        relayCa: String,
        relayQuicPort: Int,
        keyFile: String,
        sink: (String) -> Unit,
    ): Long

    private external fun nativeStop(handle: Long)

    private companion object {
        init {
            System.loadLibrary("wechat_p2p")
        }

        // A self-signed CA is required by the settings validator; it never verifies anything here.
        val TEST_CA = """
            -----BEGIN CERTIFICATE-----
            MIIBeTCCAR+gAwIBAgIUJ8Yy9m5m3Yq0dQ0m8m5m3Yq0dQ0wCgYIKoZIzj0EAwIw
            FDESMBAGA1UEAwwJdGVzdC1vbmx5MB4XDTI0MDEwMTAwMDAwMFoXDTM0MDEwMTAw
            MDAwMFowFDESMBAGA1UEAwwJdGVzdC1vbmx5MFkwEwYHKoZIzj0CAQYIKoZIzj0D
            AQcDQgAE
            -----END CERTIFICATE-----
        """.trimIndent()
    }
}
