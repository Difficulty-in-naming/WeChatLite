package local.wechatlite.mobile

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import kotlinx.coroutines.runBlocking
import local.wechatlite.mobile.data.ConnectionConfig
import local.wechatlite.mobile.data.Repository
import local.wechatlite.mobile.net.ApiClient
import local.wechatlite.mobile.net.ApiException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Read-only checks against a **demo-mode** relay.
 *
 * This suite deliberately contains no test that sends, recalls, uploads or otherwise writes to a
 * WeChat group. Group messages belong to the user; only the user decides when something is sent.
 * Anything that would reach a real WeChat account is excluded by design, not merely disabled.
 *
 * The relay must be started separately in demo mode, reachable from the emulator at 10.0.2.2:
 *
 *   wechat-relay --config <tmp config> --demo
 *   adb shell am instrument -w -e connection '<json>' \
 *     local.wechatlite.mobile.test/androidx.test.runner.AndroidJUnitRunner
 *
 * The invitation arrives through instrumentation arguments so no credential is committed here.
 * Without the argument every test is skipped.
 */
@RunWith(AndroidJUnit4::class)
class RelayReadOnlyTest {
    private val args = InstrumentationRegistry.getArguments()
    private val invitation = args.getString("connection")
    private val pinMismatch = "0".repeat(64)

    private fun config(): ConnectionConfig {
        assumeTrue("未提供 -e connection，跳过只读联调测试", !invitation.isNullOrBlank())
        return ConnectionConfig.parse(invitation!!) ?: error("连接信息解析失败")
    }

    @Test
    fun parsesInvitationAndRejectsMalformedPins() {
        val config = config()
        assertTrue(config is ConnectionConfig.Direct)
        assertEquals(64, config.certificatePin?.length)

        // A pin containing non-hex characters must be rejected rather than silently ignored.
        val broken = invitation!!.replace(config.certificatePin!!, pinMismatch.replace('0', 'z'))
        assertEquals(null, ConnectionConfig.parse(broken))
    }

    /** Pure local parsing; no network access at all. */
    @Test
    fun rejectsInvitationsThatAreMissingRequiredFields() {
        assertEquals(null, ConnectionConfig.parse(""))
        assertEquals(null, ConnectionConfig.parse("{}"))
        assertEquals(null, ConnectionConfig.parse("""{"serverUrl":"https://a","token":"short"}"""))
        assertEquals(
            null,
            ConnectionConfig.parse("""{"serverUrl":"ftp://a","token":"${"x".repeat(32)}"}"""),
        )
        assertEquals(
            null,
            ConnectionConfig.parse("""{"serverUrl":"https://a","token":"${"x".repeat(32)}","mode":"p2p"}"""),
        )
    }

    /** Read-only: status, groups, members and history. */
    @Test
    fun readsStatusGroupsMembersAndHistory() = runBlocking {
        val config = config()
        val baseUrl = config.resolveBaseUrl() ?: error("直连邀请应能给出地址")
        val client = ApiClient(config, baseUrl)
        try {
            val repo = Repository(client, File(cacheDir(), "readonly"))
            val status = repo.status()
            assertTrue("演示服务端应报告微信已就绪", status.ready)

            val groups = repo.groups()
            assertTrue("应返回演示群", groups.isNotEmpty())
            val group = groups.firstOrNull { it.id == "demo-design" } ?: groups.first()

            val members = repo.members(group.id)
            assertTrue("应返回成员列表", members.isNotEmpty())

            val page = repo.history(group.id)
            assertTrue("演示群应有历史消息", page.messages.isNotEmpty())
            assertTrue("历史页只能包含当前群的消息", page.messages.all { it.group == group.id })
        } finally {
            client.close()
        }
    }

    @Test
    fun wrongCertificatePinIsRejectedBeforeAnyCredentialIsSent() = runBlocking {
        val config = config() as ConnectionConfig.Direct
        val baseUrl = config.resolveBaseUrl()!!
        val client = ApiClient(config.copy(certificatePin = pinMismatch), baseUrl)
        try {
            client.get("/api/status")
            fail("指纹不匹配时必须拒绝连接")
        } catch (e: ApiException) {
            assertTrue("应报告指纹不匹配，实际：${e.message}", e.message!!.contains("指纹"))
        } finally {
            client.close()
        }
    }

    @Test
    fun invalidClientKeyIsRejected() = runBlocking {
        val config = config() as ConnectionConfig.Direct
        val baseUrl = config.resolveBaseUrl()!!
        val client = ApiClient(config.copy(token = "wxcl_" + "0".repeat(64)), baseUrl)
        try {
            client.get("/api/status")
            fail("无效密钥必须被拒绝")
        } catch (e: ApiException) {
            assertEquals(401, e.status)
        } finally {
            client.close()
        }
    }

    private fun cacheDir(): File =
        File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "readonly")
            .apply { mkdirs() }
}
