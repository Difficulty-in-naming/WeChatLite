package local.wechatlite.mobile.net

import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager
import okhttp3.OkHttpClient
import java.util.concurrent.TimeUnit

/** Thrown when the leaf certificate does not match the pinned SHA-256 fingerprint. */
class CertificatePinException : CertificateException("服务器证书指纹不匹配，已拒绝连接")

/**
 * Leaf-certificate pinning that mirrors the desktop client.
 *
 * With a pin configured the desktop client clears the CA list, so the only thing that authorizes a
 * peer is the SHA-256 digest of its leaf certificate. OkHttp's [CertificatePinner] still requires a
 * normally trusted chain, which is stricter than the server's self-signed certificate allows, so the
 * trust manager itself performs the comparison.
 */
object PinnedTls {
    const val TIMEOUT_SECONDS = 180L

    fun trustManager(pin: String): X509TrustManager = object : X509TrustManager {
        override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {
            throw CertificateException("客户端证书不受信任")
        }

        override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
            val leaf = chain?.firstOrNull() ?: throw CertificateException("服务器未提供证书")
            val digest = MessageDigest.getInstance("SHA-256").digest(leaf.encoded)
            val actual = digest.joinToString("") { "%02x".format(it) }
            if (!MessageDigest.isEqual(actual.toByteArray(), pin.toByteArray())) {
                throw CertificatePinException()
            }
        }

        override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
    }

    /**
     * Builds a client for the given pin. A null pin keeps the platform trust store and hostname
     * verification, matching a desktop connection that did not receive a certificate fingerprint.
     */
    fun client(pin: String?): OkHttpClient {
        val builder = OkHttpClient.Builder()
            .connectTimeout(20, TimeUnit.SECONDS)
            .readTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .writeTimeout(TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .callTimeout(TIMEOUT_SECONDS + 30, TimeUnit.SECONDS)
            // The desktop client never follows redirects; a redirect could move the request to a
            // host that was never covered by the certificate pin.
            .followRedirects(false)
            .followSslRedirects(false)
            .retryOnConnectionFailure(false)

        // Never route through a system proxy. The address handed to this client is either the
        // relay's own HTTPS endpoint or the P2P tunnel's `127.0.0.1:<port>` loopback listener; a
        // VPN/proxy app (Clash and friends) publishes itself as the default proxy, and sending a
        // loopback connection there makes the handshake fail with a bare "connection closed".
        builder.proxy(java.net.Proxy.NO_PROXY)

        if (pin != null) {
            val trustManager = trustManager(pin)
            val context = SSLContext.getInstance("TLS")
            context.init(null, arrayOf(trustManager), null)
            builder.sslSocketFactory(context.socketFactory, trustManager)
            // The pin is the whole identity check, so a certificate issued for a different host
            // name is still accepted; this is what the desktop client does as well.
            builder.hostnameVerifier { _, _ -> true }
        }

        return builder.build()
    }
}
