#include "connection_info.h"
#include "test_tls_identity.h"
#include "tunnel_session.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QUrl>
#include <QtTest>

namespace
{
QJsonObject settings(const QString &token = "fixture-good")
{
    return {{"server_id", QString(64, 'a')}, {"relay_url", "https://relay.invalid"},
        {"relay_token", token}, {"relay_ca", QString::fromUtf8(TestTlsIdentity::caPem)},
        {"relay_quic_port", 443}};
}
QJsonObject credential()
{
    return {{"version", 1}, {"mode", "p2p"}, {"serverUrl", "p2p://" + QString(64, 'a')},
        {"token", QString(32, 'c')}, {"certificatePin", QString(64, 'b')}, {"p2p", settings()}};
}
QByteArray bytes(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QString fixtureBinary()
{
    return QCoreApplication::applicationDirPath() + "/link-test-fixture"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ;
}
} // namespace

class ConnectionTests : public QObject
{
    Q_OBJECT
    QTemporaryDir temporary;
  private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("WeChatLiteTests");
        QCoreApplication::setApplicationName("Connection-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    void acceptsStableP2pAndLegacyDirectCredentials()
    {
        QJsonObject parsed;
        QString error;
        QVERIFY2(ConnectionInfo::parse(bytes(credential()), parsed, error), qPrintable(error));
        QCOMPARE(parsed.value("serverUrl"), credential().value("serverUrl"));
        QCOMPARE(parsed.value("p2p"), credential().value("p2p"));
        const QJsonObject legacy{{"serverUrl", "https://host.invalid:8765"}, {"token", QString(32, 'd')}};
        QVERIFY(ConnectionInfo::parse(bytes(legacy), parsed, error));
        QCOMPARE(parsed.value("mode").toString(), QString("direct"));
        QVERIFY(!parsed.contains("p2p"));
    }
    void malformedP2pIsAtomicAndBounded_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("wrong-device") << QString("serverUrl") << QJsonValue("p2p://different");
        QTest::newRow("missing-pin") << QString("certificatePin") << QJsonValue("");
        QTest::newRow("future-schema") << QString("version") << QJsonValue(2);
        QTest::newRow("fractional-schema") << QString("version") << QJsonValue(1.5);
        QTest::newRow("non-string-mode") << QString("mode") << QJsonValue(1);
        QTest::newRow("missing-device") << QString("server_id") << QJsonValue("");
        QTest::newRow("device-url-injection") << QString("server_id") << QJsonValue("abc@host");
        QTest::newRow("public-http") << QString("relay_url") << QJsonValue("http://192.0.2.1");
        QTest::newRow("userinfo") << QString("relay_url") << QJsonValue("https://user@relay.invalid");
        QTest::newRow("empty-userinfo") << QString("relay_url") << QJsonValue("https://@relay.invalid");
        QTest::newRow("query") << QString("relay_url") << QJsonValue("https://relay.invalid?token=bad");
        QTest::newRow("empty-query") << QString("relay_url") << QJsonValue("https://relay.invalid?");
        QTest::newRow("empty-fragment") << QString("relay_url") << QJsonValue("https://relay.invalid#");
        QTest::newRow("path") << QString("relay_url") << QJsonValue("https://relay.invalid/other");
        QTest::newRow("port-zero") << QString("relay_url") << QJsonValue("https://relay.invalid:0");
        QTest::newRow("wrong-scheme") << QString("relay_url") << QJsonValue("wss://relay.invalid");
        QTest::newRow("invalid-ca") << QString("relay_ca") << QJsonValue("invalid PEM");
        QTest::newRow("oversized-ca") << QString("relay_ca") << QJsonValue(QString(16 * 1024 + 1, 'x'));
        QTest::newRow("port-overflow") << QString("relay_quic_port") << QJsonValue(65536);
        QTest::newRow("fractional-port") << QString("relay_quic_port") << QJsonValue(443.5);
        QTest::newRow("token-control") << QString("relay_token") << QJsonValue("secret\nheader");
    }
    void malformedP2pIsAtomicAndBounded()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        auto input = credential();
        if (input.contains(field))
            input.insert(field, value);
        else
        {
            auto p2p = input.value("p2p").toObject();
            p2p.insert(field, value);
            input.insert("p2p", p2p);
        }
        QJsonObject result{{"untouched", true}};
        QString error;
        QVERIFY(!ConnectionInfo::parse(bytes(input), result, error));
        QCOMPARE(result, QJsonObject({{"untouched", true}}));
        QVERIFY(!error.isEmpty());
    }
    void entryImportStripsPrivateAndUnrelatedFields()
    {
        auto input = settings();
        input.insert("admin_token", "must-not-import");
        input.insert("key_file", "/arbitrary/path");
        QJsonObject result;
        QString error;
        QVERIFY(ConnectionInfo::publicEntry(bytes(QJsonObject{{"p2p", input}}), result, error));
        QCOMPARE(result.size(), 4);
        QVERIFY(!result.contains("server_id"));
        QVERIFY(!result.contains("admin_token"));
        QVERIFY(!result.contains("key_file"));
    }
    void processKeepsInputOpenAndOnlyReceivesNetworkConfiguration()
    {
        TunnelSession session(nullptr, fixtureBinary());
        QSignalSpy ready(&session, &TunnelSession::ready), path(&session, &TunnelSession::pathChanged),
            failed(&session, &TunnelSession::failed);
        auto input = settings();
        input.insert("token", "app-secret-must-not-reach-helper");
        input.insert("key_file", "/untrusted/override");
        session.start(input);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QTRY_COMPARE(path.size(), 1);
        const QUrl local(ready.first().first().toString());
        QCOMPARE(local.scheme(), QString("https"));
        QCOMPARE(local.host(), QString("127.0.0.1"));
        QVERIFY(local.port() > 0);
        auto *child = session.findChild<QProcess *>();
        QVERIFY(child);
        QCOMPARE(child->arguments(), QStringList({"--client"}));
        QCOMPARE(child->state(), QProcess::Running);
        QFile capture(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/p2p-client.key.fixture.json");
        QVERIFY(capture.open(QIODevice::ReadOnly));
        const auto sent = QJsonDocument::fromJson(capture.readAll()).object();
        QCOMPARE(sent.size(), 6);
        QVERIFY(!sent.contains("token"));
        QVERIFY(sent.value("key_file").toString().endsWith("/p2p-client.key"));
        QVERIFY(sent.value("key_file") != "/untrusted/override");
        QTest::qWait(40);
        QVERIFY(failed.isEmpty());
        session.stop();
        QCOMPARE(child->state(), QProcess::NotRunning);
        QVERIFY(failed.isEmpty());
    }
    void unsafeHelperOutputFailsClosed_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"unsafe", "malformed", "oversized", "error-secret"})
            QTest::newRow(mode) << QString(mode);
    }
    void unsafeHelperOutputFailsClosed()
    {
        QFETCH(QString, mode);
        TunnelSession session(nullptr, fixtureBinary());
        QSignalSpy ready(&session, &TunnelSession::ready), failed(&session, &TunnelSession::failed);
        session.start(settings(mode));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
        QVERIFY(ready.isEmpty());
        QVERIFY(!failed.first().first().toString().contains("error-secret"));
        QVERIFY(failed.first().first().toString().size() <= 500);
    }
    void replacementIgnoresOldProcessAndStopNeverStartsAnything()
    {
        TunnelSession session(nullptr, fixtureBinary());
        QSignalSpy ready(&session, &TunnelSession::ready), failed(&session, &TunnelSession::failed);
        session.start(settings("delayed"));
        auto *first = session.findChild<QProcess *>();
        QVERIFY(first);
        QTRY_COMPARE_WITH_TIMEOUT(first->state(), QProcess::Running, 5000);
        session.start(settings());
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QTest::qWait(450);
        QCOMPARE(ready.size(), 1);
        QVERIFY(failed.isEmpty());
        session.stop();
        QTest::qWait(30);
        QCOMPARE(ready.size(), 1);
        QVERIFY(failed.isEmpty());
    }
    void missingHelperDoesNotExecuteFallback()
    {
        TunnelSession session(nullptr, temporary.filePath("missing-executable"));
        QSignalSpy failed(&session, &TunnelSession::failed);
        session.start(settings());
        QCOMPARE(failed.size(), 1);
        QVERIFY(session.findChildren<QProcess *>().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ConnectionTests)
#include "connection_tests.moc"
