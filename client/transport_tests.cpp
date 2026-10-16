#include "transport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QProcess>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace
{
// Public, synthetic offline identities. They never authenticate a real service.
const QByteArray caPem = R"PEM(-----BEGIN CERTIFICATE-----
MIIDHDCCAgSgAwIBAgIURzTyqxi/fS/A9MxhRzIDUAFGFagwDQYJKoZIhvcNAQEL
BQAwJTEjMCEGA1UEAwwaV2VDaGF0TGl0ZSBPZmZsaW5lIFRlc3QgQ0EwIBcNMjYx
MDE1MTI1NDQ4WhgPMjEyNjA5MjExMjU0NDhaMCUxIzAhBgNVBAMMGldlQ2hhdExp
dGUgT2ZmbGluZSBUZXN0IENBMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC
AQEA2gtcKkfRTfUv7jUCMUQcjcwyRSQ04iHdMCjqs8GTEWzEBc1uGEpZxvsEAyU4
9o/xRAUj6EoW6fngozW00APgHntemQRVj1hjEnitHRfy+IR8yvkpNTFQQ0zCL5DS
I+HQbsnFoHt/kZyIwGht2CFEwLgGld8lgegLAnl7jJ27CvOufoovbGdc3oSKf7Lu
gwbrhLaIEAp/Pva8FWV5R/NT6rgmIjspEUbrrYiQ4tThxKGytRtgCFEdUW77TkwU
9D8RzughoQLNr6a6ADATRtYtFulV3loNP6keiW6bDbyBvhZrlG15YHYbHUxcXLWC
FyNnshJKDKHsGntPJsD4/OjKOwIDAQABo0IwQDAPBgNVHRMBAf8EBTADAQH/MA4G
A1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUpEYKjrV/lGpUMGVe3hIyAL0PnDYwDQYJ
KoZIhvcNAQELBQADggEBADRTRaMekJWd/Ax9NAlMLxVMBfKSxqT5B56VFK9ekk3v
8avp8dcMheXCEnnkd3CMxr9wbPKU3GZFd5MKRC2fSBTlFcfGzSF+MKiViUBtA/rd
NZzEWmJwcxQ2sVXWifMrDcZKjJkXQ9fkqVFJQvWVTypRJPBmPk9QWZNZJma1OQmk
TLAkRU/+MrsjDKxt1ZEtgmtAkEUre2+LsTeqykOQtRndy1bb5UaMAivqfCo/aJyb
Auilq7KtQ+g75obRiFAQSGy5RVxwofwbMrbXRgwcc88zOAARKj5bZPPThoWLuIpc
fvcUfODzhFQ7MHOLT2xIzhLo6VSVHuZSqbLDnepNJgU=
-----END CERTIFICATE-----
)PEM";
const QByteArray trustedPem = R"PEM(-----BEGIN CERTIFICATE-----
MIIDGjCCAgKgAwIBAgIUYGnfgk/dCVHdYgisuLkOUwr8vGAwDQYJKoZIhvcNAQEL
BQAwJTEjMCEGA1UEAwwaV2VDaGF0TGl0ZSBPZmZsaW5lIFRlc3QgQ0EwIBcNMjYx
MDE1MTI1NDQ4WhgPMjEyNjA5MjExMjU0NDhaMBQxEjAQBgNVBAMMCWxvY2FsaG9z
dDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAPFuEPSwyoNeH6WsMGw6
2s9MHLry3L3caeXCO33dNWU7VdLZVFMrnHlMIcZh9fGz7TyKY7SqmWCZZHe2MPG4
xuuXTQwvvlfOOt0nRI3geydOMM23KdBFa/3IAhdY7d69/9uw+csWaeIVxWaj6Du3
swGo2bz79xGEgcjcEoUQjz/pOmoJkZjUbqJ2wGLRJHEk7kTpCdL7uxH1KhCwTe3v
xk4HQFPKZfM9v0Frk8mfu/53iOzHUxamaGWsA1C9cMhRqoMwL6D3btAK/sfWm1FW
jTU/W+M+yxk7R4ibIQ4sZCfYtqlIz8O3025A3ecounDCvF0Z7v1ER1bmiXqCtMbh
Z08CAwEAAaNRME8wDAYDVR0TAQH/BAIwADAOBgNVHQ8BAf8EBAMCBaAwEwYDVR0l
BAwwCgYIKwYBBQUHAwEwGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMA0GCSqG
SIb3DQEBCwUAA4IBAQBR5KR4tdhrZ7YEQjiu6aZi0yGklFV/PuLb2/4t8DSMQJli
s3xNoSwy/EnxsvTo6wPrG44/rAE+VK5wQRvmcwH2Uddca32v/5Qdiu1BH0jmdCQU
IbDUL4sWikhUc92PedKodp6W6W5geSUAaQ+86HzUpkBZtL1wCi2jSx3oTR4JFIlF
zhjXRiaic2QrZ1oXMkJZl2EfwMx+gT7udxvU1Kj6brIVN+ygs42V+lqoUzE97Ov2
PCz905z6HWTxmadd41ddNICNlAlb4CaO4JBXk2cR08x6Nu4+WRmRCGtnAy79GYy4
b79snSa5PBEFv1+TSOiOD3aobGEtRfjAZ4LS9k0N
-----END CERTIFICATE-----
)PEM";
const QByteArray trustedKey = /* gitleaks:allow - synthetic offline test key, never a service credential */ R"PEM(-----BEGIN PRIVATE KEY-----
MIIEwAIBADANBgkqhkiG9w0BAQEFAASCBKowggSmAgEAAoIBAQDxbhD0sMqDXh+l
rDBsOtrPTBy68ty93Gnlwjt93TVlO1XS2VRTK5x5TCHGYfXxs+08imO0qplgmWR3
tjDxuMbrl00ML75XzjrdJ0SN4HsnTjDNtynQRWv9yAIXWO3evf/bsPnLFmniFcVm
o+g7t7MBqNm8+/cRhIHI3BKFEI8/6TpqCZGY1G6idsBi0SRxJO5E6QnS+7sR9SoQ
sE3t78ZOB0BTymXzPb9Ba5PJn7v+d4jsx1MWpmhlrANQvXDIUaqDMC+g927QCv7H
1ptRVo01P1vjPssZO0eImyEOLGQn2LapSM/Dt9NuQN3nKLpwwrxdGe79REdW5ol6
grTG4WdPAgMBAAECggEBALgIElyytmvUv4Ll8jNN2aTSe8P1hFYlQYOlvvt+N0ln
JVCH4UNUpv2/Dq/N1E/duyS+yKnasPd1daPa+QwIsXi76Ds12X3M8gnJ+PO8/aNE
cucci376rArS/uUVIJz9b57jFEW+XA++UK6gmlGLAaRuNStdVUPBIb0eI+/SY59u
ic+9ky7wZU4NudeW134lfZQr/PFvwXblRQDFsBHeN1IEObdfjT7q8YQX9ZPtrhYz
ZsAt+H1TX32aH+dvDznlvIrhCYSjCHO0Iwyrnu7E27sXFFtWMvZy5OR/xcQ6wmik
RwKdHWCzdrlfld1wtSQwqED/jEs0+V9Kj+ttV7BToiECgYEA/B+hAOXzPGngnEnR
a6vt14L5Hn5zmedCiNMpw6Tk3acQTlHEkB2rQVngjNXEZd09SPf8JqYvTTC+60/p
v2Q1SdYnj67a+ecHyOb/0fr78QyYiC/KAVtC1MZDPO9jil1IbObHPSbQJgThxj4Y
QqRMzWvM0+Cn0Ob82v930cYpcyMCgYEA9SRYxEooSDkvlwC/CN8j4jZrZMtVtcOU
IRi8/8hIYFbMk8fNRyz+0lx/UHBInyT4lvj4t4Bp1wE4yQlrwRZwWHk6ICHEKoJA
u0E/g1MDisGX9/2a4dLuM9/Q659dtugbp/Q2l0bz0K83iP0iVDrf2C5oKB0oe1W3
dyVvrhv8A+UCgYEAi1vsQbPZI/6eEaVX5X0OJX6Wtr1MprhGdZzKzAKcIsySXcgi
r0pG/ANy9EXAnlGQ64FvVemr4a3dCfKNEKaSMfL2wRv0nAeQrQLKSsXBdvbwZO6N
aPyJz1Sbm9Xynmt7ef6CCJIooas3JL3z6PoqHa3FdSQLfpP/Fgn5r/EpviMCgYEA
jQKFVRPbLABJEbFKMg45E+x9xhL4kH+Z+5fBT9ijWVdUgkVSHXkSc2rliR2tiG25
2Q4J83QYbaa0RV/37a+8nAfPmaASuOkH1y7nG+FqjixiADKPHws4HfrGsGKRg57s
GaZuqIIVyZqgdVo2nwDmtfGr1DdBjAB4cj7EiQBr7MkCgYEAnZ0wxziKhh80ZgTJ
Ke0Lih2z5PEuUbMZQenQnruxmrFUMDmUs6hFN+iENTwbEFLp8kfeUvMsX3Ah7k17
gSbudjKH04Qe+fDSsx7Yk9va00IiNzTve2qxqQl8ZqySZhp/I0xiIIqUGxwKcTxP
aQzXB8Em/9ZUdV7Y3CuxFigiKeY=
-----END PRIVATE KEY-----
)PEM";
const QByteArray selfSignedPem = R"PEM(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUaMBHorlPSeQJe1zzS7uC4s+Xql8wDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAxNTEyNTQ0OFoYDzIxMjYw
OTIxMTI1NDQ4WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEB
AQUAA4IBDwAwggEKAoIBAQDZnow7yJuiticAWemgatoznY25w3RXcAYwxY16sawD
6fywVIRvRgsslU84WUvfFCv6YuuT6cDaNF43AgcK+xFreLV2i1S01QD98XI+8c+F
uph8bjaMf5BQnll+8PpownnumVpceW+cbbhaGLxCGYSUJcPWArDp2QyKf4HZj4gG
ddchh3gL3TJMQfrMTYWNEcc5N2DC2LneQySJ+UTzCG+PY1m9hOiL47f1xEhvB4DC
d+r7RGHcLnFiXHodPpKrlEibuh+KOu6bSxZt8AD98Kj6R3N8cuCdvpfF/C1RjFq9
dDhNKYA1Gh8Y5TwfhS0Wg+/OlanU/+8aBtAXyvdFyJN5AgMBAAGjUTBPMAwGA1Ud
EwEB/wQCMAAwDgYDVR0PAQH/BAQDAgWgMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBoG
A1UdEQQTMBGCCWxvY2FsaG9zdIcEfwAAATANBgkqhkiG9w0BAQsFAAOCAQEACEJ+
II/rEpuO+ZhDCDWnQarz6Ck/w7G27xC2Ti628JqronrI9FtbkNckV4a34eyP0H9/
dQG5BmGcKMtXy8kXZnPW2/lyrLbwLftgK9KVxmbvpVtlzMPo34ztnt8WGmTf0+z9
ijeC+BHXNPhmUfbixJsb7DP+Zg26bLNIELC4szkZ17/asmgbyfJ772iu0qjaDNS7
dHcNdNOIyxYgD+NIgctLMhS3G+0YYc9yhVRdCXROGRabL8glrzpxruowvoZRsKXt
AlR1wKv2FcdCq6oezo1ItQVU4GsOxs9Uw3P6Yf1EdQ3q03toC+yKSZ5Ox81zQhV9
6UjkxV4NNqmaemDGfQ==
-----END CERTIFICATE-----
)PEM";
const QByteArray selfSignedKey = /* gitleaks:allow - synthetic offline test key, never a service credential */ R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDZnow7yJuiticA
WemgatoznY25w3RXcAYwxY16sawD6fywVIRvRgsslU84WUvfFCv6YuuT6cDaNF43
AgcK+xFreLV2i1S01QD98XI+8c+Fuph8bjaMf5BQnll+8PpownnumVpceW+cbbha
GLxCGYSUJcPWArDp2QyKf4HZj4gGddchh3gL3TJMQfrMTYWNEcc5N2DC2LneQySJ
+UTzCG+PY1m9hOiL47f1xEhvB4DCd+r7RGHcLnFiXHodPpKrlEibuh+KOu6bSxZt
8AD98Kj6R3N8cuCdvpfF/C1RjFq9dDhNKYA1Gh8Y5TwfhS0Wg+/OlanU/+8aBtAX
yvdFyJN5AgMBAAECggEBAKxVZSI21CE/dJVPnradx5xWaZSxVuGjYhOfblG6f8k9
tBEDbTkdU94Dg4/RD0dilw0a5wnkdAeYrv+KyH8Goh3kond1ESh/AeEYImv8XGW/
heAL+4C5yFQ/ggoH1ZW+ZIbxxGA0/pD3RqV6ryL9SEN1wKcqzGTzsH3BSdg7VPIf
S9vOGOa0HnEYypf/Q2WJWFjvVOLpE2XFKkeE7umCiIh3hjcwM5mSRSslbIVsIl+D
3lka7fREoyXfhNPUWFu/ZFeyhOyquSrH6WBXzsmKTQtHqj5W6fmPjKA3A1vIFO2/
RPAfWpfYlAfw0IM+IQ88UlNm+qsAPFlaqildWH7PLoECgYEA7kJLG9Hmd2EfGhr0
rxmmPDky/8fxrmIPSfbRJ3Gfa00u0415HOa9EoHfC1ZFM3zJ4g3CPGYiOHNEDqZA
svzmFZLtys31aoOFqNJpqbUjLz3s3X88EqI8/3xl4iLzkRu3mhlrvBJ04HlJJPdw
YIwMee1XwRvriXQXlNIGDlVrIXECgYEA6dLSDFLCmeIoXAKfhU9D1ez+UUX3x6Rk
HVwRVsvP7m/Ax+qw26me3woYzmnfkXzV6oSfuOW0lPH7jitVb/eRD8uGfw1B9dot
PXapgBNkMedNzsdeUEvF1RQkvbbjkC6VOjILAg8WLvCjpmVOogZGQ6ltY/csJWW8
Y1mo+13XjokCgYBvWBXHq4EbuwyQ45YDmmYAcO6vcB4202a3txupZkUNHSXMAape
4knrszusDaYLWL7z4GAsX8KQRPXCLtm/i8ZuwJpWE4gkPdChw6tTfu4xbqwaGxLU
1nLRJq5qTtEquBxGbKjXXHy4RpXzK5ud0vUlqdA/069wLguzJa9k1XBaUQKBgGUH
l4SFmdndsyyWYZC7QAdGAFjjj30uladt6XwEUlKm7Lg0urge4Stj9HXznwsA0nkj
VtQQRGd66wNxy9e2M3KMsQ9ijhaJb7S3+eMB/e/mDmODF0hAhTldG8bxKABbcFUV
3FXGaQJdLYivCzvM1D1WDpL78JI0FCMlUXNTB9lpAoGBAIGLbSdtEMLzi1idkmh0
yKOiWRfllXGop2qqJ+ZxGONzSuVw4DdraPyaLxNeBoC/Iz39M7d/zmBhqChPDOCC
/YXf9iUZcU+QIejuYWWQS1stY6UwUDnADurExIBY1FuTPy0rK4hjQGmqsmIJOEQ6
A0DhdLEfmfywLQdUackM4Bjt
-----END PRIVATE KEY-----
)PEM";

