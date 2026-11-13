package local.wechatlite.mobile.data

import org.json.JSONObject
import java.net.URI

/**
 * Connection settings copied from the desktop server's "copy client connection info" dialog.
 *
 * The JSON shape is identical to [client/connection_info.cpp] so one invitation works on every
 * platform. P2P invitations are parsed and kept intact, but establishing the QUIC tunnel needs the
 * Rust helper, which this client does not ship yet; [ConnectionConfig.isSupported] reports that.
 */
sealed interface ConnectionConfig {
    val token: String
    val certificatePin: String?

    /** A plain HTTPS endpoint, optionally on the local network. */
    data class Direct(
        override val token: String,
        override val certificatePin: String?,
        val serverUrl: String,
    ) : ConnectionConfig

    /** `p2p://<server_id>` reached through a self-hosted iroh relay. */
    data class P2p(
        override val token: String,
        override val certificatePin: String,
        val serverUrl: String,
        val relayUrl: String,
        val relayToken: String,
        val relayCa: String,
        val relayQuicPort: Int,
        val serverId: String,
    ) : ConnectionConfig

    val isSupported: Boolean get() = true

    /**
     * Converts the invitation into a request base URL.
     *
     * A direct invitation already knows its address. A P2P invitation only becomes reachable once
     * the bundled helper reports its loopback listener, so [tunnelUrl] must be supplied then.
     */
    fun resolveBaseUrl(tunnelUrl: String? = null): String? = when (this) {
        is Direct -> serverUrl.trimEnd('/')
        is P2p -> tunnelUrl
    }

    companion object {
        const val MAX_DIRECT_BYTES = 8192
        const val MAX_P2P_BYTES = 64 * 1024

        /** Normalizes a SHA-256 pin exactly like the desktop client: drop separators, lowercase. */
        fun normalizePin(value: String?): String? {
            val cleaned = value?.replace(":", "")?.replace(" ", "")?.trim()?.lowercase()
            return cleaned?.takeIf { it.length == 64 && it.all { ch -> ch in '0'..'9' || ch in 'a'..'f' } }
        }

        /**
         * Parses an invitation. Returns null when the payload is malformed so callers can show the
         * same "copy the complete JSON" message the desktop client uses.
         */
        fun parse(raw: String): ConnectionConfig? {
            if (raw.length > MAX_P2P_BYTES) return null
            val json = runCatching { JSONObject(raw) }.getOrNull() ?: return null

            val token = json.optString("token").trim()
            if (token.length < 24 || token.length > 512) return null
            val mode = json.optString("mode", "direct")

            val pin = normalizePin(json.optString("certificatePin").ifBlank { null })
            val declaredPin = json.optString("certificatePin").trim()

            return when (mode) {
                "direct" -> {
                    if (raw.toByteArray().size > MAX_DIRECT_BYTES) return null
                    if (json.has("version") && json.optDouble("version", -1.0) != 1.0) return null
                    val url = json.optString("serverUrl").trim()
                    if (!validDirectUrl(url)) return null
                    // An explicitly supplied pin must be well formed; a garbage pin is a mistake.
                    if (declaredPin.isNotEmpty() && pin == null) return null
                    Direct(token = token, certificatePin = pin, serverUrl = url)
                }

                "p2p" -> {
                    if (json.optDouble("version", -1.0) != 1.0) return null
                    if (pin == null) return null
                    val p2p = json.optJSONObject("p2p") ?: return null
                    val serverId = p2p.optString("server_id")
                    if (!serverId.matches(Regex("^[a-z0-9]{1,128}$"))) return null
                    if (json.optString("serverUrl").trim() != "p2p://$serverId") return null

                    val relayUrl = p2p.optString("relay_url")
                    val relayToken = p2p.optString("relay_token")
                    val relayCa = p2p.optString("relay_ca")
                    val port = p2p.optDouble("relay_quic_port", -1.0)
                    if (relayUrl.isEmpty() || relayUrl.length > 2048) return null
                    if (relayToken.isEmpty() || relayToken.length > 512) return null
                    if (relayCa.isEmpty() || relayCa.length > 16 * 1024) return null
                    if (port < 1 || port > 65535 || port != port.toInt().toDouble()) return null
                    if (relayUrl.any { it.code <= 0x20 || it.code == 0x7f }) return null
                    if (relayToken.any { it.code <= 0x20 || it.code == 0x7f }) return null
                    val relay = runCatching { URI(relayUrl) }.getOrNull() ?: return null
                    if (relay.host.isNullOrEmpty() || relay.port <= 0) return null
                    if (relay.userInfo != null || relay.query != null || relay.fragment != null) return null
                    if (relay.path?.let { it.isNotEmpty() && it != "/" } == true) return null

                    P2p(
                        token = token,
                        certificatePin = pin,
                        serverUrl = "p2p://$serverId",
                        relayUrl = relayUrl,
                        relayToken = relayToken,
                        relayCa = relayCa,
                        relayQuicPort = port.toInt(),
                        serverId = serverId,
                    )
                }

                else -> null
            }
        }

        private fun validDirectUrl(value: String): Boolean {
            if (value.isEmpty() || value.length > 2048) return false
            if (value.any { it.code <= 0x20 || it.code == 0x7f }) return false
            val uri = runCatching { URI(value.trim()) }.getOrNull() ?: return false
            if (uri.host.isNullOrEmpty()) return false
            if (uri.userInfo != null || uri.query != null || uri.fragment != null) return false
            return uri.scheme == "https" || uri.scheme == "http"
        }
    }
}
