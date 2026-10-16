#include "transport.h"
#include "test_tls_identity.h"
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
using TestTlsIdentity::caPem;
using TestTlsIdentity::trustedPem;
using TestTlsIdentity::trustedKey;
using TestTlsIdentity::selfSignedPem;
using TestTlsIdentity::selfSignedKey;

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
        QVERIFY2(failure.isEmpty() == succeeds, qPrintable(failure));
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