class TlsFixture : public QTcpServer
{
  public:
    explicit TlsFixture(bool selfSigned = false)
        : certificate(selfSigned ? selfSignedPem : trustedPem),
          key(selfSigned ? selfSignedKey : trustedKey, QSsl::Rsa)
    {
    }
    QString url() const { return QString("https://127.0.0.1:%1").arg(serverPort()); }
    QString pin() const
    {
        return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex());
    }
    int connections = 0;
    QList<QByteArray> requests;
    bool keepAlive = false;
    QString redirect;
    std::function<void(const QByteArray &)> onRequest;

  protected:
    void incomingConnection(qintptr descriptor) override
    {
        ++connections;
        auto *socket = new QSslSocket(this);
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QIODevice::readyRead, this,
                [this, socket]
                {
                    auto input = socket->property("requestBytes").toByteArray() + socket->readAll();
                    if (!input.contains("\r\n\r\n"))
                    {
                        socket->setProperty("requestBytes", input);
                        return;
                    }
                    socket->setProperty("requestBytes", QByteArray{});
                    requests.append(input);
                    if (onRequest)
                        onRequest(input);
                    if (!redirect.isEmpty())
                    {
                        socket->write("HTTP/1.1 302 Found\r\nLocation: " + redirect.toUtf8() +
                                      "\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                        socket->disconnectFromHost();
                        return;
                    }
                    if (input.toLower().contains("upgrade: websocket"))
                    {
                        QByteArray clientKey;
                        for (auto line : input.split('\n'))
                            if (line.toLower().startsWith("sec-websocket-key:"))
                                clientKey = line.mid(line.indexOf(':') + 1).trimmed();
                        auto accept = QCryptographicHash::hash(
                                          clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                          QCryptographicHash::Sha1)
                                          .toBase64();
                        socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                      "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                      accept + "\r\n\r\n");
                        const QByteArray message = "{\"type\":\"status\",\"ready\":true}";
                        socket->write(QByteArray("\x81", 1) + QByteArray(1, char(message.size())) +
                                      message);
                    }
                    else
                    {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                      "Content-Length: 11\r\nConnection: " +
                                      QByteArray(keepAlive ? "keep-alive" : "close") +
                                      "\r\n\r\n{\"ok\":true}");
                        if (!keepAlive)
                            socket->disconnectFromHost();
                    }
                });
        if (!socket->setSocketDescriptor(descriptor))
            socket->deleteLater();
        else
            socket->startServerEncryption();
    }

  private:
    QSslCertificate certificate;
    QSslKey key;
};

