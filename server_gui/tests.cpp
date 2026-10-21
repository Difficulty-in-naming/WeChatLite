#include "server_window.h"
#include "autostart.h"
#include "client_permissions.h"
#include "server_settings.h"
#include "connection_info.h"
#include "test_tls_identity.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTabWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <QXmlStreamReader>
#include <QScopeGuard>
#include <memory>
#include <functional>
#include <algorithm>

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
    bool available = true, tokenKnown = true;
    int created = 0, updated = 0, rotated = 0, credentialReads = 0, p2pReads = 0, rejectedAuthorization = 0;
    QString secret = QString(64, 'a');
    QString clientToken = "wxcl_" + QString(64, 'c');
    QString serverId = QString(64, 'b');
    QJsonObject client, rotationBody;
    QJsonObject forwarding{{"groups", QJsonArray{QJsonObject{{"id", "offline-group"}, {"name", "离线群"}}}},
        {"selected", QJsonArray{"offline-group"}}, {"manual_selected", QJsonArray{"offline-group"}}, {"patterns", QJsonArray{}}};
    QJsonObject forwardingRequest;

    void seedClient()
    {
        client = {{"id", "offline-client"}, {"name", "已保存的客户端"}, {"enabled", true}, {"revoked", false},
            {"can_send", true}, {"can_download", false}, {"groups", QJsonArray{"offline-group"}}};
    }

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
            int length = 0, authorizationHeaders = 0;
            QByteArray authorization;
            for (const auto &line : lines)
            {
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                if (line.toLower().startsWith("authorization:"))
                {
                    ++authorizationHeaders;
                    authorization = line.mid(line.indexOf(':') + 1).trimmed();
                }
            }
            if (buffer->size() < end + 4 + length)
                return;
            socket->setProperty("handled", true);
            const auto request = lines.first().trimmed().split(' ');
            const auto path = request.value(1);
            const auto method = request.value(0);
            const auto payload = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
            QJsonObject response;
            QByteArray status = "200 OK";
            if (authorizationHeaders != 1 || authorization != "Bearer " + secret.toUtf8())
            {
                ++rejectedAuthorization;
                status = "403 Forbidden";
                response = {{"error", "fixture administrator authorization required"}};
            }
            else if (path == "/api/local/forwarding" && method == "GET")
                response = forwarding;
            else if (path == "/api/local/forwarding" && method == "POST")
            {
                forwardingRequest = payload;
                forwarding["manual_selected"] = payload["selected"];
                forwarding["selected"] = payload["selected"];
                forwarding["patterns"] = payload["patterns"];
                response = forwarding;
            }
            else if (path == "/api/local/p2p" && method == "GET")
            {
                ++p2pReads;
                response = {{"enabled", available}, {"server_id", available ? serverId : QString()},
                    {"status", available ? "listening" : "failed"}};
            }
            else if (path == "/api/local/clients" && method == "GET")
                response = {{"clients", client.isEmpty() ? QJsonArray{} : QJsonArray{client}}};
            else if (path == "/api/local/clients" && method == "POST")
            {
                ++created;
                client = payload;
                client.insert("id", "offline-client");
                client.insert("revoked", false);
                response = {{"client", client}, {"token", clientToken}};
            }
            else if (!client.isEmpty() && path == "/api/local/clients/offline-client/credential" && method == "GET")
            {
                ++credentialReads;
                response = {{"client", client},
                    {"token", tokenKnown ? QJsonValue(clientToken) : QJsonValue(QJsonValue::Null)}};
            }
            else if (!client.isEmpty() && path == "/api/local/clients/offline-client/rotate" && method == "POST")
            {
                ++rotated;
                rotationBody = payload;
                tokenKnown = true;
                clientToken = "wxcl_" + QString(64, 'd');
                response = {{"client", client}, {"token", clientToken}};
            }
            else if (!client.isEmpty() && path == "/api/local/clients/offline-client" && method == "POST")
            {
                ++updated;
                for (auto entry = payload.begin(); entry != payload.end(); ++entry)
                    client.insert(entry.key(), entry.value());
                response = {{"client", client}};
            }
            else
            {
                status = "404 Not Found";
                response = {{"error", "unexpected fixture route"}};
            }
            const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 " + status + "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
        socket->startServerEncryption();
    }
};

bool writeFixtureConfiguration(const QString &directory, const AdminFixture &fixture, QJsonObject p2p = relayEntry())
{
    QFile certificate(directory + "/cert.pem");
    if (!certificate.open(QIODevice::WriteOnly) || certificate.write(TestTlsIdentity::selfSignedPem) <= 0)
        return false;
    certificate.close();
    QFile key(directory + "/key.pem");
    if (!key.open(QIODevice::WriteOnly) || key.write(TestTlsIdentity::selfSignedKey) <= 0)
        return false;
    key.close();
    p2p.insert("enabled", true);
    QFile config(directory + "/server.json");
    return config.open(QIODevice::WriteOnly) && config.write(QJsonDocument(QJsonObject{
        {"listen", "127.0.0.1:" + QString::number(fixture.serverPort())}, {"token", fixture.secret},
        {"data", directory + "/data"}, {"tls_cert", certificate.fileName()}, {"tls_key", key.fileName()},
        {"keep", "fixture setting preserved"}, {"p2p", p2p}}).toJson()) > 0;
}

// Modal handlers must always close their dialog, including failure paths: otherwise
// QDialog::exec can prevent the assertion following the initiating click from running.
class CredentialInteraction final : public QObject
{
  public:
    enum class Action { Copy, Cancel, EditRelay, Forbid };
    bool complete = false, timedOut = false, confirmed = false, defaultNo = false;
    bool hiddenByDefault = false, revealWorks = false;
    QString error, title, addressText, addressLabel;
    bool addressReadOnly = false;
    QJsonObject connection;
    std::function<QString(QDialog &)> inspect;

