#include "composer.h"
#include "chat_preferences.h"
#include "connection_info.h"
#include "messages.h"
#include "test_tls_identity.h"
#include "tunnel_session.h"
#include "voice_player.h"
#include "window.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDataStream>
#include <QDesktopServices>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtTest>
#include <memory>

class FileOpenHandler : public QObject
{
    Q_OBJECT
  public:
    QList<QUrl> urls;
  public slots:
    void open(const QUrl &url) { urls.append(url); }
};

class DesktopTests : public QObject
{
    Q_OBJECT
    QTemporaryDir files;
    Window *window = nullptr;
    QJsonArray demoGroups;
  private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QFont uiFont;
#ifdef Q_OS_WIN
        uiFont.setFamily("Microsoft YaHei UI");
#else
        uiFont.setFamily("PingFang SC");
#endif
        uiFont.setPixelSize(14);
        qApp->setFont(uiFont);
        QCoreApplication::setOrganizationName("WeChatLiteTests");
        QCoreApplication::setApplicationName("Desktop-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, files.path());
        QSettings().setValue("nickname", "桌面测试");
        QSettings().setValue("downloads", files.filePath("downloads"));
        const auto captureTheme = qEnvironmentVariable("WECHAT_LITE_CAPTURE_THEME");
        if (!captureTheme.isEmpty())
        {
            QFile theme(captureTheme);
            QVERIFY(theme.open(QIODevice::ReadOnly));
            qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));
        }
        window = new Window(true);
        window->show();
        window->activateWindow();
        auto *model = window->findChild<MessageModel *>();
        QVERIFY(model);
        auto *transport = window->findChild<Transport *>();
        QTRY_VERIFY_WITH_TIMEOUT(transport->baseUrl().port() > 0, 8000);
        bool loaded = false;
        QString failure;
        QJsonArray catalog;
        transport->get("/api/local/forwarding",
                       [&](const QJsonValue &value, const QString &error)
                       {
                           failure = error;
                           catalog = value["groups"].toArray();
                           loaded = true;
                       });
        QTRY_VERIFY_WITH_TIMEOUT(loaded, 8000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(catalog.size(), 3);
        QJsonArray selected;
        for (const auto &group : catalog)
            selected.append(group.toObject()["id"]);
        demoGroups = selected;
        loaded = false;
        transport->post("/api/local/forwarding", {{"selected", selected}},
                        [&](const QJsonValue &, const QString &error)
                        {
                            failure = error;
                            loaded = true;
                        });
        QTRY_VERIFY_WITH_TIMEOUT(loaded, 8000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() >= 4, 8000);
    }
    void chatHasOnlySendAndWindowControls()
    {
        const auto buttons = window->findChildren<QPushButton *>();
        QCOMPARE(buttons.size(), 5);
        for (auto *button : buttons)
            QVERIFY(button->objectName() == "sendButton" || button->objectName() == "windowButton" ||
                    button->objectName() == "closeWindow");
        auto *tray = window->findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->contextMenu());
        QStringList entries;
        for (auto *action : tray->contextMenu()->actions())
            entries.append(action->text());
        QVERIFY(entries.contains("连接与下载设置"));
        QVERIFY(entries.contains("退出客户端"));
        for (const auto &entry : entries)
            QVERIFY(!entry.contains("服务配置") && !entry.contains("转发群") && !entry.contains("停止"));
        auto *process = window->findChild<QProcess *>();
        QVERIFY(process && process->arguments().contains("--demo"));
    }
    void serverForwardingPolicyHotUpdatesClientGroups()
    {
        auto *transport = window->findChild<Transport *>();
        auto *mainList = window->findChild<QListWidget *>("groups");
        auto configureServerFixture = [transport](const QJsonArray &selected)
        {
            struct Reply
            {
                bool done = false;
                QString failure;
            };
            auto reply = std::make_shared<Reply>();
            transport->post("/api/local/forwarding", {{"selected", selected}},
                            [reply](const QJsonValue &, const QString &error)
                            {
                                reply->failure = error;
                                reply->done = true;
                            });
            return QTest::qWaitFor([reply] { return reply->done; }, 8000) && reply->failure.isEmpty();
        };
        QVERIFY(configureServerFixture({}));
        QTRY_COMPARE_WITH_TIMEOUT(mainList->count(), 0, 8000);
        QCOMPARE(window->findChild<MessageModel *>()->rowCount(), 0);
        QVERIFY(window->findChild<Composer *>("composer")->isReadOnly());
        QVERIFY(!window->findChild<QPushButton *>("sendButton")->isEnabled());
        QVERIFY(configureServerFixture({"demo-design"}));
        QTRY_COMPARE_WITH_TIMEOUT(mainList->count(), 1, 8000);
        QCOMPARE(mainList->item(0)->data(Qt::UserRole).toJsonObject()["id"].toString(), "demo-design");
        QVERIFY(configureServerFixture(demoGroups));
        QTRY_COMPARE_WITH_TIMEOUT(mainList->count(), 3, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(window->findChild<MessageModel *>()->rowCount() >= 4, 8000);
    }
    void chatPreferencesPersistAndSeparateLogicalServersAndKeys()
    {
        const auto logical = "p2p://" + QString(64, 'a');
        ChatPreferences preferences;
        QVERIFY(preferences.configure(logical, "offline-client-key-a"));
        QVERIFY(!preferences.pinned("group") && !preferences.muted("group"));
        preferences.setPinned("group", true);
        preferences.setMuted("group", true);
        QSettings().sync();
        ChatPreferences restarted;
        QVERIFY(restarted.configure(logical, "offline-client-key-a"));
        QVERIFY(restarted.pinned("group") && restarted.muted("group"));
        QVERIFY(!restarted.configure(logical, "offline-client-key-a"));
        QVERIFY(restarted.configure(logical, "offline-client-key-b"));
        QVERIFY(!restarted.pinned("group") && !restarted.muted("group"));
        QVERIFY(restarted.configure("p2p://" + QString(64, 'b'), "offline-client-key-a"));
        QVERIFY(!restarted.pinned("group") && !restarted.muted("group"));
        QVERIFY(restarted.configure(logical, "offline-client-key-a"));
        restarted.setPinned("group", false);
        QVERIFY(!restarted.pinned("group") && restarted.muted("group"));
        restarted.setMuted("group", false);
        ChatPreferences defaults;
        defaults.configure(logical, "offline-client-key-a");
        QVERIFY(!defaults.pinned("group") && !defaults.muted("group"));
    }
    void groupMenuPinsMutesAndPreservesSelectionSearchAndNormalOrder()
    {
        auto *list = window->findChild<QListWidget *>("groups");
        auto *search = window->findChild<QLineEdit *>("search");
        auto *transport = window->findChild<Transport *>();
        QTRY_VERIFY(list->count() >= 2 && list->currentItem());
        const auto selected = list->currentItem()->data(Qt::UserRole).toJsonObject();
        const auto selectedId = selected.value("id").toString();
        QString target;
        for (int row = list->count() - 1; row >= 0; --row)
            if (list->item(row)->data(Qt::UserRole).toJsonObject().value("id").toString() != selectedId)
            {
                target = list->item(row)->data(Qt::UserRole).toJsonObject().value("id").toString();
                break;
            }
        QVERIFY(!target.isEmpty());
        search->setText(selected.value("name").toString());
        QHash<QString, bool> hidden;
        for (int row = 0; row < list->count(); ++row)
            hidden.insert(list->item(row)->data(Qt::UserRole).toJsonObject().value("id").toString(),
                list->item(row)->isHidden());
        auto *menu = window->groupContextMenuFor(target);
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(menu->actions().first()->text(), QString("置顶"));
        QCOMPARE(menu->actions().last()->text(), QString("忽略消息通知"));
        QVERIFY(menu->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(!menu->toolTipsVisible());
        menu->actions().first()->trigger();
        delete menu;
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject().value("id").toString(), target);
        QVERIFY(list->item(0)->data(Qt::UserRole).toJsonObject().value("pinned").toBool());
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString(), selectedId);
        QCOMPARE(search->text(), selected.value("name").toString());
        for (int row = 0; row < list->count(); ++row)
            QCOMPARE(list->item(row)->isHidden(),
                hidden.value(list->item(row)->data(Qt::UserRole).toJsonObject().value("id").toString()));
        menu = window->groupContextMenuFor(target);
        QCOMPARE(menu->actions().first()->text(), QString("取消置顶"));
        menu->actions().last()->trigger();
        delete menu;
        QSignalSpy refreshed(list->model(), &QAbstractItemModel::rowsInserted);
        transport->event({{"type", "groups"}});
        QTRY_VERIFY_WITH_TIMEOUT(!refreshed.isEmpty(), 8000);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject().value("id").toString(), target);
        menu = window->groupContextMenuFor(target);
        QCOMPARE(menu->actions().last()->text(), QString("显示消息通知"));
        menu->actions().last()->trigger();
        delete menu;
        transport->event({{"type", "message"}, {"created", false}, {"seq", 99999},
            {"message", QJsonObject{{"id", "group-order-fixture"}, {"group", selectedId},
                {"sender", "离线成员"}, {"text", "排序回归"}, {"status", "received"},
                {"timestamp", QDateTime::currentMSecsSinceEpoch() + 1000}}}});
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject().value("id").toString(), target);
        menu = window->groupContextMenuFor(target);
        menu->actions().first()->trigger();
        delete menu;
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject().value("id").toString(), selectedId);
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString(), selectedId);
        QVERIFY(!window->groupContextMenuFor("not-authorized"));
        search->clear();
        auto *composer = window->findChild<Composer *>();
        composer->setPlainText("activation keeps this draft");
        list->clear(); // An activation can precede the asynchronous catalog response.
        window->activateConversation(target);
        QTRY_VERIFY_WITH_TIMEOUT(list->currentItem() &&
            list->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString() == target, 8000);
        window->activateConversation(selectedId);
        QTRY_COMPARE(composer->toPlainText(), QString("activation keeps this draft"));
        QSignalSpy rejectedActivation(list->model(), &QAbstractItemModel::rowsInserted);
        window->activateConversation("not-authorized");
        QTRY_VERIFY_WITH_TIMEOUT(!rejectedActivation.isEmpty(), 8000);
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString(), selectedId);
        composer->clear();
    }
    void systemNotificationsOnlyDescribeNewIncomingMessagesAfterSync()
    {
        auto *transport = window->findChild<Transport *>();
        auto *list = window->findChild<QListWidget *>("groups");
        QTRY_VERIFY(list->count() >= 2 && list->currentItem());
        const auto current = list->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString();
        QString other;
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toJsonObject().value("id").toString() != current)
                other = list->item(row)->data(Qt::UserRole).toJsonObject().value("id").toString();
        QSignalSpy notifications(window, &Window::systemNotificationRequested);
        qint64 seq = 1000000;
        const auto event = [&seq](const QString &id, const QString &group)
        {
            return QJsonObject{{"type", "message"}, {"seq", ++seq}, {"created", true},
                {"message", QJsonObject{{"id", "notification-" + id}, {"group", group},
                    {"sender", "离线成员"}, {"text", "测试新消息"}, {"status", "received"},
                    {"timestamp", 1}}}}; // Server creation truth works regardless of machine clock skew.
        };
        window->hide();
        transport->connectionChanged(false);
        transport->connectionChanged(true);
        auto replay = event("replay", current);
        transport->event(replay);
        QVERIFY(notifications.isEmpty());
        transport->event({{"type", "synced"}, {"seq", seq}});
        replay["seq"] = ++seq;
        transport->event(replay);
        auto historical = event("historical", current);
        historical["historical"] = true;
        transport->event(historical);
        auto updated = event("update", current);
        updated["created"] = false;
        transport->event(updated);
        auto legacy = event("legacy", current);
        legacy.remove("created");
        transport->event(legacy);
        auto own = event("own", current);
        auto ownMessage = own.value("message").toObject();
        ownMessage["outgoing"] = true;
        own["message"] = ownMessage;
        transport->event(own);
        auto withdrawn = event("withdrawn", current);
        auto withdrawnMessage = withdrawn.value("message").toObject();
        withdrawnMessage["status"] = "recalled";
        withdrawn["message"] = withdrawnMessage;
        transport->event(withdrawn);
        QVERIFY(notifications.isEmpty());
        auto fresh = event("fresh", current);
        transport->event(fresh);
        QCOMPARE(notifications.size(), 1);
        QCOMPARE(notifications.last().at(0).toString(), current);
        QVERIFY(notifications.last().at(2).toString().contains("离线成员：测试新消息"));
        fresh["seq"] = ++seq;
        transport->event(fresh);
        auto completion = fresh.value("message").toObject();
        completion["media"] = QJsonArray{QJsonObject{{"kind", "gif"}, {"id", "completed"}}};
        fresh["message"] = completion;
        fresh["created"] = false;
        transport->event(fresh);
        QCOMPARE(notifications.size(), 1);
        auto *menu = window->groupContextMenuFor(current);
        QCOMPARE(menu->actions().last()->text(), QString("忽略消息通知"));
        menu->actions().last()->trigger();
        delete menu;
        const auto muted = event("muted", current);
        transport->event(muted);
        QCOMPARE(notifications.size(), 1);
        bool receivedWhileMuted = false;
        for (const auto &row : window->findChild<MessageModel *>()->rows)
            receivedWhileMuted |= row.value("id") == muted.value("message").toObject().value("id");
        QVERIFY(receivedWhileMuted);
        menu = window->groupContextMenuFor(current);
        QCOMPARE(menu->actions().last()->text(), QString("显示消息通知"));
        menu->actions().last()->trigger();
        delete menu;
        window->showNormal();
        QApplication::setActiveWindow(window);
        QTRY_VERIFY(window->isActiveWindow());
        transport->event(event("active-current", current));
        QCOMPARE(notifications.size(), 1);
        transport->event(event("active-other", other));
        QCOMPARE(notifications.size(), 2);
        window->hide();
        auto media = event("media", current);
        auto mediaMessage = media.value("message").toObject();
        mediaMessage["text"] = "";
        mediaMessage["media"] = QJsonArray{QJsonObject{{"kind", "gif"}, {"id", "new-gif"}}};
        media["message"] = mediaMessage;
        transport->event(media);
        QCOMPARE(notifications.size(), 3);
        QVERIFY(notifications.last().at(2).toString().contains("[GIF]"));
        transport->connectionChanged(false);
        transport->event(event("disconnected", current));
        transport->connectionChanged(true);
        auto reconnectReplay = event("reconnect-replay", current);
        transport->event(reconnectReplay);
        transport->event({{"type", "synced"}, {"seq", seq}});
        reconnectReplay["seq"] = ++seq;
        transport->event(reconnectReplay);
        QCOMPARE(notifications.size(), 3);
        transport->event(event("after-reconnect", current));
        QCOMPARE(notifications.size(), 4);
        transport->event(event("unknown-group", "not-authorized"));
        QCOMPARE(notifications.size(), 4);
        transport->event({{"type", "reset"}});
        window->showNormal();
        window->activateConversation(current);
        // The simulated disconnect cleared Window::ready. The real socket never reconnected,
        // so restore the actual demo handshake status rather than leaking that fixture state.
        struct DemoStatus
        {
            bool done = false;
            QJsonObject value;
            QString error;
        };
        auto status = std::make_shared<DemoStatus>();
        transport->get("/api/status", [status](const QJsonValue &value, const QString &error)
        {
            status->value = value.toObject();
            status->error = error;
            status->done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(status->done, 8000);
        QVERIFY2(status->error.isEmpty(), qPrintable(status->error));
        QVERIFY(status->value.value("ready").toBool());
        status->value.insert("type", "status");
        transport->event(status->value);
        transport->event({{"type", "synced"}, {"seq", 0}});
    }
    void automaticClientStoresStableIdentityAndStartsOnlyOnSave()
    {
        QSettings settings;
        const QStringList keys{"serverUrl", "token", "certificatePin", "connectionMode", "p2p"};
        QVariantMap previous;
        for (const auto &key : keys)
            previous[key] = settings.value(key);
        auto restore = qScopeGuard([&]
        {
            for (const auto &key : keys)
                if (previous[key].isValid())
                    settings.setValue(key, previous[key]);
                else
                    settings.remove(key);
        });
        settings.remove("serverUrl");
        settings.remove("token");
        auto helper = QCoreApplication::applicationDirPath() + "/link-test-fixture"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
        Window isolated(false, helper);
        isolated.show();
        auto *tunnel = isolated.findChild<TunnelSession *>();
        QVERIFY(tunnel);
        QSignalSpy helperReady(tunnel, &TunnelSession::ready);
        QJsonObject p2p{{"server_id", QString(64, 'a')}, {"relay_url", "https://relay.invalid"},
            {"relay_token", "fixture-good"}, {"relay_ca", QString::fromUtf8(TestTlsIdentity::caPem)},
            {"relay_quic_port", 443}};
        const auto stable = "p2p://" + QString(64, 'a');
        const QJsonObject credential{{"version", 1}, {"mode", "p2p"}, {"serverUrl", stable},
            {"token", QString(32, 'c')}, {"certificatePin", QString(64, 'b')}, {"p2p", p2p}};
        bool pasteOnly = false;
        QTimer::singleShot(0, &isolated, [&]
        {
            auto *dialog = isolated.findChild<QDialog *>("connectionSettings");
            if (!dialog)
                return;
            QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(credential).toJson()));
            QTest::mouseClick(dialog->findChild<QPushButton *>("pasteConnectionInfo"), Qt::LeftButton);
            auto *url = dialog->findChild<QLineEdit *>("connectionServerUrl");
            pasteOnly = helperReady.isEmpty() && tunnel->findChildren<QProcess *>().isEmpty() &&
                dialog->findChild<QComboBox *>("connectionMode")->currentData().toString() == "p2p" &&
                url->text() == stable && url->isReadOnly();
            QTest::mouseClick(dialog->findChild<QPushButton *>("connectionSave"), Qt::LeftButton);
        });
        isolated.showSettings();
        QVERIFY(pasteOnly);
        QTRY_COMPARE_WITH_TIMEOUT(helperReady.size(), 1, 5000);
        auto *transport = isolated.findChild<Transport *>();
        QCOMPARE(transport->baseUrl().scheme(), QString("https"));
        QCOMPARE(transport->baseUrl().host(), QString("127.0.0.1"));
        QVERIFY(transport->baseUrl().port() > 0);
        QCOMPARE(settings.value("serverUrl").toString(), stable);
        QCOMPARE(settings.value("connectionMode").toString(), QString("p2p"));
        QCOMPARE(QJsonDocument::fromJson(settings.value("p2p").toByteArray()).object(), p2p);
        transport->disconnectService();
        tunnel->stop();
        QCOMPARE(settings.value("serverUrl").toString(), stable);
    }
    void remoteClientOnlyConnectsAndNeverManagesTheServer()
    {
        QSettings settings;
        const QStringList keys{"serverUrl", "token", "nickname", "downloads", "certificatePin", "connectionMode", "p2p"};
        QVariantMap previous;
        for (const auto &key : keys)
            previous[key] = settings.value(key);
        auto restoreSettings = qScopeGuard(
            [&]
            {
                for (const auto &key : keys)
                    if (previous[key].isValid())
                        settings.setValue(key, previous[key]);
                    else
                        settings.remove(key);
            });
        settings.remove("serverUrl");
        settings.remove("token");
        QTcpServer http;
        QVERIFY(http.listen(QHostAddress::LocalHost, 0));
        int requests = 0, management = 0, messagePosts = 0, mediaGets = 0, mediaUploads = 0, statusReads = 0;
        bool allowed = false;
        const QJsonObject media{
            {"id", "not-downloaded"}, {"kind", "file"}, {"name", "private.txt"}, {"size", 12}};
        connect(
            &http, &QTcpServer::newConnection, &http,
            [&]
            {
                while (http.hasPendingConnections())
                {
                    auto *socket = http.nextPendingConnection();
                    auto buffer = std::make_shared<QByteArray>();
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    connect(
                        socket, &QTcpSocket::readyRead, &http,
                        [&, socket, buffer]
                        {
                            buffer->append(socket->readAll());
                            if (!buffer->contains("\r\n\r\n") || socket->property("handled").toBool())
                                return;
                            socket->setProperty("handled", true);
                            const auto request = buffer->left(buffer->indexOf('\n')).split(' ');
                            const auto path =
                                QUrl("http://localhost" + QString::fromLatin1(request.value(1))).path();
                            ++requests;
                            management += path.startsWith("/api/local/") || path == "/api/shutdown";
                            messagePosts += path == "/api/messages" && request.value(0) == "POST";
                            mediaGets += path.startsWith("/api/media/");
                            mediaUploads += path == "/api/media" && request.value(0) == "POST";
                            QJsonValue value(QJsonObject{});
                            if (path == "/api/status")
                            {
                                ++statusReads;
                                value = QJsonObject{{"permissions", QJsonObject{{"can_send", allowed},
                                                                                {"can_download", allowed}}}};
                            }
                            else if (path == "/api/groups")
                                value = QJsonArray{QJsonObject{{"id", "remote-group"}, {"name", "远程群"}}};
                            else if (path.endsWith("/members"))
                                value = QJsonArray{};
                            else if (path == "/api/history")
                                value = QJsonArray{QJsonObject{{"id", "remote-message"},
                                                               {"group", "remote-group"},
                                                               {"text", "server data"},
                                                               {"media", QJsonArray{media}}}};
                            const auto body =
                                value.isArray()
                                    ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                    : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
                            socket->write(
                                "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n"
                                "Content-Length: " +
                                QByteArray::number(body.size()) + "\r\n\r\n" + body);
                            socket->disconnectFromHost();
                        });
                }
            });
        Window isolated(false);
        isolated.show();
        auto *process = isolated.findChild<QProcess *>();
        QVERIFY(process);
        QSignalSpy spawned(process, &QProcess::started);
        const auto base = "http://127.0.0.1:" + QString::number(http.serverPort());
        const auto accessKey = QString(32, 'c');
        bool pastedWithoutConnecting = false, settingsOnly = false, oversizeRejected = false;
        QTimer::singleShot(
            0, &isolated,
            [&]
            {
                auto *dialog = isolated.findChild<QDialog *>("connectionSettings");
                if (!dialog)
                    return;
                const auto fields = dialog->findChildren<QLineEdit *>();
                auto *url = dialog->findChild<QLineEdit *>("connectionServerUrl");
                auto *token = dialog->findChild<QLineEdit *>("connectionAccessKey");
                auto *paste = dialog->findChild<QPushButton *>("pasteConnectionInfo");
                auto *save = dialog->findChild<QPushButton *>("connectionSave");
                settingsOnly = fields.size() == 5 && url && token && paste && save;
                for (auto *button : dialog->findChildren<QPushButton *>())
                    settingsOnly &= !button->text().contains("启动") && !button->text().contains("转发群") &&
                                    !button->text().contains("本机服务") && !button->text().contains("演示");
                if (!settingsOnly)
                {
                    dialog->reject();
                    return;
                }
                QApplication::clipboard()->setText(QString::fromUtf8(
                    QJsonDocument(
                        QJsonObject{{"serverUrl", base}, {"token", accessKey}, {"certificatePin", ""}})
                        .toJson()));
                QTest::mouseClick(paste, Qt::LeftButton);
                pastedWithoutConnecting =
                    url->text() == base && token->text() == accessKey && requests == 0 && spawned.isEmpty();
                QApplication::clipboard()->setText(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{"serverUrl", "https://not-imported.invalid"},
                                              {"token", QString(32, 'o')},
                                              {"padding", QStringLiteral("汉").repeated(3000)}})
                        .toJson()));
                QTest::mouseClick(paste, Qt::LeftButton);
                oversizeRejected =
                    url->text() == base && token->text() == accessKey &&
                    dialog->findChild<QLabel *>("connectionPasteResult")->text().contains("8 KiB");
                QTest::mouseClick(save, Qt::LeftButton);
            });
        isolated.showSettings();
        QVERIFY(settingsOnly && pastedWithoutConnecting && oversizeRejected);
        auto *transport = isolated.findChild<Transport *>();
        transport->connectionChanged(true);
        auto *list = isolated.findChild<QListWidget *>("groups");
        auto *model = isolated.findChild<MessageModel *>();
        auto *view = isolated.findChild<MessageView *>();
        auto *composer = isolated.findChild<Composer *>("composer");
        auto *attachments = isolated.findChild<AttachmentStore *>();
        auto *send = isolated.findChild<QPushButton *>("sendButton");
        QTRY_COMPARE(list->count(), 1);
        QTRY_COMPARE(model->rowCount(), 1);
        QTRY_VERIFY(statusReads > 0 && !view->property("canDownload").toBool());
        composer->setPlainText("keep this local draft");
        QFile staged(files.filePath("permission-staged.txt"));
        QVERIFY(staged.open(QIODevice::WriteOnly));
        staged.write("staged locally");
        staged.close();
        attachments->addFiles({staged.fileName()});
        QVERIFY(!send->isEnabled());
        composer->sendRequested();
        view->mediaClicked(media, true);
        auto *menu = view->contextMenuFor(model->index(0), false);
        QVERIFY(menu);
        bool disabledSave = false;
        for (auto *action : menu->actions())
            if (action->text().startsWith("保存 "))
                disabledSave = !action->isEnabled();
        delete menu;
        QVERIFY(disabledSave);
        QTest::qWait(30);
        QCOMPARE(messagePosts, 0);
        QCOMPARE(mediaGets, 0);
        QCOMPARE(mediaUploads, 0);
        QCOMPARE(attachments->items.size(), 1);
        QCOMPARE(composer->toPlainText(), "keep this local draft");
        allowed = true;
        transport->connectionChanged(true);
        QTRY_VERIFY(send->isEnabled() && view->property("canDownload").toBool());
        menu = view->contextMenuFor(model->index(0), false);
        QVERIFY(menu);
        for (auto *action : menu->actions())
            if (action->text().startsWith("保存 "))
                QVERIFY(action->isEnabled());
        delete menu;
        transport->disconnectService();
        isolated.close();
        QTest::qWait(30);
        QCOMPARE(process->state(), QProcess::NotRunning);
        QCOMPARE(spawned.count(), 0);
        QCOMPARE(management, 0);
        QVERIFY(http.isListening());
    }
    void pastedFilesAndScreenshotAreStagedThenSent()
    {
        auto *composer = window->findChild<Composer *>("composer");
        auto *store = window->findChild<AttachmentStore *>();
        auto *model = window->findChild<MessageModel *>();
        QVERIFY(composer && store && model);
        QVERIFY(store->items.isEmpty());
        auto filePath = files.filePath("报告.txt"), videoPath = files.filePath("剪辑.mp4"),
             gifPath = files.filePath("动画.gif");
        for (const auto &path : QStringList{filePath, videoPath, gifPath})
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(path == gifPath     ? QByteArray::fromBase64(
                                                 "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7")
                       : path == videoPath ? QByteArray("0000ftypmp42")
                                           : QString("文件测试内容").toUtf8());
        }
        auto *mime = new QMimeData;
        mime->setUrls(
            {QUrl::fromLocalFile(filePath), QUrl::fromLocalFile(videoPath), QUrl::fromLocalFile(gifPath)});
        QApplication::clipboard()->setMimeData(mime);
        composer->paste();
        QCOMPARE(store->items.size(), 3);
        auto *imageMime = new QMimeData;
        QImage image(48, 48, QImage::Format_ARGB32);
        image.fill(Qt::green);
        imageMime->setImageData(image);
        QApplication::clipboard()->setMimeData(imageMime);
        composer->paste();
        QCOMPARE(store->items.size(), 4);
        auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        composer->insertPlainText("桌面附件验证 " + id);
        auto originalCount = model->rowCount();
        QCOMPARE(model->rowCount(), originalCount);
        auto *send = window->findChild<QPushButton *>("sendButton");
        QVERIFY(send);
        QTRY_VERIFY(send->isEnabled());
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(composer->toPlainText().isEmpty(), 8000);
        QCOMPARE(store->items.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(model->rows.begin(), model->rows.end(),
                                             [&](const QJsonObject &message)
                                             {
                                                 return message["text"].toString().contains(id) &&
                                                        message["status"] == "sent" &&
                                                        message["media"].toArray().size() == 4;
                                             }),
                                 8000);
    }
    void actualMentionCanBeSelectedAndSent()
    {
        auto *composer = window->findChild<Composer *>("composer");
        auto *model = window->findChild<MessageModel *>();
        window->activateWindow();
        composer->setFocus();
        QTRY_VERIFY(composer->hasFocus());
        QTest::keyClicks(composer, "@");
        QTRY_VERIFY(composer->popupVisible());
        QTest::keyClick(composer, Qt::Key_Return);
        QCOMPARE(composer->mentions().size(), 1);
        auto member = composer->mentions().first()["id"].toString();
        auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        composer->insertPlainText("确认测试 " + id);
        auto *send = window->findChild<QPushButton *>("sendButton");
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(model->rows.begin(), model->rows.end(),
                        [&](const QJsonObject &message)
                        {
                            return message["text"].toString().contains(id) && message["status"] == "sent" &&
                                   message["mentions"].toArray().first()["id"].toString() == member;
                        }),
            8000);
    }
    void revokingAGroupCancelsItsUploadAndPreservesItsDraft()
    {
        QTcpServer http;
        QVERIFY(http.listen(QHostAddress::LocalHost, 0));
        int policy = 0, olderRequests = 0, activations = 0, memberRequests = 0, pendingOlderReads = 0;
        bool pendingNextOlder = false;
        QJsonArray sourceHistory, olderHistory;
        for (int i = 1; i <= 120; ++i)
            sourceHistory.append(QJsonObject{{"id", "h" + QString::number(i)},
                                             {"seq", i + 1},
                                             {"timestamp", i * 1000},
                                             {"text", "old history"}});
        sourceHistory.append(
            QJsonObject{{"id", "latest"}, {"seq", 1}, {"timestamp", 200000}, {"text", "latest text"}});
        for (int i = 0; i < 60; ++i)
            olderHistory.append(QJsonObject{{"id", "page" + QString::number(i)},
                                            {"seq", i + 500},
                                            {"timestamp", i + 10},
                                            {"text", "older page"}});
        bool uploadStarted = false, holdNextHistory = false;
        QPointer<QTcpSocket> heldHistory;
        int recalls = 0, locateRequests = 0, locatePages = 0;
        bool locateWrongGroup = false, holdLocatePage = false;
        QPointer<QTcpSocket> locatePageSocket;
        QJsonObject original{
            {"id", "missing-source"}, {"group", "A"},     {"seq", 7},
            {"timestamp", 1},         {"sender", "成员"}, {"text", "source outside current page"}};
        QJsonObject recalledMessage;
        QString uploadGroup;
        QList<QJsonObject> sent;
        connect(&http, &QTcpServer::newConnection, &http,
                [&]
                {
                    while (http.hasPendingConnections())
                    {
                        auto *socket = http.nextPendingConnection();
                        auto buffer = std::make_shared<QByteArray>();
                        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                        connect(socket, &QTcpSocket::readyRead, &http,
                                [&, socket, buffer]
                                {
                                    buffer->append(socket->readAll());
                                    const int end = buffer->indexOf("\r\n\r\n");
                                    if (end < 0 || socket->property("handled").toBool())
                                        return;
                                    const auto headers = buffer->left(end).split('\n');
                                    qint64 length = 0;
                                    for (const auto &header : headers)
                                        if (header.toLower().startsWith("content-length:"))
                                            length = header.mid(15).trimmed().toLongLong();
                                    if (buffer->size() < end + 4 + length)
                                        return;
                                    socket->setProperty("handled", true);
                                    const auto first = headers.first().split(' ');
                                    const auto url = QUrl("http://localhost" + QString::fromLatin1(first[1]));
                                    const auto path = url.path();
                                    QJsonValue value(QJsonObject{});
                                    if (path == "/api/groups")
                                    {
                                        QJsonArray list;
                                        if (policy == 0)
                                            list.append(QJsonObject{{"id", "A"},
                                                                    {"name", "Group A"},
                                                                    {"timestamp", 200000},
                                                                    {"preview", "latest text"}});
                                        if (policy < 2)
                                            list.append(QJsonObject{{"id", "B"}, {"name", "Group B"}});
                                        value = list;
                                    }
                                    else if ((path == "/api/messages" || path == "/api/history") && first[0] == "GET")
                                    {
                                        const QUrlQuery query(url);
                                        if (query.queryItemValue("before") == "7")
                                        {
                                            ++locatePages;
                                            if (holdLocatePage)
                                            {
                                                locatePageSocket = socket;
                                                return;
                                            }
                                            value = QJsonArray{};
                                        }
                                        else if (query.hasQueryItem("before"))
                                        {
                                            if (pendingNextOlder)
                                            {
                                                pendingNextOlder = false;
                                                ++pendingOlderReads;
                                                value = QJsonObject{{"messages", QJsonArray{}},
                                                    {"has_more", true}, {"pending", true}};
                                            }
                                            else
                                                value = ++olderRequests == 1 ? olderHistory : QJsonArray{};
                                        }
                                        else
                                        {
                                            if (query.queryItemValue("group") == "A" && holdNextHistory)
                                            {
                                                holdNextHistory = false;
                                                heldHistory = socket;
                                                return;
                                            }
                                            value = query.queryItemValue("group") == "A" ? sourceHistory
                                                                                         : QJsonArray{};
                                        }
                                    }
                                    else if (path == "/api/messages/missing-source")
                                    {
                                        ++locateRequests;
                                        QCOMPARE(QUrlQuery(url).queryItemValue("group"), "A");
                                        value = original;
                                        if (locateWrongGroup)
                                        {
                                            auto invalid = original;
                                            invalid["group"] = "B";
                                            value = invalid;
                                        }
                                    }
                                    else if (path.endsWith("/members"))
                                    {
                                        ++memberRequests;
                                        value = QJsonArray{};
                                    }
                                    else if (path.endsWith("/activate"))
                                        ++activations;
                                    else if (path == "/api/media")
                                    {
                                        uploadStarted = true;
                                        uploadGroup = QUrlQuery(url).queryItemValue("group");
                                        return; // Keep the old upload response pending while policy changes.
                                    }
                                    else if (path == "/api/messages")
                                    {
                                        auto message =
                                            QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
                                        sent.append(message);
                                        message["seq"] = 1;
                                        message["status"] = "sent";
                                        value = message;
                                    }
                                    else if (path == "/api/messages/recallable/recall")
                                    {
                                        ++recalls;
                                        recalledMessage["recall_status"] = "pending";
                                        value = recalledMessage;
                                    }
                                    if (path == "/api/history" && value.isArray())
                                        value = QJsonObject{{"messages", value.toArray()},
                                            {"has_more", !value.toArray().isEmpty()}, {"pending", false}};
                                    const auto body =
                                        value.isArray()
                                            ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                            : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
                                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: "
                                                  "application/json\r\nContent-Length: " +
                                                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
                                    socket->disconnectFromHost();
                                });
                    }
                });
        Window isolated(false);
        isolated.show();
        auto *transport = isolated.findChild<Transport *>();
        const auto token = QString(32, 's');
        transport->configure("http://127.0.0.1:" + QString::number(http.serverPort()), token);
        transport->connectionChanged(true);
        transport->event({{"type", "status"}, {"ready", false}});
        auto *list = isolated.findChild<QListWidget *>("groups");
        auto *composer = isolated.findChild<Composer *>("composer");
        auto *store = isolated.findChild<AttachmentStore *>();
        auto *send = isolated.findChild<QPushButton *>("sendButton");
        QTRY_COMPARE(list->count(), 2);
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString(), "A");
        QCOMPARE(activations, 0);
        transport->event({{"type", "status"}, {"ready", true}});
        QTRY_COMPARE(activations, 1);
        QTRY_VERIFY(memberRequests >= 2);
        transport->event({{"type", "status"}, {"ready", true}});
        QTest::qWait(20);
        QCOMPARE(activations, 1);
        auto *history = isolated.findChild<MessageModel *>();
        auto *view = isolated.findChild<MessageView *>();
        QTRY_COMPARE(history->rowCount(), 121);
        QCOMPARE(history->rows.last()["id"].toString(), "latest");
        QCOMPARE(olderRequests, 0);
        view->scrollToTop();
        const auto anchor = view->indexAt(QPoint(1, 0)).data(Qt::UserRole).toJsonObject()["id"].toString();
        auto scrollOlder = [view]
        {
            QWheelEvent wheel(QPointF(20, 20), view->viewport()->mapToGlobal(QPoint(20, 20)), QPoint(),
                              QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(view->viewport(), &wheel);
        };
        pendingNextOlder = true;
        scrollOlder();
        QTRY_COMPARE(history->rowCount(), 181);
        QCOMPARE(pendingOlderReads, 1);
        QTRY_COMPARE(view->indexAt(QPoint(1, 0)).data(Qt::UserRole).toJsonObject()["id"].toString(), anchor);
        QVERIFY(view->verticalScrollBar()->value() < view->verticalScrollBar()->maximum() - 100);
        transport->event({{"type", "message"},
                          {"message", QJsonObject{{"id", "late"},
                                                  {"seq", 900},
                                                  {"group", "A"},
                                                  {"timestamp", 500},
                                                  {"text", "late old history"},
                                                  {"status", "received"}}}});
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject()["preview"].toString(), "latest text");
        QCOMPARE(history->rows.last()["id"].toString(), "latest");
        QVERIFY(view->verticalScrollBar()->value() < view->verticalScrollBar()->maximum() - 100);
        view->scrollToTop();
        scrollOlder();
        QTRY_COMPARE(olderRequests, 2);
        QTest::qWait(20);
        for (int i = 0; i < 4; ++i)
            scrollOlder();
        QTest::qWait(20);
        QCOMPARE(olderRequests, 2);
        const auto path = files.filePath("held-upload.txt");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("draft attachment");
        file.close();
        store->addFiles({path});
        composer->setPlainText("old group draft");
        view->quoteRequested(history->rows.last());
        auto *quoteArea = isolated.findChild<QWidget *>("quotePreview");
        auto *quoteText = isolated.findChild<QLabel *>("quoteText");
        auto *cancelQuote = isolated.findChild<QToolButton *>("cancelQuote");
        QVERIFY(quoteArea->isVisible());
        QVERIFY(quoteText->accessibleName().contains("latest text"));
        QVERIFY(quoteText->toolTip().isEmpty());
        QVERIFY(quoteArea->mapToGlobal(QPoint()).y() >
                composer->mapToGlobal(QPoint(0, composer->height() - 1)).y());
        QTRY_VERIFY(qAbs(quoteArea->mapToGlobal(quoteArea->rect().center()).y() -
                         send->mapToGlobal(send->rect().center()).y()) <= 1);
        QVERIFY(!isolated.findChild<QLabel *>("notice"));
        const auto composeTop = composer->mapToGlobal(QPoint()).y();
        transport->warning("synthetic transport failure");
        QTest::qWait(20);
        QCOMPARE(composer->mapToGlobal(QPoint()).y(), composeTop);
        QVERIFY(!isolated.findChild<QLabel *>("notice"));
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY(uploadStarted);
        QVERIFY(!cancelQuote->isEnabled());
        QCOMPARE(uploadGroup, "A");
        policy = 1;
        transport->event({{"type", "groups"}});
        QTRY_COMPARE(list->count(), 1);
        QTRY_COMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString(), "B");
        QVERIFY(composer->toPlainText().isEmpty());
        QVERIFY(store->items.isEmpty());
        QVERIFY(QFile::exists(path));
        composer->setPlainText("new group text");
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(sent.first()["group"].toString(), "B");
        QCOMPARE(sent.first()["text"].toString(), "new group text");
        QVERIFY(sent.first()["media"].toArray().isEmpty());
        QVERIFY(!sent.first().contains("quote"));
        policy = 2;
        transport->event({{"type", "groups"}});
        QTRY_COMPARE(list->count(), 0);
        QVERIFY(isolated.findChild<MessageModel *>()->rows.isEmpty());
        QVERIFY(!send->isEnabled());
        QVERIFY(composer->isReadOnly());
        policy = 0;
        holdNextHistory = true;
        transport->event({{"type", "groups"}});
        QTRY_COMPARE(list->count(), 2);
        QTRY_COMPARE(composer->toPlainText(), "old group draft");
        QCOMPARE(store->items.size(), 1);
        QCOMPARE(store->items.first().path, path);
        QVERIFY(store->items.first().uploaded.isEmpty());
        QVERIFY(quoteArea->isVisible());
        QVERIFY(quoteText->accessibleName().contains("latest text"));
        QVERIFY(quoteText->toolTip().isEmpty());
        QTest::mouseClick(cancelQuote, Qt::LeftButton);
        QVERIFY(!quoteArea->isVisible());
        QCOMPARE(composer->toPlainText(), "old group draft");
        QCOMPARE(store->items.size(), 1);
        QTRY_VERIFY(heldHistory);
        recalledMessage = {{"id", "recallable"},       {"group", "A"},
                           {"sender", "自己"},         {"text", "confirmed own message"},
                           {"outgoing", true},         {"status", "sent"},
                           {"source_id", "wx-source"}, {"timestamp", QDateTime::currentMSecsSinceEpoch()}};
        recalledMessage["quote"] = QJsonObject{{"id", "latest"}, {"sender", "成员"}, {"text", "latest text"}};
        transport->event({{"type", "message"}, {"message", recalledMessage}});
        const auto staleHistory = QJsonDocument(sourceHistory).toJson(QJsonDocument::Compact);
        heldHistory->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n"
                           "Content-Length: " +
                           QByteArray::number(staleHistory.size()) + "\r\n\r\n" + staleHistory);
        heldHistory->disconnectFromHost();
        QTRY_COMPARE(history->rowCount(), 122);
        QCOMPARE(history->rows.last()["id"].toString(), "recallable");
        view->scrollToBottom();
        QTest::qWait(20);
        QWheelEvent leavingBottom(QPointF(20, 20), view->viewport()->mapToGlobal(QPoint(20, 20)), QPoint(),
                                  QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &leavingBottom);
        QVERIFY(view->scrollAnimationActive());
        transport->event(
            {{"type", "message"},
             {"message", QJsonObject{{"id", "arrived-during-wheel"},
                                     {"group", "A"},
                                     {"seq", 904},
                                     {"text", "arrived while scrolling"},
                                     {"timestamp", recalledMessage["timestamp"].toInteger() - 1}}}});
        QTest::qWait(200);
        QVERIFY(view->verticalScrollBar()->value() < view->verticalScrollBar()->maximum() - 10);
        view->scrollToBottom();
        view->quoteRequested(recalledMessage);
        const auto captureRoot = qEnvironmentVariable("WECHAT_LITE_CAPTURE_UI");
        if (!captureRoot.isEmpty() && QGuiApplication::platformName() == "windows")
        {
            QDir().mkpath(captureRoot);
            QWidget backdrop;
            backdrop.setStyleSheet("background:white;");
            backdrop.setGeometry(60, 60, 960, 820);
            backdrop.show();
            isolated.resize(900, 760);
            isolated.move(90, 90);
            isolated.setWindowFlag(Qt::WindowStaysOnTopHint, true);
            isolated.show();
            isolated.raise();
            isolated.activateWindow();
            QTest::qWait(300);
            isolated.grab().save(QDir(captureRoot).filePath("quote-draft.png"));
            const auto rect =
                QRect(isolated.mapToGlobal(QPoint()), isolated.size()).adjusted(-15, -15, 15, 15);
            isolated.screen()
                ->grabWindow(0, rect.x(), rect.y(), rect.width(), rect.height())
                .save(QDir(captureRoot).filePath("native-frame.png"));
            auto *menu = view->contextMenuFor(history->index(history->rowCount() - 1), false);
            QVERIFY(menu);
            menu->popup(isolated.mapToGlobal(QPoint(500, 200)));
            menu->setActiveAction(menu->actions().first());
            QTest::qWait(40);
            menu->grab().save(QDir(captureRoot).filePath("text-menu.png"));
            delete menu;
        }
        view->recallRequested(recalledMessage);
        QTRY_COMPARE(recalls, 1);
        QTRY_VERIFY(std::any_of(history->rows.cbegin(), history->rows.cend(),
                                [](const QJsonObject &row)
                                {
                                    return row["id"] == "recallable" && row["recall_status"] == "pending" &&
                                           row["text"] == "confirmed own message";
                                }));
        view->recallRequested(recalledMessage);
        QTest::qWait(30);
        QCOMPARE(recalls, 1);
        recalledMessage["status"] = "recalled";
        recalledMessage["recall_status"] = "";
        transport->event({{"type", "message"}, {"message", recalledMessage}});
        QVERIFY(quoteArea->isVisible());
        QVERIFY(quoteText->accessibleName().contains("不可用"));
        QVERIFY(!quoteText->accessibleName().contains("confirmed own message"));
        QVERIFY(quoteText->toolTip().isEmpty());
        view->scrollToBottom();
        QTest::qWait(20);
        const auto recalledIndex = history->index(history->rowCount() - 1);
        QVERIFY(recalledIndex.data(Qt::UserRole).toJsonObject()["_can_reedit"].toBool());
        QStyleOptionViewItem option;
        option.rect = view->visualRect(recalledIndex);
        option.font = view->font();
        const auto editLink = MessageDelegate::reeditRect(option, recalledIndex);
        QVERIFY(!editLink.isEmpty() && view->viewport()->rect().contains(editLink.center()));
        if (!captureRoot.isEmpty() && QGuiApplication::platformName() == "windows")
        {
            if (auto *notice = isolated.findChild<QLabel *>("notice"))
                notice->hide();
            isolated.grab().save(QDir(captureRoot).filePath("recalled-reedit.png"));
        }
        const auto sentBeforeEdit = sent.size();
        QSignalSpy edited(view, &MessageView::reeditRequested);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, editLink.center());
        QCOMPARE(edited.count(), 1);
        QCOMPARE(composer->toPlainText(), "old group draft\nconfirmed own message");
        QCOMPARE(store->items.size(), 1);
        QVERIFY(quoteText->accessibleName().contains("不可用")); // Keep an existing draft's quote.
        QVERIFY(quoteText->toolTip().isEmpty());
        QTest::qWait(40);
        QCOMPARE(sent.size(), sentBeforeEdit);
        QCOMPARE(recalls, 1);
        const auto redacted = recalledIndex.data(Qt::UserRole).toJsonObject();
        QVERIFY(redacted["text"].toString().isEmpty() && redacted["media"].toArray().isEmpty());
        QVERIFY(!redacted["_can_reedit"].toBool());
        const QJsonObject locate{{"id", "missing-source"}, {"group", "A"}};
        view->locateRequested(locate);
        view->locateRequested(locate); // Do not queue duplicate searches while a lookup is pending.
        QTRY_COMPARE(history->rowCount(), 1);
        QCOMPARE(locateRequests, 1);
        QCOMPARE(locatePages, 1);
        QCOMPARE(history->rows.first()["id"].toString(), "missing-source");
        QVERIFY(view->property("locatedHistory").toBool());
        view->locateRequested(locate);
        QCOMPARE(locateRequests, 1); // Already loaded originals need no HTTP call.
        QTest::keyClick(view, Qt::Key_End);
        QTRY_COMPARE(history->rowCount(), 121);
        QVERIFY(!view->property("locatedHistory").toBool());
        locateWrongGroup = true;
        view->locateRequested(locate);
        QTRY_COMPARE(locateRequests, 2);
        QTest::qWait(30);
        QCOMPARE(history->rowCount(), 121);
        QCOMPARE(locatePages, 1); // A cross-group endpoint response is never rendered or paged.
        locateWrongGroup = false;
        holdLocatePage = true;
        view->locateRequested(locate);
        QTRY_VERIFY(locatePageSocket);
        auto recalledOriginal = original;
        recalledOriginal["status"] = "recalled";
        transport->event({{"type", "message"}, {"message", recalledOriginal}});
        locatePageSocket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n"
                                "Content-Length: 2\r\n\r\n[]");
        locatePageSocket->disconnectFromHost();
        QTest::qWait(80);
        QCOMPARE(locateRequests, 3);
        QCOMPARE(locatePages, 2);
        QVERIFY(!view->property("locatedHistory").toBool());
        bool keptRedacted = false;
        for (const auto &row : history->rows)
            if (row["id"] == "missing-source")
                keptRedacted = row["status"] == "recalled" && row["text"].toString().isEmpty();
        QVERIFY(keptRedacted); // A recall arriving between the two GET responses wins.
        view->locateRequested({{"id", "missing-source"}, {"group", "B"}});
        QTest::qWait(30);
        QCOMPARE(locateRequests, 3);
        QCOMPARE(sent.size(), sentBeforeEdit);
        QCOMPARE(recalls, 1);
        transport->disconnectService();
    }
    void downloadedMediaUsesSystemApplicationsWithoutPreviewDialogs()
    {
        FileOpenHandler handler;
        QDesktopServices::setUrlHandler("file", &handler, "open");
        const auto reset = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        auto *transport = window->findChild<Transport *>();
        auto *view = window->findChild<MessageView *>();
        auto *list = window->findChild<QListWidget *>("groups");
        const auto group = list->currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString();
        const auto dialogs = window->findChildren<QDialog *>().size();
        const QStringList kinds{"image", "gif", "video", "file"};
        const QStringList names{"系统打开图片.png", "系统打开动画.gif", "系统打开视频.mp4", "系统打开文档.txt"};
        for (int i = 0; i < kinds.size(); ++i)
        {
            const auto contents = QByteArray("system application handoff ") + kinds[i].toUtf8();
            const auto source = files.filePath(names[i]);
            QFile file(source);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(contents), contents.size());
            file.close();
            QJsonObject media;
            QString failure;
            bool uploaded = false;
            transport->upload(group, source, names[i],
                [&](const QJsonValue &value, const QString &error)
                {
                    media = value.toObject();
                    failure = error;
                    uploaded = true;
                });
            QTRY_VERIFY_WITH_TIMEOUT(uploaded, 8000);
            QVERIFY2(failure.isEmpty(), qPrintable(failure));
            QVERIFY(!media["id"].toString().isEmpty());
            media["kind"] = kinds[i];
            if (i == 0)
            {
                // Saving and automatic thumbnail downloads must never launch an app.
                view->mediaClicked(media, true);
                QTRY_VERIFY(QFile::exists(files.filePath("downloads/" +
                    QDate::currentDate().toString("yyyy-MM-dd") + '/' + names[i])));
                QVERIFY(handler.urls.isEmpty());
            }
            view->mediaClicked(media, false);
            QTRY_COMPARE_WITH_TIMEOUT(handler.urls.size(), i + 1, 8000);
            const auto local = handler.urls.last().toLocalFile();
            QCOMPARE(QFileInfo(local).fileName(), names[i]);
            QCOMPARE(QFileInfo(local).absolutePath(), QDir(files.filePath("downloads/" +
                QDate::currentDate().toString("yyyy-MM-dd"))).absolutePath());
            QFile downloaded(local);
            QVERIFY(downloaded.open(QIODevice::ReadOnly));
            QCOMPARE(downloaded.readAll(), contents);
            QCOMPARE(window->findChildren<QDialog *>().size(), dialogs);
        }
    }
    void voicePlaysInsideChatAndStopsOnClickGroupChangeMinimizeAndRecall()
    {
        FileOpenHandler handler;
        QDesktopServices::setUrlHandler("file", &handler, "open");
        const auto reset = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        auto *transport = window->findChild<Transport *>();
        auto *view = window->findChild<MessageView *>();
        auto *player = window->findChild<VoicePlayer *>();
        auto *list = window->findChild<QListWidget *>("groups");
        QVERIFY(player);
        window->showNormal();
        const auto current = list->currentRow();
        const auto group = list->currentItem()->data(Qt::UserRole).toJsonObject()["id"].toString();
        const auto path = files.filePath("聊天语音.wav");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::LittleEndian);
        const quint32 length = 24000 * 2 * 4;
        stream.writeRawData("RIFF", 4);
        stream << quint32(36 + length);
        stream.writeRawData("WAVEfmt ", 8);
        stream << quint32(16) << quint16(1) << quint16(1) << quint32(24000) << quint32(48000)
               << quint16(2) << quint16(16);
        stream.writeRawData("data", 4);
        stream << length;
        const QByteArray silence(length, 0);
        stream.writeRawData(silence.constData(), silence.size());
        file.close();
        QJsonObject media;
        QString failure;
        bool uploaded = false;
        transport->upload(group, path, "聊天语音.wav", [&](const QJsonValue &value, const QString &error)
        {
            media = value.toObject();
            failure = error;
            uploaded = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(uploaded, 8000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        media["kind"] = "voice";
        media["duration"] = 4;
        const QJsonObject message{{"id", "inline-voice"}, {"seq", 50000}, {"group", group},
            {"timestamp", QDateTime::currentMSecsSinceEpoch()}, {"status", "received"},
            {"sender", "语音测试"}, {"media", QJsonArray{media}}};
        transport->event({{"type", "message"}, {"message", message}});
        const auto dialogs = window->findChildren<QDialog *>().size();
        view->mediaClicked(media, false);
        QTRY_VERIFY_WITH_TIMEOUT(player->isPlaying(), 5000);
        QCOMPARE(view->itemDelegate()->property("voiceState").toString(), "playing");
        QCOMPARE(view->itemDelegate()->property("voiceId").toString(), media["id"].toString());
        const auto capture = qEnvironmentVariable("WECHAT_LITE_CAPTURE_UI");
        if (!capture.isEmpty() && QGuiApplication::platformName() == "windows")
        {
            QDir().mkpath(capture);
            window->grab().save(QDir(capture).filePath("inline-voice.png"));
        }
        view->mediaClicked(media, false);
        QVERIFY(!player->isPlaying());
        QVERIFY(view->itemDelegate()->property("voiceState").toString().isEmpty());
        view->mediaClicked(media, false);
        QTRY_VERIFY(player->isPlaying());
        window->showMinimized();
        QTRY_VERIFY(!player->isPlaying());
        QTest::qWait(100);
        QVERIFY(!player->isPlaying());
        window->showNormal();
        view->mediaClicked(media, false);
        QTRY_VERIFY(player->isPlaying());
        QVERIFY(list->count() > 1);
        list->setCurrentRow((current + 1) % list->count());
        QVERIFY(!player->isPlaying());
        list->setCurrentRow(current);
        transport->event({{"type", "message"}, {"message", message}});
        view->mediaClicked(media, false);
        QTRY_VERIFY(player->isPlaying());
        auto recalled = message;
        recalled["status"] = "recalled";
        recalled["media"] = QJsonArray{};
        transport->event({{"type", "message"}, {"message", recalled}});
        QVERIFY(!player->isPlaying());
        view->mediaClicked(media, true);
        QTest::qWait(30);
        QVERIFY(!player->isPlaying());
        QVERIFY(handler.urls.isEmpty());
        QCOMPARE(window->findChildren<QDialog *>().size(), dialogs);
    }
    void cleanupTestCase()
    {
        if (window)
        {
            auto *transport = window->findChild<Transport *>();
            auto *process = window->findChild<QProcess *>();
            if (transport)
                transport->disconnectService();
            if (transport && process)
                transport->shutdownLocalServer(process);
            if (process)
                QCOMPARE(process->state(), QProcess::NotRunning);
            delete window;
            window = nullptr;
        }
    }
};
QTEST_MAIN(DesktopTests)
#include "desktop_tests.moc"
