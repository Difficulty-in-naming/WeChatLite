#include "server_window.h"
#include "client_permissions.h"
#include "server_settings.h"
#include "connection_info.h"
#include "test_tls_identity.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTabWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <memory>

namespace
{
QJsonObject relayEntry()
{
    return {{"relay_url", "https://offline-relay.invalid"}, {"relay_token", "offline-relay-only-token"},
        {"relay_ca", QString::fromUtf8(TestTlsIdentity::caPem)}, {"relay_quic_port", 443}};
}

class AdminFixture final : public QTcpServer
{
  public:
    bool available = true;
    int created = 0;
    QString secret = QString(64, 'a');
    QString clientToken = "wxcl_" + QString(64, 'c');
  protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QSslSocket(this);
        socket->setSocketDescriptor(descriptor);
        socket->setLocalCertificate(QSslCertificate(TestTlsIdentity::selfSignedPem));
        socket->setPrivateKey(QSslKey(TestTlsIdentity::selfSignedKey, QSsl::Rsa));
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::readyRead, this, [this, socket, buffer]
        {
            buffer->append(socket->readAll());
            const auto end = buffer->indexOf("\r\n\r\n");
            if (end < 0 || socket->property("handled").toBool())
                return;
            const auto lines = buffer->left(end).split('\n');
            int length = 0;
            for (const auto &line : lines)
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
            if (buffer->size() < end + 4 + length)
                return;
            socket->setProperty("handled", true);
            const auto request = lines.first().trimmed().split(' ');
            const auto path = request.value(1);
            QJsonObject response;
            if (path == "/api/local/forwarding")
                response = {{"groups", QJsonArray{QJsonObject{{"id", "offline-group"}, {"name", "离线群"}}}},
                    {"selected", QJsonArray{"offline-group"}}};
            else if (path == "/api/local/p2p")
                response = {{"enabled", available}, {"server_id", available ? QString(64, 'b') : QString()},
                    {"status", available ? "listening" : "failed"}};
            else if (path == "/api/local/clients" && request.value(0) == "POST")
            {
                ++created;
                response = {{"client", QJsonObject{{"id", "offline-client"}, {"name", "测试客户端"}}},
                    {"token", clientToken}};
            }
            else
                response = {{"clients", QJsonArray{}}};
            const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
        socket->startServerEncryption();
    }
};
} // namespace