    explicit CredentialInteraction(QWidget &parent) : owner(parent)
    {
        poll.setInterval(20);
        deadline.setSingleShot(true);
        deadline.setInterval(10000);
        connect(&deadline, &QTimer::timeout, this, [this]
        {
            timedOut = true;
            error = "Credential fixture timed out";
            stop();
            if (auto *question = owner.findChild<QMessageBox *>())
                question->reject();
            if (auto *dialog = owner.findChild<QDialog *>("clientCredentialDialog"))
                dialog->reject();
        });
        connect(&poll, &QTimer::timeout, this, [this]
        {
            if (auto *question = owner.findChild<QMessageBox *>(); question && question->isVisible())
            {
                const auto answer = confirmation;
                confirmation = QMessageBox::NoButton;
                auto *button = question->button(answer);
                if (!button)
                {
                    error = "Unexpected confirmation dialog";
                    stop();
                    question->reject();
                    complete = true;
                    return;
                }
                defaultNo = question->defaultButton() == question->button(QMessageBox::No);
                confirmed = true;
                if (answer == QMessageBox::No)
                {
                    complete = true;
                    stop();
                }
                button->click();
                return;
            }
            auto *dialog = owner.findChild<QDialog *>("clientCredentialDialog");
            if (!dialog || !dialog->isVisible())
                return;
            poll.stop(); // Clipboard callbacks may dispatch nested platform events; keep the deadline armed.
            title = dialog->windowTitle();
            auto *copy = dialog->findChild<QPushButton *>("clientCredentialCopy");
            auto *secret = dialog->findChild<QLineEdit *>("clientCredentialToken");
            auto *reveal = dialog->findChild<QCheckBox *>("clientCredentialReveal");
            auto *urls = dialog->findChild<QComboBox *>("clientCredentialUrl");
            auto *label = dialog->findChild<QLabel *>("clientCredentialAddressLabel");
            if (action == Action::Forbid)
                error = "An operation unexpectedly displayed a credential";
            else if (!copy || !secret || !reveal || !urls || !urls->lineEdit() || !label || !copy->isEnabled())
                error = "Credential controls are missing or disabled";
            else
            {
                addressText = urls->currentText();
                addressLabel = label->text();
                addressReadOnly = urls->lineEdit()->isReadOnly();
                if (inspect)
                    error = inspect(*dialog);
                hiddenByDefault = !reveal->isChecked() && secret->isReadOnly() &&
                    secret->echoMode() == QLineEdit::Password;
                reveal->click();
                const bool visible = secret->echoMode() == QLineEdit::Normal;
                reveal->click();
                revealWorks = visible && secret->echoMode() == QLineEdit::Password;
                if (action == Action::Copy && error.isEmpty())
                {
                    copy->click();
                    connection = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
                }
                if (action == Action::EditRelay && error.isEmpty())
                {
                    auto *edit = dialog->findChild<QPushButton *>("clientCredentialEditRelay");
                    if (!edit || !edit->isVisible() || !edit->isEnabled())
                        error = "Relay settings entry is missing or disabled";
                    else
                        edit->click();
                }
            }
            stop();
            complete = true;
            if (action == Action::Cancel || !error.isEmpty())
                dialog->reject();
            else if (action != Action::EditRelay)
                dialog->accept();
        });
    }

    void arm(Action nextAction, QMessageBox::StandardButton answer = QMessageBox::NoButton)
    {
        stop();
        action = nextAction;
        confirmation = answer;
        complete = timedOut = confirmed = defaultNo = hiddenByDefault = revealWorks = false;
        error.clear();
        title.clear();
        addressText.clear();
        addressLabel.clear();
        addressReadOnly = false;
        inspect = {};
        connection = {};
        poll.start();
        deadline.start();
    }

    void stop()
    {
        poll.stop();
        deadline.stop();
    }

  private:
    QWidget &owner;
    QTimer poll, deadline;
    Action action = Action::Forbid;
    QMessageBox::StandardButton confirmation = QMessageBox::NoButton;
};
} // namespace

class ServerGuiTests : public QObject
{
    Q_OBJECT
  private slots:
    void groupRulesRoundTripWithoutPromotingAutomaticMatchesIntoManualSelections()
    {
        QTemporaryDir directory;
        AdminFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        QVERIFY(writeFixtureConfiguration(directory.path(), fixture));
        fixture.forwarding["groups"] = QJsonArray{
            QJsonObject{{"id", "offline-group"}, {"name", "手动群"}},
            QJsonObject{{"id", "auto-group"}, {"name", "BB-PLY002 - jx"}}};
        fixture.forwarding["selected"] = QJsonArray{"offline-group", "auto-group"};
        fixture.forwarding["patterns"] = QJsonArray{"*-PLY* - jx"};
        ForwardingSettings settings(directory.filePath("server.json"));
        settings.show();
        auto *rules = settings.findChild<QPlainTextEdit *>("forwardingPatterns");
        auto *groups = settings.findChild<QListWidget *>("forwardingGroups");
        QTRY_COMPARE(groups->count(), 2);
        QCOMPARE(rules->toPlainText(), QString("*-PLY* - jx"));
        QCOMPARE(groups->item(0)->checkState(), Qt::Checked);
        QCOMPARE(groups->item(1)->checkState(), Qt::Unchecked);
        QVERIFY(groups->item(1)->text().contains("规则自动转发"));
        rules->setPlainText("  *-PLY* - jx  \n\n项目?群\n");
        settings.findChild<QPushButton *>("forwardingApply")->click();
        QTRY_COMPARE(fixture.forwardingRequest["patterns"].toArray(), (QJsonArray{"*-PLY* - jx", "项目?群"}));
        QCOMPARE(fixture.forwardingRequest["selected"].toArray(), QJsonArray{"offline-group"});
        QTRY_COMPARE(rules->toPlainText(), QString("*-PLY* - jx\n项目?群"));

        fixture.seedClient();
        fixture.client["group_patterns"] = QJsonArray{"*-PLY* - jx"};
        ClientPermissions permissions(directory.filePath("server.json"));
        permissions.show();
        permissions.refresh();
        auto *clientRules = permissions.findChild<QPlainTextEdit *>("clientAccessPatterns");
        QTRY_COMPARE(clientRules->toPlainText(), QString("*-PLY* - jx"));
        clientRules->setPlainText("*-PLY* - jx\n项目?群");
        permissions.findChild<QPushButton *>("clientAccessApply")->click();
        QTRY_COMPARE(fixture.updated, 1);
        QCOMPARE(fixture.client["group_patterns"].toArray(), (QJsonArray{"*-PLY* - jx", "项目?群"}));
        QTRY_VERIFY(permissions.findChild<QPushButton *>("clientAccessNew")->isEnabled());
        permissions.findChild<QPushButton *>("clientAccessNew")->click();
        QVERIFY(clientRules->toPlainText().isEmpty());
        QCOMPARE(fixture.rejectedAuthorization, 0);
    }