void pinCases()
{
    QTest::addColumn<bool>("selfSigned");
    QTest::addColumn<QString>("pinMode");
    QTest::addColumn<bool>("succeeds");
    QTest::newRow("trusted-no-pin") << false << QString("none") << true;
    QTest::newRow("trusted-correct-pin") << false << QString("right") << true;
    QTest::newRow("trusted-wrong-pin") << false << QString("wrong") << false;
    QTest::newRow("self-signed-correct-pin") << true << QString("right") << true;
    QTest::newRow("self-signed-wrong-pin") << true << QString("wrong") << false;
    QTest::newRow("self-signed-no-pin") << true << QString("none") << false;
    QTest::newRow("malformed-explicit-pin") << false << QString("malformed") << false;
}

QString fixturePin(const TlsFixture &fixture, const QString &mode)
{
    if (mode == "none")
        return {};
    if (mode == "malformed")
        return "not-a-sha256-fingerprint";
    return mode == "right" ? fixture.pin() : QString(64, '0');
}
} // namespace

class TransportTests : public QObject
{
    Q_OBJECT
    QTemporaryDir files;
    QSslConfiguration original;

  private slots:
    void initTestCase()
    {
        QVERIFY(files.isValid());
        QVERIFY2(QSslSocket::supportsSsl(), "A Qt TLS backend is required; tests must not silently skip TLS");
        qInfo() << "TLS backend:" << QSslSocket::activeBackend();
        QCoreApplication::setOrganizationName("WeChatLiteTransportTests");
        QCoreApplication::setApplicationName("OfflineTls");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, files.path());
        original = QSslConfiguration::defaultConfiguration();
        auto trusted = original;
        trusted.setCaCertificates({QSslCertificate(caPem)});
        QSslConfiguration::setDefaultConfiguration(trusted);
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QVERIFY(!QSslCertificate(trustedPem).isNull());
        QVERIFY(!QSslKey(trustedKey, QSsl::Rsa).isNull());
    }
    void cleanupTestCase() { QSslConfiguration::setDefaultConfiguration(original); }
    void httpsPin_data() { pinCases(); }
    void httpsPin()
    {
        QFETCH(bool, selfSigned);
        QFETCH(QString, pinMode);
        QFETCH(bool, succeeds);
        TlsFixture peer(selfSigned);
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        Transport transport;
        transport.configure(peer.url(), "offline-http-secret-at-least-24", fixturePin(peer, pinMode));
        bool completed = false;
        QString failure;
        transport.get("/api/status",
                      [&](const QJsonValue &, const QString &error)
                      {
                          failure = error;
                          completed = true;
                      }, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
        QCOMPARE(failure.isEmpty(), succeeds);
        if (succeeds)
        {
            QCOMPARE(peer.requests.size(), 1);
            QVERIFY(peer.requests.first().contains("Bearer offline-http-secret-at-least-24"));
        }
        else
            QVERIFY2(peer.requests.isEmpty(), "An unverified TLS peer received HTTP application bytes");
    }
    void webSocketPin_data() { pinCases(); }
    void webSocketPin()
    {
        QFETCH(bool, selfSigned);
        QFETCH(QString, pinMode);
        QFETCH(bool, succeeds);
        TlsFixture peer(selfSigned);
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        Transport transport;
        QSignalSpy warnings(&transport, &Transport::warning);
        QSignalSpy events(&transport, &Transport::event);
        transport.configure(peer.url(), "offline-ws-secret-at-least-24", fixturePin(peer, pinMode));
        transport.connectEvents();
        if (succeeds)
        {
            QTRY_VERIFY_WITH_TIMEOUT(!events.isEmpty(), 5000);
            QCOMPARE(peer.requests.size(), 1);
            QVERIFY(peer.requests.first().contains("Bearer offline-ws-secret-at-least-24"));
            QCOMPARE(events.first().first().toJsonObject()["ready"], QJsonValue(true));
        }
        else
        {
            QTRY_VERIFY_WITH_TIMEOUT(!warnings.isEmpty(), 5000);
            QVERIFY2(peer.requests.isEmpty(), "An unverified TLS peer received a WebSocket Upgrade/token");
        }
        transport.disconnectService();
    }
    void pinChangeDoesNotReusePreviouslyApprovedConnection()
    {
        TlsFixture peer;
        peer.keepAlive = true;
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        Transport transport;
        transport.configure(peer.url(), "offline-first-secret-at-least-24", peer.pin());
        bool done = false;
        QString failure;
        transport.get("/api/status", [&](const QJsonValue &, const QString &error)
                      { failure = error; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(failure.isEmpty());
        QCOMPARE(peer.requests.size(), 1);
        transport.configure(peer.url(), "offline-second-secret-at-least-24", QString(64, '0'));
        done = false;
        transport.get("/api/status", [&](const QJsonValue &, const QString &error)
                      { failure = error; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!failure.isEmpty());
        QCOMPARE(peer.requests.size(), 1);
        transport.disconnectService();
    }
    void redirectsDoNotForwardCredentials()
    {
        TlsFixture redirector, target;
        QVERIFY(redirector.listen(QHostAddress::LocalHost));
        QVERIFY(target.listen(QHostAddress::LocalHost));
        redirector.redirect = target.url() + "/stolen";
        Transport transport;
        transport.configure(redirector.url(), "offline-redirect-secret-at-least-24", redirector.pin());
        bool done = false;
        QString failure;
        transport.get("/api/status", [&](const QJsonValue &, const QString &error)
                      { failure = error; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!failure.isEmpty());
        QSignalSpy warnings(&transport, &Transport::warning);
        transport.connectEvents();
        QTRY_VERIFY_WITH_TIMEOUT(!warnings.isEmpty(), 5000);
        transport.disconnectService();
        QCOMPARE(redirector.requests.size(), 2);
        QCOMPARE(target.connections, 0);
        QVERIFY(target.requests.isEmpty());
    }
    void explicitPinNeverUsesPlaintext()
    {
        QTcpServer peer;
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        Transport transport;
        transport.configure(QString("http://127.0.0.1:%1").arg(peer.serverPort()),
                            "offline-plaintext-secret-at-least-24", QString(64, '0'));
        bool done = false;
        QString failure;
        transport.get("/api/status", [&](const QJsonValue &, const QString &error)
                      { failure = error; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!failure.isEmpty());
        QSignalSpy warnings(&transport, &Transport::warning);
        transport.connectEvents();
        QCOMPARE(warnings.size(), 1);
        QVERIFY(!peer.hasPendingConnections());
    }
    void ownedSelfSignedServerStopsWithItsOwnCredentials()
    {
        TlsFixture owned(true), other;
        QVERIFY(owned.listen(QHostAddress::LocalHost));
        QVERIFY(other.listen(QHostAddress::LocalHost));
        auto stopPath = files.filePath("owned-child-stop");
        owned.onRequest = [stopPath](const QByteArray &request)
        {
            if (request.startsWith("POST /api/shutdown ") &&
                request.contains("Bearer offline-owned-secret-at-least-24"))
            {
                QFile file(stopPath);
                if (file.open(QIODevice::WriteOnly))
                    file.write("stop");
            }
        };
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(), {"--offline-owned-child", stopPath});
        QVERIFY(process.waitForStarted(3000));
        Transport transport;
        transport.setOwnedServer(owned.url(), "offline-owned-secret-at-least-24", owned.pin());
        transport.configure(other.url(), "offline-other-secret-at-least-24", other.pin());
        transport.shutdownLocalServer(&process);
        QCOMPARE(process.state(), QProcess::NotRunning);
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(QFile::exists(stopPath));
        QCOMPARE(owned.requests.size(), 1);
        QVERIFY(other.requests.isEmpty());
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--offline-owned-child")
    {
        const auto stopPath = app.arguments()[2];
        QTimer check;
        QObject::connect(&check, &QTimer::timeout, &app,
                         [&]
                         {
                             if (QFile::exists(stopPath))
                                 app.quit();
                         });
        check.start(10);
        QTimer::singleShot(10000, &app, [&] { app.exit(7); });
        return app.exec();
    }
    const auto backend = qEnvironmentVariable("WECHAT_LITE_TEST_TLS_BACKEND");
    if (!backend.isEmpty() && !QSslSocket::setActiveBackend(backend))
    {
        qCritical() << "The requested Qt TLS backend is unavailable:" << backend;
        return 8;
    }
    TransportTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "transport_tests.moc"