class ServerGuiTests : public QObject
{
    Q_OBJECT
  private slots:
    void separateGuiOwnsServiceAndManagesIndependentClientKey()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        const auto administrator = QUuid::createUuid().toString() + QUuid::createUuid().toString();
        QJsonObject config{{"listen", "127.0.0.1:" + QString::number(port)}, {"token", administrator},
            {"data", temporary.path() + "/data"}};
        const auto path = temporary.path() + "/server.json";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(config).toJson());
        file.close();
        ServerWindow window(path, true);
        window.show();
        QTest::qWait(50);
        auto *tabs = window.findChild<QTabWidget *>("serverTabs");
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        QTest::mouseClick(window.findChild<QPushButton *>("serverStart"), Qt::LeftButton);
        QVERIFY2(window.findChild<QLabel *>("serverBridgeStatus")->text() != "尚未启动",
            qPrintable(window.findChild<QLabel *>("serverFeedback")->text()));
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel *>("serverBridgeStatus")->text(), QString("已连接"), 15000);
        QVERIFY(!window.findChild<QPushButton *>("serverConfigure")->isEnabled());
        tabs->setCurrentIndex(2);
        auto *groups = window.findChild<QListWidget *>("clientAccessGroups");
        QTRY_VERIFY_WITH_TIMEOUT(groups->count() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton *>("clientAccessNew")->isEnabled(), 5000);
        QTest::mouseClick(window.findChild<QPushButton *>("clientAccessNew"), Qt::LeftButton);
        window.findChild<QLineEdit *>("clientAccessName")->setText("另一台电脑");
        window.findChild<QCheckBox *>("clientAccessCanSend")->setChecked(true);
        groups->item(0)->setCheckState(Qt::Checked);
        const auto allowedGroup = groups->item(0)->data(Qt::UserRole).toString();
        QJsonObject connection;
        QTimer credentialTimer;
        credentialTimer.setInterval(25);
        connect(&credentialTimer, &QTimer::timeout, &window, [&]
        {
            auto *dialog = window.findChild<QDialog *>("clientCredentialDialog");
            if (!dialog)
                return;
            QTest::mouseClick(dialog->findChild<QPushButton *>("clientCredentialCopy"), Qt::LeftButton);
            connection = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
            dialog->accept();
            credentialTimer.stop();
        });
        credentialTimer.start();
        QTest::mouseClick(window.findChild<QPushButton *>("clientAccessApply"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!connection.value("token").toString().isEmpty(), 5000);
        QVERIFY(connection.value("token").toString() != administrator);
        QCOMPARE(connection.value("serverUrl").toString(), "http://127.0.0.1:" + QString::number(port));
        auto *list = window.findChild<QListWidget *>("clientAccessList");
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 5000);
        Transport client;
        client.configure(connection.value("serverUrl").toString(), connection.value("token").toString());
        QJsonValue value;
        QString error;
        bool done = false;
        client.get("/api/groups", [&](const QJsonValue &v, const QString &e) { value = v; error = e; done = true; });
        QTRY_VERIFY(done);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(value.toArray().size(), 1);
        QCOMPARE(value.toArray().first().toObject().value("id").toString(), allowedGroup);
        done = false;
        client.get("/api/local/clients", [&](const QJsonValue &, const QString &e) { error = e; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(!error.isEmpty());
        window.findChild<QCheckBox *>("clientAccessCanSend")->setChecked(false);
        QTest::mouseClick(window.findChild<QPushButton *>("clientAccessApply"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton *>("clientAccessApply")->isEnabled(), 5000);
        done = false;
        client.get("/api/status", [&](const QJsonValue &v, const QString &e) { value = v; error = e; done = true; });
        QTRY_VERIFY(done);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!value.toObject().value("permissions").toObject().value("can_send").toBool(true));
        QVERIFY(!value.toObject().value("permissions").toObject().value("can_download").toBool(true));
        QTest::mouseClick(window.findChild<QPushButton *>("serverStop"), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel *>("serverState")->text(), QString("已停止"), 7000);
        QVERIFY(window.findChild<QPushButton *>("serverConfigure")->isEnabled());
        window.startService();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel *>("serverBridgeStatus")->text(), QString("已连接"), 15000);
        done = false;
        client.get("/api/groups", [&](const QJsonValue &v, const QString &e) { value = v; error = e; done = true; });
        QTRY_VERIFY(done);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(value.toArray().size(), 1);
        window.stopService();
    }

    void localAdminRejectsSpecificLanBindAndMissingCertificate()
    {
        QTemporaryDir temporary;
        const auto path = temporary.path() + "/server.json";
        auto write = [&](const QJsonObject &config)
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return false;
            return file.write(QJsonDocument(config).toJson()) > 0;
        };
        QJsonObject config{{"listen", "192.0.2.12:8765"}, {"token", QString(64, 'a')}};
        QVERIFY(write(config));
        Transport transport;
        QString error;
        QVERIFY(!configureLocalAdmin(transport, path, error));
        config["listen"] = "0.0.0.0:8765";
        QVERIFY(write(config));
        QVERIFY(configureLocalAdmin(transport, path, error));
        QCOMPARE(transport.baseUrl().host(), QString("127.0.0.1"));
        config["tls_cert"] = "missing.pem";
        QVERIFY(write(config));
        QVERIFY(!configureLocalAdmin(transport, path, error));
    }
    void automaticSettingsPasteIsAtomicAndRequiresTlsBeforeSaving()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("server.json");
        const QJsonObject original{{"listen", "127.0.0.1:8765"}, {"token", QString(64, 'a')},
            {"data", directory.filePath("data")}, {"keep", "unchanged"}};
        QFile config(path);
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QJsonDocument(original).toJson());
        config.close();
        QFile certificate(directory.filePath("cert.pem")), key(directory.filePath("key.pem"));
        QVERIFY(certificate.open(QIODevice::WriteOnly));
        certificate.write(TestTlsIdentity::selfSignedPem);
        certificate.close();
        QVERIFY(key.open(QIODevice::WriteOnly));
        key.write(TestTlsIdentity::selfSignedKey);
        key.close();
        QWidget parent;
        bool pasted = false, rejected = false, requiresTls = false, timedOut = false;
        QString modalFailure;
        QTimer::singleShot(10000, &parent, [&]
        {
            timedOut = true;
            auto *dialog = parent.findChild<QDialog *>("serviceConnectionSettings");
            if (!dialog)
                return;
            if (auto *message = dialog->findChild<QMessageBox *>())
            {
                modalFailure = message->text();
                message->reject();
            }
            dialog->reject();
        });
        QTimer::singleShot(0, &parent, [&]
        {
            auto *dialog = parent.findChild<QDialog *>("serviceConnectionSettings");
            if (!dialog)
                return;
            dialog->findChild<QTabWidget *>("serviceConnectionTabs")->setCurrentIndex(1);
            auto *paste = dialog->findChild<QPushButton *>("servicePasteP2pEntry");
            auto *url = dialog->findChild<QLineEdit *>("serviceRelayUrl");
            auto *enabled = dialog->findChild<QCheckBox *>("serviceP2pEnabled");
            auto *save = dialog->findChild<QPushButton *>("serviceConnectionSave");
            QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(relayEntry()).toJson()));
            QTest::mouseClick(paste, Qt::LeftButton);
            pasted = url->text() == relayEntry().value("relay_url").toString();
            auto invalid = relayEntry();
            invalid["relay_url"] = "http://192.0.2.1";
            QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(invalid).toJson()));
            QTest::mouseClick(paste, Qt::LeftButton);
            rejected = url->text() == relayEntry().value("relay_url").toString();
            enabled->setChecked(true);
            QTest::mouseClick(save, Qt::LeftButton);
            requiresTls = dialog->isVisible() &&
                dialog->findChild<QLabel *>("serviceP2pEntryStatus")->text().contains("TLS");
            QFile unchanged(path);
            if (unchanged.open(QIODevice::ReadOnly))
                requiresTls &= QJsonDocument::fromJson(unchanged.readAll()).object() == original;
            unchanged.close(); // Release the Windows read handle before QSaveFile replaces this file.
            dialog->findChild<QLineEdit *>("serviceCertificate")->setText(certificate.fileName());
            dialog->findChild<QLineEdit *>("servicePrivateKey")->setText(key.fileName());
            QTest::mouseClick(save, Qt::LeftButton);
        });
        const bool accepted = editLocalService(&parent, path);
        QVERIFY2(!timedOut, qPrintable("Settings fixture timed out: " + modalFailure));
        QVERIFY(accepted);
        QVERIFY(pasted && rejected && requiresTls);
        QVERIFY(config.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(config.readAll()).object();
        QCOMPARE(saved.value("keep"), original.value("keep"));
        QCOMPARE(saved.value("token"), original.value("token"));
        auto expected = relayEntry();
        expected["enabled"] = true;
        QCOMPARE(saved.value("p2p").toObject(), expected);
    }
    void automaticCredentialUsesStableIdAndNeverExportsAdministratorKey()
    {
        QTemporaryDir directory;
        AdminFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        QFile certificate(directory.filePath("cert.pem"));
        QVERIFY(certificate.open(QIODevice::WriteOnly));
        certificate.write(TestTlsIdentity::selfSignedPem);
        certificate.close();
        auto p2p = relayEntry();
        p2p["enabled"] = true;
        QFile config(directory.filePath("server.json"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QJsonDocument(QJsonObject{{"listen", "127.0.0.1:" + QString::number(fixture.serverPort())},
            {"token", fixture.secret}, {"tls_cert", certificate.fileName()}, {"p2p", p2p}}).toJson());
        config.close();
        ClientPermissions editor(config.fileName());
        QJsonObject connection;
        QString stage;
        bool timedOut = false;
        QTimer capture, deadline;
        capture.setInterval(20);
        deadline.setSingleShot(true);
        deadline.setInterval(10000);
        auto diagnostic = [&]
        {
            return QString("Credential fixture stage=%1; created=%2; status=%3")
                .arg(stage).arg(fixture.created)
                .arg(editor.findChild<QLabel *>("clientAccessStatus")->text());
        };
        auto enterStage = [&](const char *next)
        {
            stage = QString::fromLatin1(next);
            qInfo("Credential fixture stage: %s", next);
            deadline.start();
        };
        connect(&deadline, &QTimer::timeout, &editor, [&]
        {
            timedOut = true;
            capture.stop();
            qWarning().noquote() << diagnostic();
            // QDialog::exec can block the mouseClick call before its following QTRY starts.
            if (auto *dialog = editor.findChild<QDialog *>("clientCredentialDialog"))
                dialog->reject();
        });
        enterStage("refresh TLS administrator catalogue");
        editor.show();
        editor.refresh();
        auto *create = editor.findChild<QPushButton *>("clientAccessNew");
        QTRY_VERIFY2_WITH_TIMEOUT(create->isEnabled(), qPrintable(diagnostic()), 5000);
        editor.findChild<QLineEdit *>("clientAccessName")->setText("自动连接客户端");
        connect(&capture, &QTimer::timeout, &editor, [&]
        {
            auto *dialog = editor.findChild<QDialog *>("clientCredentialDialog");
            auto *copy = dialog ? dialog->findChild<QPushButton *>("clientCredentialCopy") : nullptr;
            if (!dialog || !dialog->isVisible() || !copy || !copy->isVisible() || !copy->isEnabled())
                return;
            // Stop before any platform clipboard work can process nested events. The production
            // clicked handler and real clipboard remain under test, without offscreen mouse hit testing.
            capture.stop();
            enterStage("copy visible credential dialog to clipboard");
            copy->click();
            enterStage("read credential clipboard JSON");
            connection = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
            dialog->accept();
        });
        enterStage("clear credential clipboard");
        QApplication::clipboard()->clear();
        enterStage("create independent client credential");
        capture.start();
        QTest::mouseClick(editor.findChild<QPushButton *>("clientAccessApply"), Qt::LeftButton);
        QTRY_VERIFY2_WITH_TIMEOUT(!connection.isEmpty() || timedOut, qPrintable(diagnostic()), 5000);
        QVERIFY2(!timedOut, qPrintable(diagnostic()));
        QCOMPARE(fixture.created, 1);
        QCOMPARE(connection.value("mode").toString(), QString("p2p"));
        QCOMPARE(connection.value("serverUrl").toString(), "p2p://" + QString(64, 'b'));
        QCOMPARE(connection.value("token").toString(), fixture.clientToken);
        QVERIFY(!QJsonDocument(connection).toJson().contains(fixture.secret.toUtf8()));
        QJsonObject parsed;
        QString error;
        QVERIFY2(ConnectionInfo::parse(QJsonDocument(connection).toJson(), parsed, error), qPrintable(error));
        enterStage("refresh administrator catalogue after creation");
        QTRY_VERIFY2_WITH_TIMEOUT(create->isEnabled(), qPrintable(diagnostic()), 5000);
        QTest::mouseClick(create, Qt::LeftButton);
        fixture.available = false;
        editor.findChild<QLineEdit *>("clientAccessName")->setText("未就绪不创建");
        enterStage("reject unavailable automatic connection without creating a key");
        QTest::mouseClick(editor.findChild<QPushButton *>("clientAccessApply"), Qt::LeftButton);
        QTRY_VERIFY2_WITH_TIMEOUT(editor.findChild<QLabel *>("clientAccessStatus")->text().contains("未创建"),
            qPrintable(diagnostic()), 5000);
        QVERIFY2(!timedOut, qPrintable(diagnostic()));
        QCOMPARE(fixture.created, 1);
        deadline.stop();
    }
};

QTEST_MAIN(ServerGuiTests)
#include "tests.moc"