    void autostartRegistrationIsScopedReversibleAndTracksCurrentProgram_data()
    {
        QTest::addColumn<bool>("windows");
#ifdef Q_OS_WIN
        QTest::newRow("temporary-registry-value") << true;
#endif
        QTest::newRow("temporary-launch-agent") << false;
    }

    void autostartRegistrationIsScopedReversibleAndTracksCurrentProgram()
    {
        QFETCH(bool, windows);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto registry = "HKEY_CURRENT_USER\\Software\\WeChatLite\\AutostartTests\\" +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto cleanup = qScopeGuard([&]
        {
            if (windows) { QSettings settings(registry, QSettings::NativeFormat); settings.remove(""); settings.sync(); }
        });
        const auto location = windows ? registry : temporary.filePath("LaunchAgents/local.wechatlite.server.plist");
        const auto platform = windows ? ServerAutostart::Platform::Windows : ServerAutostart::Platform::MacOS;
        const auto name = windows ? "wechat-server.exe" : "wechat-server";
        const auto oldProgram = temporary.filePath("旧版本 & app/" + QString(name));
        const auto newProgram = temporary.filePath("新版本 & app/" + QString(name));
        for (const auto &path : {oldProgram, newProgram})
        {
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("fixture executable") > 0);
            file.close();
        }
        const auto configPath = temporary.filePath("配置 & server.json");
        QFile config(configPath);
        QVERIFY(config.open(QIODevice::WriteOnly));
        QVERIFY(config.write("{\"token\":\"fixture-sensitive-value\"}") > 0);
        config.close();
        const auto neighbor = temporary.filePath("LaunchAgents/other.application.plist");
        if (windows)
        {
            QSettings settings(registry, QSettings::NativeFormat);
            settings.setValue("OtherApplication", "neighbor must survive");
            settings.sync();
        }
        else
        {
            QVERIFY(QDir().mkpath(QFileInfo(neighbor).absolutePath()));
            QFile file(neighbor);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("neighbor must survive") > 0);
        }
        auto command = [&]() -> QStringList
        {
            if (windows)
            {
                QSettings settings(registry, QSettings::NativeFormat);
                return QProcess::splitCommand(settings.value("WeChatLiteServer").toString());
            }
            QFile file(location);
            if (!file.open(QIODevice::ReadOnly)) return {};
            const auto content = file.readAll();
            if (content.contains("fixture-sensitive-value")) return {};
            QXmlStreamReader xml(content);
            QStringList result;
            while (!xml.atEnd())
            {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == u"array")
                    while (xml.readNextStartElement()) result << xml.readElementText();
            }
            return xml.hasError() ? QStringList{} : result;
        };
        auto neighborIntact = [&]
        {
            if (windows)
            {
                QSettings settings(registry, QSettings::NativeFormat);
                return settings.value("OtherApplication").toString() == "neighbor must survive";
            }
            QFile file(neighbor);
            return file.open(QIODevice::ReadOnly) && file.readAll() == "neighbor must survive";
        };
        ServerAutostart old(platform, location, oldProgram, configPath);
        QString error;
        bool enabled = true;
        QVERIFY2(old.read(enabled, error), qPrintable(error));
        QVERIFY(!enabled);
        QVERIFY2(old.refreshIfEnabled(error), qPrintable(error));
        QVERIFY2(old.read(enabled, error), qPrintable(error));
        QVERIFY(!enabled); // Merely starting a newer app never enables the option.
        QVERIFY2(old.setEnabled(true, error), qPrintable(error));
        QVERIFY2(old.read(enabled, error), qPrintable(error));
        QVERIFY(enabled);
        QCOMPARE(QDir::fromNativeSeparators(command().value(0)), oldProgram);
        QCOMPARE(command().value(1), "--config");
        QCOMPARE(QDir::fromNativeSeparators(command().value(2)), configPath);
        QCOMPARE(command().value(3), "--start");
        ServerAutostart current(platform, location, newProgram, configPath);
        QVERIFY2(current.refreshIfEnabled(error), qPrintable(error));
        QCOMPARE(QDir::fromNativeSeparators(command().value(0)), newProgram);
        QVERIFY2(current.read(enabled, error), qPrintable(error));
        QVERIFY(enabled && neighborIntact());
        AutostartSettings widget(current);
        widget.show();
        auto *checkbox = widget.findChild<QCheckBox *>("serverAutostartEnabled");
        QVERIFY(checkbox && checkbox->isChecked());
        QVERIFY(checkbox->toolTip().isEmpty());
        checkbox->click();
        QVERIFY(!checkbox->isChecked());
        QVERIFY2(current.read(enabled, error), qPrintable(error));
        QVERIFY(!enabled && neighborIntact());
        checkbox->click();
        QVERIFY(checkbox->isChecked());
        // Reopening reads the OS registration, rather than a stale application preference.
        QVERIFY2(current.setEnabled(false, error), qPrintable(error));
        AutostartSettings reopened(current);
        auto *reopenedCheckbox = reopened.findChild<QCheckBox *>("serverAutostartEnabled");
        QVERIFY(!reopenedCheckbox->isChecked());
        QVERIFY(QFile::remove(newProgram));
        reopenedCheckbox->click();
        QVERIFY(!reopenedCheckbox->isChecked());
        QVERIFY(reopened.findChild<QLabel *>("serverAutostartStatus")->text().contains("路径无效"));
        QVERIFY2(current.read(enabled, error), qPrintable(error));
        QVERIFY(!enabled);
        QVERIFY(neighborIntact());
    }

    void autostartRejectsForeignOrDamagedRegistrationWithoutChangingIt_data()
    {
        autostartRegistrationIsScopedReversibleAndTracksCurrentProgram_data();
    }

    void autostartRejectsForeignOrDamagedRegistrationWithoutChangingIt()
    {
        QFETCH(bool, windows);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto registry = "HKEY_CURRENT_USER\\Software\\WeChatLite\\AutostartTests\\" +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto cleanup = qScopeGuard([&]
        {
            if (windows) { QSettings settings(registry, QSettings::NativeFormat); settings.remove(""); settings.sync(); }
        });
        const auto location = windows ? registry : temporary.filePath("local.wechatlite.server.plist");
        const QByteArray foreign = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
            "<key>Label</key><string>other.application</string></dict></plist>";
        QFile file(location);
        if (windows)
        {
            QSettings settings(registry, QSettings::NativeFormat);
            settings.setValue("WeChatLiteServer", "other-application.exe --start");
            settings.sync();
        }
        else
        {
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(foreign), foreign.size());
            file.close();
        }
        ServerAutostart registration(windows ? ServerAutostart::Platform::Windows : ServerAutostart::Platform::MacOS, location,
            temporary.filePath("wechat-server"), temporary.filePath("server.json"));
        QString error;
        bool enabled = true;
        QVERIFY(!registration.read(enabled, error));
        QVERIFY(!error.isEmpty() && !enabled);
        QVERIFY(!registration.refreshIfEnabled(error));
        QVERIFY(!registration.setEnabled(false, error));
        AutostartSettings widget(registration);
        auto *checkbox = widget.findChild<QCheckBox *>("serverAutostartEnabled");
        checkbox->click();
        QVERIFY(!checkbox->isChecked());
        QVERIFY(!widget.findChild<QLabel *>("serverAutostartStatus")->text().isEmpty());
        QVERIFY(widget.findChildren<QDialog *>().isEmpty());
        if (windows)
        {
            QSettings settings(registry, QSettings::NativeFormat);
            QCOMPARE(settings.value("WeChatLiteServer").toString(), "other-application.exe --start");
        }
        else
        {
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), foreign);
        }
    }

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
        auto *serverSettingsButton = window.findChild<QPushButton *>("serverConfigure");
        QVERIFY(serverSettingsButton->isEnabled() && serverSettingsButton->menu());
        const auto settingsActions = serverSettingsButton->menu()->actions();
        auto updateAction = std::find_if(settingsActions.cbegin(), settingsActions.cend(),
            [](QAction *action) { return action->objectName() == "serverSoftwareUpdate"; });
        QVERIFY(updateAction != settingsActions.cend());
        QVERIFY(std::any_of(settingsActions.cbegin(), settingsActions.cend(),
            [](QAction *action) { return action->objectName() == "serverAutostart"; }));
        (*updateAction)->trigger();
        auto *updateDialog = window.findChild<QDialog *>("serverSoftwareUpdateDialog");
        QVERIFY(updateDialog && updateDialog->isVisible());
        QVERIFY(updateDialog->findChild<QLabel *>("updateStatus")->text().contains("尚未检查"));
        QVERIFY(window.findChild<QPushButton *>("serverStop")->isEnabled());
        updateDialog->close();
        tabs->setCurrentIndex(2);
        auto *groups = window.findChild<QListWidget *>("clientAccessGroups");
        QTRY_VERIFY_WITH_TIMEOUT(groups->count() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton *>("clientAccessNew")->isEnabled(), 5000);
        QTest::mouseClick(window.findChild<QPushButton *>("clientAccessNew"), Qt::LeftButton);
        window.findChild<QLineEdit *>("clientAccessName")->setText("另一台电脑");
        window.findChild<QCheckBox *>("clientAccessCanSend")->setChecked(true);
        groups->item(0)->setCheckState(Qt::Checked);
        const auto allowedGroup = groups->item(0)->data(Qt::UserRole).toString();
        CredentialInteraction credential(window);
        credential.arm(CredentialInteraction::Action::Copy);
        QTest::mouseClick(window.findChild<QPushButton *>("clientAccessApply"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(credential.complete || credential.timedOut, 5000);
        QVERIFY2(credential.error.isEmpty(), qPrintable(credential.error));
        QVERIFY(credential.hiddenByDefault && credential.revealWorks);
        const auto connection = credential.connection;
        QVERIFY(!connection.value("token").toString().isEmpty());
        QVERIFY(connection.value("token").toString() != administrator);
        QCOMPARE(connection.value("serverUrl").toString(), "http://127.0.0.1:" + QString::number(port));
        auto *list = window.findChild<QListWidget *>("clientAccessList");
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 5000);
        const auto clientId = list->item(0)->data(Qt::UserRole).toJsonObject().value("id").toString();
        QVERIFY(!clientId.isEmpty());
        auto *copyExisting = window.findChild<QPushButton *>("clientAccessCredential");
        QVERIFY(copyExisting);
        QTest::mouseClick(window.findChild<QPushButton *>("clientAccessRefresh"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(copyExisting->isEnabled(), 5000);
        credential.arm(CredentialInteraction::Action::Copy);
        QTest::mouseClick(copyExisting, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(credential.complete || credential.timedOut, 5000);
        QVERIFY2(credential.error.isEmpty(), qPrintable(credential.error));
        QCOMPARE(credential.connection.value("token"), connection.value("token"));
        QCOMPARE(credential.connection.value("serverUrl"), connection.value("serverUrl"));
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject().value("id").toString(), clientId);
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
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton *>("clientAccessRefresh")->isEnabled(), 5000);
        QTest::mouseClick(window.findChild<QPushButton *>("clientAccessRefresh"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(copyExisting->isEnabled(), 5000);
        credential.arm(CredentialInteraction::Action::Copy);
        QTest::mouseClick(copyExisting, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(credential.complete || credential.timedOut, 5000);
        QVERIFY2(credential.error.isEmpty(), qPrintable(credential.error));
        QCOMPARE(credential.connection.value("token"), connection.value("token"));
        QCOMPARE(credential.connection.value("serverUrl"), connection.value("serverUrl"));
        QCOMPARE(list->count(), 1);
        const auto restoredClient = list->item(0)->data(Qt::UserRole).toJsonObject();
        QCOMPARE(restoredClient.value("id").toString(), clientId);
        QVERIFY(!restoredClient.value("can_send").toBool(true));
        QVERIFY(!restoredClient.value("can_download").toBool(true));
        QCOMPARE(restoredClient.value("groups").toArray(), QJsonArray{allowedGroup});
        done = false;
        client.get("/api/groups", [&](const QJsonValue &v, const QString &e) { value = v; error = e; done = true; });
        QTRY_VERIFY(done);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(value.toArray().size(), 1);
        done = false;
        client.get("/api/status", [&](const QJsonValue &v, const QString &e) { value = v; error = e; done = true; });
        QTRY_VERIFY(done);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!value.toObject().value("permissions").toObject().value("can_send").toBool(true));
        QVERIFY(!value.toObject().value("permissions").toObject().value("can_download").toBool(true));
        auto *permissions = window.findChild<ClientPermissions *>("clientPermissions");
        QVERIFY(permissions);
        credential.arm(CredentialInteraction::Action::Forbid, QMessageBox::No);
        permissions->editConnectionRequested();
        QTRY_VERIFY_WITH_TIMEOUT(credential.complete || credential.timedOut, 5000);
        QVERIFY2(credential.error.isEmpty(), qPrintable(credential.error));
        QVERIFY(credential.confirmed && credential.defaultNo);
        QVERIFY(window.findChild<QPushButton *>("serverStop")->isEnabled());
        QVERIFY(!window.findChild<QDialog *>("serviceConnectionSettings"));

        // The owned demo can finish while the consent message box is open. Agreeing
        // afterward must open settings immediately rather than wait for a past signal.
        QFile beforeMaintenance(path);
        QVERIFY(beforeMaintenance.open(QIODevice::ReadOnly));
        const auto configBeforeMaintenance = beforeMaintenance.readAll();
        beforeMaintenance.close();
        bool stoppedDuringConsent = false, acceptedAfterStop = false, openedAutomatic = false, maintenanceTimedOut = false;
        QTimer consent, settings, deadline;
        consent.setInterval(20);
        settings.setInterval(20);
        deadline.setSingleShot(true);
        deadline.setInterval(10000);
        connect(&consent, &QTimer::timeout, &window, [&]
        {
            auto *question = window.findChild<QMessageBox *>();
            if (!question || !question->isVisible())
                return;
            if (!stoppedDuringConsent)
            {
                stoppedDuringConsent = true;
                window.stopService();
                return;
            }
            if (window.findChild<QLabel *>("serverState")->text() != "已停止")
                return;
            auto *yes = question->button(QMessageBox::Yes);
            if (!yes)
                return;
            consent.stop();
            acceptedAfterStop = true;
            yes->click();
        });
        connect(&settings, &QTimer::timeout, &window, [&]
        {
            auto *dialog = window.findChild<QDialog *>("serviceConnectionSettings");
            if (!dialog || !dialog->isVisible())
                return;
            auto *settingsTabs = dialog->findChild<QTabWidget *>("serviceConnectionTabs");
            openedAutomatic = settingsTabs && settingsTabs->currentIndex() == 1;
            settings.stop();
            dialog->reject();
        });
        connect(&deadline, &QTimer::timeout, &window, [&]
        {
            maintenanceTimedOut = true;
            consent.stop();
            settings.stop();
            if (auto *question = window.findChild<QMessageBox *>())
                question->reject();
            if (auto *dialog = window.findChild<QDialog *>("serviceConnectionSettings"))
                dialog->reject();
        });
        consent.start();
        settings.start();
        deadline.start();
        permissions->editConnectionRequested();
        QTRY_VERIFY_WITH_TIMEOUT(openedAutomatic || maintenanceTimedOut, 5000);
        deadline.stop();
        consent.stop();
        settings.stop();
        QVERIFY(!maintenanceTimedOut && stoppedDuringConsent && acceptedAfterStop && openedAutomatic);
        QCOMPARE(window.findChild<QLabel *>("serverState")->text(), QString("已停止"));
        QFile afterMaintenance(path);
        QVERIFY(afterMaintenance.open(QIODevice::ReadOnly));
        QCOMPARE(afterMaintenance.readAll(), configBeforeMaintenance);
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

    void repeatCopyRefreshesAutomaticEntryWithoutSavingPermissionEdits()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AdminFixture fixture;
        fixture.seedClient();
        QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        QVERIFY(writeFixtureConfiguration(directory.path(), fixture));
        const auto storedPolicy = fixture.client;
        ClientPermissions editor(directory.filePath("server.json"));
        CredentialInteraction interaction(editor);
        editor.show();
        editor.refresh();
        auto *list = editor.findChild<QListWidget *>("clientAccessList");
        auto *copy = editor.findChild<QPushButton *>("clientAccessCredential");
        auto *name = editor.findChild<QLineEdit *>("clientAccessName");
        auto *send = editor.findChild<QCheckBox *>("clientAccessCanSend");
        auto *download = editor.findChild<QCheckBox *>("clientAccessCanDownload");
        auto *groups = editor.findChild<QListWidget *>("clientAccessGroups");
        QVERIFY(list && copy && name && send && download && groups);
        QTRY_VERIFY_WITH_TIMEOUT(copy->isEnabled() && list->count() == 1, 5000);
        name->setText("尚未保存的客户端名称");
        send->setChecked(false);
        download->setChecked(true);
        groups->item(0)->setCheckState(Qt::Unchecked);

        const QString sentinel = "credential-cancel-sentinel";
        QApplication::clipboard()->setText(sentinel);
        interaction.arm(CredentialInteraction::Action::Cancel);
        QTest::mouseClick(copy, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(interaction.complete || interaction.timedOut, 5000);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QVERIFY(interaction.hiddenByDefault && interaction.revealWorks);
        QCOMPARE(QApplication::clipboard()->text(), sentinel);
        QCOMPARE(fixture.credentialReads, 1);
        QCOMPARE(fixture.client, storedPolicy);
        QCOMPARE(name->text(), QString("尚未保存的客户端名称"));
        QVERIFY(!send->isChecked() && download->isChecked());
        QCOMPARE(groups->item(0)->checkState(), Qt::Unchecked);

        const auto beforeFirstCopy = fixture.p2pReads;
        interaction.arm(CredentialInteraction::Action::Copy);
        QTest::mouseClick(copy, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(interaction.complete || interaction.timedOut, 5000);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QVERIFY(interaction.hiddenByDefault && interaction.revealWorks);
        QCOMPARE(interaction.addressLabel, QString("公网中转地址"));
        QCOMPARE(interaction.addressText, relayEntry().value("relay_url").toString());
        QVERIFY(interaction.addressReadOnly);
        QCOMPARE(fixture.p2pReads, beforeFirstCopy + 1);
        QCOMPARE(interaction.connection.value("token").toString(), fixture.clientToken);
        QCOMPARE(interaction.connection.value("serverUrl").toString(), "p2p://" + fixture.serverId);
        QVERIFY(interaction.title.contains(storedPolicy.value("name").toString()));
        QVERIFY(!interaction.title.contains(name->text()));
        QVERIFY(!QJsonDocument(interaction.connection).toJson().contains(fixture.secret.toUtf8()));

        // Change both persisted entry details and the live public server ID. A repeat
        // copy must re-read these rather than reuse the first credential's snapshot.
        auto changedEntry = relayEntry();
        changedEntry["relay_url"] = "https://fresh-offline-relay.invalid";
        changedEntry["relay_token"] = "fresh-offline-relay-token";
        changedEntry["relay_quic_port"] = 8443;
        fixture.serverId = QString(64, 'e');
        QVERIFY(writeFixtureConfiguration(directory.path(), fixture, changedEntry));
        const auto beforeSecondCopy = fixture.p2pReads;
        interaction.arm(CredentialInteraction::Action::Copy);
        QTest::mouseClick(copy, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(interaction.complete || interaction.timedOut, 5000);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QCOMPARE(interaction.addressText, changedEntry.value("relay_url").toString());
        QVERIFY(interaction.addressReadOnly);
        QCOMPARE(fixture.p2pReads, beforeSecondCopy + 1);
        QCOMPARE(fixture.credentialReads, 3);
        QCOMPARE(interaction.connection.value("token").toString(), fixture.clientToken);
        QCOMPARE(interaction.connection.value("serverUrl").toString(), "p2p://" + fixture.serverId);
        changedEntry["server_id"] = fixture.serverId;
        QCOMPARE(interaction.connection.value("p2p").toObject(), changedEntry);
        QJsonObject parsed;
        QString error;
        QVERIFY2(ConnectionInfo::parse(QJsonDocument(interaction.connection).toJson(), parsed, error), qPrintable(error));
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject(), storedPolicy);
        QCOMPARE(fixture.client, storedPolicy);
        QCOMPARE(name->text(), QString("尚未保存的客户端名称"));
        QVERIFY(!send->isChecked() && download->isChecked());
        QCOMPARE(groups->item(0)->checkState(), Qt::Unchecked);
        QCOMPARE(fixture.created, 0);
        QCOMPARE(fixture.updated, 0);
        QCOMPARE(fixture.rotated, 0);
        QCOMPARE(fixture.rejectedAuthorization, 0);
    }

    void automaticCredentialDistinguishesRelayAndDirectAddressesAndRequestsEditing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AdminFixture fixture;
        fixture.seedClient();
        QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        QVERIFY(writeFixtureConfiguration(directory.path(), fixture));
        ClientPermissions editor(directory.filePath("server.json"));
        CredentialInteraction interaction(editor);
        QSignalSpy editRequested(&editor, &ClientPermissions::editConnectionRequested);
        editor.show();
        editor.refresh();
        auto *open = editor.findChild<QPushButton *>("clientAccessCredential");
        QTRY_VERIFY_WITH_TIMEOUT(open && open->isEnabled(), 5000);
        QJsonObject directConnection;
        const QString directAddress = "https://192.0.2.47:9876";
        interaction.arm(CredentialInteraction::Action::Copy);
        interaction.inspect = [&](QDialog &dialog) -> QString
        {
            auto *mode = dialog.findChild<QComboBox *>("clientCredentialMode");
            auto *urls = dialog.findChild<QComboBox *>("clientCredentialUrl");
            auto *label = dialog.findChild<QLabel *>("clientCredentialAddressLabel");
            auto *edit = dialog.findChild<QPushButton *>("clientCredentialEditRelay");
            auto *copy = dialog.findChild<QPushButton *>("clientCredentialCopy");
            if (!mode || !urls || !urls->lineEdit() || !label || !edit || !copy)
                return "Credential address controls are missing";
            if (mode->currentData().toString() != "p2p" || !urls->isEnabled() || !urls->lineEdit()->isReadOnly() ||
                urls->currentText() != relayEntry().value("relay_url").toString() ||
                label->text() != "公网中转地址" || !edit->isVisible())
                return "Automatic mode must display its configured relay URL";
            mode->setCurrentIndex(mode->findData("direct"));
            if (urls->lineEdit()->isReadOnly() || !urls->isEnabled() || edit->isVisible() ||
                label->text() != "客户端可达的服务器地址")
                return "Direct mode must provide an editable client-reachable address";
            urls->setEditText(directAddress);
            copy->click();
            directConnection = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
            mode->setCurrentIndex(mode->findData("p2p"));
            if (!urls->lineEdit()->isReadOnly() ||
                urls->currentText() != relayEntry().value("relay_url").toString())
                return "Switching to automatic mode must restore the relay URL";
            mode->setCurrentIndex(mode->findData("direct"));
            if (urls->currentText() != directAddress)
                return "Switching modes lost the typed direct address";
            mode->setCurrentIndex(mode->findData("p2p"));
            return {};
        };
        QTest::mouseClick(open, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(interaction.complete || interaction.timedOut, 5000);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QCOMPARE(directConnection.value("mode").toString(), QString("direct"));
        QCOMPARE(directConnection.value("serverUrl").toString(), directAddress);
        QCOMPARE(directConnection.value("token").toString(), fixture.clientToken);
        QVERIFY(!directConnection.contains("p2p"));
        QCOMPARE(interaction.connection.value("mode").toString(), QString("p2p"));
        QCOMPARE(interaction.connection.value("serverUrl").toString(), "p2p://" + fixture.serverId);
        QCOMPARE(interaction.connection.value("p2p").toObject().value("relay_url"), relayEntry().value("relay_url"));

        const QString sentinel = "edit-relay-does-not-copy-or-rotate";
        QApplication::clipboard()->setText(sentinel);
        interaction.arm(CredentialInteraction::Action::EditRelay);
        QTest::mouseClick(open, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(interaction.complete || interaction.timedOut, 5000);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QTRY_COMPARE(editRequested.size(), 1);
        QVERIFY(!editor.findChild<QDialog *>("clientCredentialDialog"));
        QCOMPARE(QApplication::clipboard()->text(), sentinel);
        QCOMPARE(fixture.created, 0);
        QCOMPARE(fixture.updated, 0);
        QCOMPARE(fixture.rotated, 0);
        QCOMPARE(fixture.rejectedAuthorization, 0);
    }

    void automaticSettingsEditAddressPreservesCredentialsAndCancelDoesNotWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AdminFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        QVERIFY(writeFixtureConfiguration(directory.path(), fixture));
        const auto path = directory.filePath("server.json");
        const auto readConfig = [&]
        {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto originalBytes = readConfig();
        const auto original = QJsonDocument::fromJson(originalBytes).object();
        QWidget owner;
        bool timedOut = false, startsAutomatic = false, editable = false, invalidRejected = false;
        QString failure;
        QTimer deadline;
        deadline.setSingleShot(true);
        deadline.setInterval(10000);
        connect(&deadline, &QTimer::timeout, &owner, [&]
        {
            timedOut = true;
            if (auto *dialog = owner.findChild<QDialog *>("serviceConnectionSettings"))
            {
                if (auto *message = dialog->findChild<QMessageBox *>())
                    message->reject();
                dialog->reject();
            }
        });
        QTimer::singleShot(0, &owner, [&]
        {
            auto *dialog = owner.findChild<QDialog *>("serviceConnectionSettings");
            auto *tabs = dialog ? dialog->findChild<QTabWidget *>("serviceConnectionTabs") : nullptr;
            auto *url = dialog ? dialog->findChild<QLineEdit *>("serviceRelayUrl") : nullptr;
            if (!dialog || !tabs || !url)
            {
                failure = "Automatic settings controls are missing";
                if (dialog)
                    dialog->reject();
                return;
            }
            startsAutomatic = tabs->currentIndex() == 1;
            editable = !url->isReadOnly() && url->isEnabled();
            url->setText("https://cancelled-relay.invalid:54443/");
            dialog->reject();
        });
        deadline.start();
        const bool cancelAccepted = editLocalService(&owner, path, true);
        deadline.stop();
        QVERIFY2(!timedOut && failure.isEmpty(), qPrintable(failure));
        QVERIFY(!cancelAccepted && startsAutomatic && editable);
        QCOMPARE(readConfig(), originalBytes);

        const QString editedUrl = "https://updated-relay.invalid:54443/";
        QTimer::singleShot(0, &owner, [&]
        {
            auto *dialog = owner.findChild<QDialog *>("serviceConnectionSettings");
            auto *url = dialog ? dialog->findChild<QLineEdit *>("serviceRelayUrl") : nullptr;
            auto *save = dialog ? dialog->findChild<QPushButton *>("serviceConnectionSave") : nullptr;
            auto *status = dialog ? dialog->findChild<QLabel *>("serviceP2pEntryStatus") : nullptr;
            if (!dialog || !url || !save || !status)
            {
                failure = "Automatic settings save controls are missing";
                if (dialog)
                    dialog->reject();
                return;
            }
            invalidRejected = true;
            const QStringList invalid{"http://192.0.2.2:54443/", "https://relay.invalid/not-root",
                "https://person@relay.invalid/", "https://relay.invalid:0/"};
            for (const auto &candidate : invalid)
            {
                url->setText(candidate);
                save->click();
                invalidRejected &= dialog->isVisible() && !status->text().isEmpty() && readConfig() == originalBytes;
                if (!invalidRejected)
                {
                    failure = "An invalid public relay address was accepted or changed the saved config";
                    dialog->reject();
                    return;
                }
            }
            url->setText("  " + editedUrl + "  ");
            save->click();
        });
        deadline.start();
        const bool saveAccepted = editLocalService(&owner, path, true);
        deadline.stop();
        QVERIFY2(!timedOut && failure.isEmpty(), qPrintable(failure));
        QVERIFY(saveAccepted && invalidRejected);
        auto expected = original;
        auto p2p = original.value("p2p").toObject();
        p2p["relay_url"] = editedUrl;
        expected["p2p"] = p2p;
        QCOMPARE(QJsonDocument::fromJson(readConfig()).object(), expected);
    }

    void legacySecretNeverRotatesImplicitlyAndConfirmedRotationPreservesPolicy()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AdminFixture fixture;
        fixture.seedClient();
        fixture.tokenKnown = false;
        QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        QVERIFY(writeFixtureConfiguration(directory.path(), fixture));
        const auto storedPolicy = fixture.client;
        const auto originalToken = fixture.clientToken;
        ClientPermissions editor(directory.filePath("server.json"));
        CredentialInteraction interaction(editor);
        editor.show();
        editor.refresh();
        auto *copy = editor.findChild<QPushButton *>("clientAccessCredential");
        auto *rotate = editor.findChild<QPushButton *>("clientAccessRotate");
        auto *status = editor.findChild<QLabel *>("clientAccessStatus");
        auto *list = editor.findChild<QListWidget *>("clientAccessList");
        QVERIFY(copy && rotate && status && list);
        QTRY_VERIFY_WITH_TIMEOUT(copy->isEnabled() && rotate->isEnabled(), 5000);
        editor.findChild<QLineEdit *>("clientAccessName")->setText("尚未提交的权限修改");
        editor.findChild<QCheckBox *>("clientAccessCanSend")->setChecked(false);
        editor.findChild<QCheckBox *>("clientAccessCanDownload")->setChecked(true);
        editor.findChild<QListWidget *>("clientAccessGroups")->item(0)->setCheckState(Qt::Unchecked);

        const QString sentinel = "legacy-null-secret-sentinel";
        QApplication::clipboard()->setText(sentinel);
        interaction.arm(CredentialInteraction::Action::Forbid);
        QTest::mouseClick(copy, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.credentialReads, 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(copy->isEnabled(), 5000);
        interaction.stop();
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QVERIFY(!interaction.complete && !interaction.timedOut);
        QVERIFY(status->text().contains("更换密钥"));
        QCOMPARE(QApplication::clipboard()->text(), sentinel);
        QCOMPARE(fixture.rotated, 0);
        QCOMPARE(fixture.clientToken, originalToken);
        QCOMPARE(fixture.client, storedPolicy);

        const auto beforeCancel = fixture.p2pReads;
        interaction.arm(CredentialInteraction::Action::Forbid, QMessageBox::No);
        QTest::mouseClick(rotate, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(interaction.complete || interaction.timedOut, 5000);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QVERIFY(interaction.confirmed && interaction.defaultNo);
        QCOMPARE(fixture.p2pReads, beforeCancel);
        QCOMPARE(fixture.rotated, 0);
        QCOMPARE(fixture.clientToken, originalToken);

        fixture.available = false;
        const auto beforeUnavailable = fixture.p2pReads;
        interaction.arm(CredentialInteraction::Action::Forbid, QMessageBox::Yes);
        QTest::mouseClick(rotate, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.p2pReads, beforeUnavailable + 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(rotate->isEnabled(), 5000);
        interaction.stop();
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QVERIFY(interaction.confirmed && interaction.defaultNo && !interaction.timedOut);
        QCOMPARE(fixture.rotated, 0); // Resolve usable connection information before invalidating the old key.
        QCOMPARE(fixture.clientToken, originalToken);

        fixture.available = true;
        interaction.arm(CredentialInteraction::Action::Copy, QMessageBox::Yes);
        QTest::mouseClick(rotate, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(interaction.complete || interaction.timedOut, 5000);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QVERIFY(interaction.confirmed && interaction.defaultNo);
        QVERIFY(interaction.hiddenByDefault && interaction.revealWorks);
        QCOMPARE(fixture.rotated, 1);
        QVERIFY(fixture.rotationBody.isEmpty());
        QVERIFY(fixture.clientToken != originalToken);
        QCOMPARE(interaction.connection.value("token").toString(), fixture.clientToken);
        QCOMPARE(fixture.client, storedPolicy);
        QTRY_VERIFY_WITH_TIMEOUT(copy->isEnabled(), 5000);
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject(), storedPolicy);
        interaction.arm(CredentialInteraction::Action::Copy);
        QTest::mouseClick(copy, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(interaction.complete || interaction.timedOut, 5000);
        QVERIFY2(interaction.error.isEmpty(), qPrintable(interaction.error));
        QCOMPARE(interaction.connection.value("token").toString(), fixture.clientToken);
        QCOMPARE(fixture.created, 0);
        QCOMPARE(fixture.updated, 0);
        QCOMPARE(fixture.rotated, 1);
        QCOMPARE(fixture.rejectedAuthorization, 0);
    }
};

QTEST_MAIN(ServerGuiTests)
#include "tests.moc"
