#include "composer.h"
#include "announcement_dialog.h"
#include "chat_preferences.h"
#include "connection_info.h"
#include "history_browser.h"
#include "messages.h"
#include "test_tls_identity.h"
#include "tunnel_session.h"
#include "voice_player.h"
#include "window.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QDataStream>
#include <QDesktopServices>
#include <QDateTime>
#include <QDateEdit>
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
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextDocument>
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

// A local read-only API fixture. All payloads and credentials below are synthetic.
class RecordsApiFixture : public QObject
{
  public:
    QTcpServer server;
    QList<QUrl> requests;
    QStringList methods;
    bool clientCredentialOnly = true;
    std::function<void(QTcpSocket *, const QUrl &)> handler;
    explicit RecordsApiFixture(QObject *parent = nullptr) : QObject(parent), server(this)
    {
        connect(&server, &QTcpServer::newConnection, this, [this]
        {
            while (server.hasPendingConnections())
            {
                auto *socket = server.nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes]
                {
                    bytes->append(socket->readAll());
                    if (socket->property("handled").toBool() || !bytes->contains("\r\n\r\n"))
                        return;
                    socket->setProperty("handled", true);
                    const auto first = bytes->left(bytes->indexOf('\n')).split(' ');
                    const QUrl url("http://localhost" + QString::fromLatin1(first.value(1)));
                    methods.append(QString::fromLatin1(first.value(0)));
                    requests.append(url);
                    QByteArray credential;
                    int authorizationHeaders = 0;
                    for (const auto &header : bytes->left(bytes->indexOf("\r\n\r\n")).split('\n'))
                    {
                        const auto colon = header.indexOf(':');
                        if (colon > 0 && header.left(colon).trimmed().compare("authorization", Qt::CaseInsensitive) == 0)
                        {
                            credential = header.mid(colon + 1).trimmed();
                            ++authorizationHeaders;
                        }
                    }
                    clientCredentialOnly &= authorizationHeaders == 1 && credential == "Bearer offline-records-client" &&
                                            !url.path().startsWith("/api/local/");
                    if (handler)
                        handler(socket, url);
                    else
                        respond(socket, QJsonObject{});
                });
            }
        });
    }
    QString url() const { return "http://127.0.0.1:" + QString::number(server.serverPort()); }
    int count(const QString &path) const
    {
        return int(std::count_if(requests.cbegin(), requests.cend(), [&path](const QUrl &url)
            { return url.path() == path; }));
    }
    static void respond(QTcpSocket *socket, const QJsonValue &value)
    {
        if (!socket)
            return;
        const auto body = value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact) :
                                           QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
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
    void chatHasOnlySendHeaderEntriesAndWindowControls()
    {
        const auto buttons = window->findChildren<QPushButton *>();
        QCOMPARE(buttons.size(), 7);
        for (auto *button : buttons)
        {
            QVERIFY(button->objectName() == "sendButton" || button->objectName() == "windowButton" ||
                    button->objectName() == "closeWindow" || button->objectName() == "historyButton" ||
                    button->objectName() == "announcementButton");
            QVERIFY(button->toolTip().isEmpty());
            if (button->objectName() == "historyButton" || button->objectName() == "announcementButton")
            {
                QVERIFY(button->text().isEmpty());
                QCOMPARE(button->accessibleName(), button->objectName() == "historyButton" ?
                    QString("聊天记录") : QString("群公告"));
                QVERIFY(!button->icon().isNull());
                QCOMPARE(button->iconSize(), QSize(20, 20));
                const auto rendered = button->icon().pixmap(QSize(20, 20)).toImage();
                bool visible = false;
                for (int y = 0; y < rendered.height(); ++y)
                    for (int x = 0; x < rendered.width(); ++x)
                        visible |= qAlpha(rendered.pixel(x, y)) != 0;
                QVERIFY(visible); // The SVG is embedded and renders, rather than merely naming a resource.
            }
        }
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
    void recordBrowserPagesFiltersAndLateResponsesStayBounded()
    {
        RecordsApiFixture api;
        QVERIFY(api.server.listen(QHostAddress::LocalHost, 0));
        QPointer<QTcpSocket> held;
        QList<QUrlQuery> queries;
        api.handler = [&](QTcpSocket *socket, const QUrl &url)
        {
            if (url.path().endsWith("/members"))
            {
                RecordsApiFixture::respond(socket, QJsonArray{QJsonObject{{"id", "member-1"}, {"name", "合成成员"}}});
                return;
            }
            QCOMPARE(url.path(), QString("/api/records"));
            const QUrlQuery query(url);
            queries.append(query);
            if (query.queryItemValue("q") == "held")
            {
                held = socket;
                return;
            }
            if (query.queryItemValue("q") == "pending")
            {
                RecordsApiFixture::respond(socket, QJsonObject{{"messages", QJsonArray{}},
                    {"next_before", QJsonValue(QJsonValue::Null)}, {"has_more", true}, {"pending", true}});
                return;
            }
            QJsonArray entries;
            QString next;
            bool hasMore = true;
            if (query.queryItemValue("type") == "file")
            {
                if (!query.hasQueryItem("before"))
                    next = "file-scan-1"; // Empty filtered pages still make progress.
                else
                {
                    entries.append(QJsonObject{{"id", "matched-file"}, {"group", "records-group"},
                        {"sender", "合成成员"}, {"text", ""}, {"timestamp", 2000},
                        {"media", QJsonArray{QJsonObject{{"id", "synthetic-file"}, {"kind", "file"},
                            {"name", "fixture.txt"}, {"size", 120}}}}});
                    hasMore = false;
                }
            }
            else
            {
                const int page = query.queryItemValue("before").toInt();
                for (int i = 0; i < 50; ++i)
                {
                    const auto id = "record-" + QString::number(page * 50 + i);
                    entries.append(QJsonObject{{"id", id}, {"group", "records-group"}, {"sender", "合成成员"},
                        {"sender_id", "member-1"}, {"text", query.queryItemValue("q").isEmpty() ? id : "new filter result"},
                        {"timestamp", 1000000 - page * 50000 - i * 1000}, {"status", "received"}});
                }
                next = QString::number(page + 1);
            }
            RecordsApiFixture::respond(socket, QJsonObject{{"messages", entries}, {"next_before", next},
                {"has_more", hasMore}, {"pending", false}});
        };
        Transport transport;
        transport.configure(api.url(), "offline-records-client");
        auto *browser = new HistoryBrowser(&transport, "records-group", "合成记录群", files.path(), false);
        const auto closeBrowser = qScopeGuard([&] { if (browser) delete browser; });
        browser->show();
        auto *list = browser->findChild<QListView *>("historyRecords");
        auto *more = browser->findChild<QPushButton *>("historyMore");
        auto *search = browser->findChild<QLineEdit *>("historySearch");
        auto *member = browser->findChild<QComboBox *>("historyMember");
        QVERIFY(list && more && search && member);
        QTRY_COMPARE(list->model()->rowCount(), 50);
        QTRY_COMPARE(member->count(), 2);
        QTest::qWait(360);
        QCOMPARE(api.count("/api/records"), 1); // No eager history scan while idle.
        for (int page = 1; page <= 5; ++page)
        {
            QTest::mouseClick(more, Qt::LeftButton);
            QTRY_COMPARE(api.count("/api/records"), page + 1);
            QTRY_VERIFY(more->isEnabled());
        }
        QCOMPARE(list->model()->rowCount(), 200);
        QCOMPARE(list->model()->index(0, 0).data(Qt::UserRole).toJsonObject()["id"].toString(), "record-100");
        QSignalSpy activated(browser, &HistoryBrowser::recordActivated);
        list->setCurrentIndex(list->model()->index(0, 0));
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.first()[0].toJsonObject()["id"].toString(), "record-100");
        for (auto *filter : browser->findChildren<QPushButton *>("historyFilter"))
            if (filter->property("recordType") == "file")
                QTest::mouseClick(filter, Qt::LeftButton);
        QTRY_COMPARE(queries.last().queryItemValue("type"), "file");
        QTRY_VERIFY(more->isEnabled());
        QCOMPARE(list->model()->rowCount(), 0);
        QCOMPARE(more->text(), QString("继续查找"));
        const auto beforeManual = api.count("/api/records");
        QTest::qWait(360);
        QCOMPARE(api.count("/api/records"), beforeManual);
        QTest::mouseClick(more, Qt::LeftButton);
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QVERIFY(!more->isEnabled());
        QCOMPARE(queries.last().queryItemValue("before"), "file-scan-1");
        for (auto *filter : browser->findChildren<QPushButton *>("historyFilter"))
            if (filter->property("recordType") == "all")
                QTest::mouseClick(filter, Qt::LeftButton);
        QTRY_COMPARE(list->model()->rowCount(), 50);
        search->setText("held");
        QTRY_VERIFY(held);
        const auto beforeSearch = api.count("/api/records");
        search->setText("n");
        search->setText("ne");
        search->setText("new");
        QTRY_COMPARE(api.count("/api/records"), beforeSearch + 1);
        QTRY_COMPARE(list->model()->rowCount(), 50);
        RecordsApiFixture::respond(held, QJsonObject{{"messages", QJsonArray{QJsonObject{{"id", "stale"},
            {"group", "records-group"}, {"text", "must not replace newer filters"}}}}, {"has_more", false}});
        QTest::qWait(40);
        QCOMPARE(list->model()->index(0, 0).data(Qt::UserRole).toJsonObject()["text"].toString(), "new filter result");
        QVERIFY(!queries.last().hasQueryItem("before"));
        auto *enabled = browser->findChild<QCheckBox *>("historyDateEnabled");
        auto *day = browser->findChild<QDateEdit *>("historyDate");
        day->setDate(QDate(2026, 4, 5));
        enabled->setChecked(true);
        member->setCurrentIndex(1);
        QTRY_COMPARE(queries.last().queryItemValue("member"), "member-1");
        QCOMPARE(queries.last().queryItemValue("start").toLongLong(), day->date().startOfDay().toMSecsSinceEpoch());
        QCOMPARE(queries.last().queryItemValue("end").toLongLong(), day->date().addDays(1).startOfDay().toMSecsSinceEpoch());
        QVERIFY(!queries.last().hasQueryItem("before"));
        search->setText("pending");
        QTRY_COMPARE(more->text(), QString("继续读取"));
        QVERIFY(more->isEnabled());
        const auto pendingReads = api.count("/api/records");
        QTest::qWait(360);
        QCOMPARE(api.count("/api/records"), pendingReads);
        QTest::mouseClick(more, Qt::LeftButton);
        QTRY_COMPARE(api.count("/api/records"), pendingReads + 1);
        QTRY_VERIFY(more->isEnabled());
        QVERIFY(!queries.last().hasQueryItem("before"));
        QVERIFY(api.clientCredentialOnly);
        for (const auto &method : api.methods)
            QCOMPARE(method, QString("GET"));
        QPointer<HistoryBrowser> closed(browser);
        browser->invalidate();
        browser = nullptr;
        QTRY_VERIFY(closed.isNull());
    }
    void announcementsRemainLiteralReadOnlyAndSeparateUnavailableFromUnset()
    {
        RecordsApiFixture api;
        QVERIFY(api.server.listen(QHostAddress::LocalHost, 0));
        int reply = 0;
        QPointer<QTcpSocket> held;
        const QString plain = "合成公告\nhttps://example.invalid/reference?a=1&b=2\n"
                              "<img src=\"https://example.invalid/pixel\">\nfile:///private/not-a-link";
        api.handler = [&](QTcpSocket *socket, const QUrl &url)
        {
            QCOMPARE(url.path(), QString("/api/groups/announcement-group/announcement"));
            ++reply;
            if (reply == 4)
            {
                held = socket;
                return;
            }
            RecordsApiFixture::respond(socket, QJsonObject{{"group", "announcement-group"},
                {"available", reply > 1}, {"set", reply > 2}, {"author", "合成发布者"},
                {"updated_at", 1700000000000LL}, {"text", reply > 2 ? plain : QString()}});
        };
        Transport transport;
        transport.configure(api.url(), "offline-records-client");
        auto *dialog = new AnnouncementDialog(&transport, "announcement-group", "合成公告群");
        const auto closeDialog = qScopeGuard([&] { if (dialog) delete dialog; });
        dialog->show();
        auto *body = dialog->findChild<QTextBrowser *>("announcementBody");
        auto *retry = dialog->findChild<QPushButton *>("announcementRetry");
        auto *state = dialog->findChild<QLabel *>("announcementState");
        QTRY_COMPARE(dialog->property("announcementState").toString(), "unavailable");
        QVERIFY(state->text().contains("暂不可读"));
        QVERIFY(body->toPlainText().isEmpty());
        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_COMPARE(dialog->property("announcementState").toString(), "unset");
        QVERIFY(state->text().contains("尚未设置"));
        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_COMPARE(dialog->property("announcementState").toString(), "set");
        QCOMPARE(body->toPlainText(), plain);
        QVERIFY(body->isReadOnly());
        QVERIFY(body->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
        QCOMPARE(dialog->findChild<QLabel *>("announcementAuthor")->text(), QString("合成发布者"));
        int images = 0, anchors = 0;
        for (auto block = body->document()->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
            {
                const auto format = it.fragment().charFormat();
                images += format.isImageFormat();
                if (format.isAnchor())
                {
                    ++anchors;
                    QVERIFY(format.anchorHref().startsWith("https://"));
                }
            }
        QCOMPARE(images, 0);
        QVERIFY(anchors > 0);
        FileOpenHandler handler;
        QDesktopServices::setUrlHandler("https", &handler, "open");
        QDesktopServices::setUrlHandler("file", &handler, "open");
        const auto restore = qScopeGuard([]
        {
            QDesktopServices::unsetUrlHandler("https");
            QDesktopServices::unsetUrlHandler("file");
        });
        body->anchorClicked(QUrl("file:///private/not-a-link"));
        QVERIFY(handler.urls.isEmpty());
        body->anchorClicked(QUrl("https://example.invalid/reference?a=1&b=2"));
        QCOMPARE(handler.urls.size(), 1);
        QTest::qWait(60);
        QCOMPARE(api.requests.size(), 3); // Rendering an announcement does not fetch its URLs/images.
        const auto capture = qEnvironmentVariable("WECHAT_LITE_CAPTURE_UI");
        if (!capture.isEmpty() && QGuiApplication::platformName() == "windows")
            dialog->grab().save(QDir(capture).filePath("announcement.png"));
        QTest::mouseClick(retry, Qt::LeftButton);
        QTRY_VERIFY(held);
        QPointer<AnnouncementDialog> closed(dialog);
        dialog->invalidate();
        dialog = nullptr;
        QTRY_VERIFY(closed.isNull());
        RecordsApiFixture::respond(held, QJsonObject{{"group", "announcement-group"}, {"available", true},
            {"set", true}, {"text", "late response must stay closed"}});
        QTest::qWait(40);
        QVERIFY(api.clientCredentialOnly);
    }
    void headerEntriesLocateOriginalsAndInvalidateScopedWindows()
    {
        QSettings settings;
        const QStringList connectionKeys{"serverUrl", "token", "certificatePin", "p2p", "connectionMode"};
        QVariantMap previous;
        for (const auto &key : connectionKeys)
        {
            previous[key] = settings.value(key);
            settings.remove(key);
        }
        const auto restoreSettings = qScopeGuard([&]
        {
            for (const auto &key : connectionKeys)
                if (previous[key].isValid()) settings.setValue(key, previous[key]);
                else settings.remove(key);
        });
        RecordsApiFixture api;
        QVERIFY(api.server.listen(QHostAddress::LocalHost, 0));
        bool allowed = true;
        int sourceReads = 0, contextReads = 0;
        QPointer<QTcpSocket> heldAnnouncement;
        const QJsonObject original{{"id", "original-record"}, {"group", "A"}, {"seq", 7},
            {"timestamp", 1000}, {"sender", "合成成员"}, {"text", "source outside initial page"}, {"status", "received"}};
        api.handler = [&](QTcpSocket *socket, const QUrl &url)
        {
            const auto path = url.path();
            QJsonValue value(QJsonObject{});
            if (path == "/api/status")
                value = QJsonObject{{"permissions", QJsonObject{{"can_send", true}, {"can_download", true}}}};
            else if (path == "/api/groups")
                value = allowed ? QJsonArray{QJsonObject{{"id", "A"}, {"name", "合成群 A"}, {"timestamp", 2000}},
                    QJsonObject{{"id", "B"}, {"name", "合成群 B"}, {"timestamp", 1000}}} : QJsonArray{};
            else if (path.endsWith("/members"))
                value = QJsonArray{};
            else if (path == "/api/history")
            {
                const auto group = QUrlQuery(url).queryItemValue("group");
                value = QJsonObject{{"messages", QJsonArray{QJsonObject{{"id", "latest-" + group}, {"group", group},
                    {"seq", 10}, {"timestamp", 2000}, {"text", "latest fixture"}, {"status", "received"}}}},
                    {"has_more", false}, {"pending", false}};
            }
            else if (path == "/api/records")
                value = QJsonObject{{"messages", QJsonArray{original}}, {"has_more", false}, {"pending", false}};
            else if (path == "/api/messages/original-record")
            {
                ++sourceReads;
                QCOMPARE(QUrlQuery(url).queryItemValue("group"), "A");
                value = original;
            }
            else if (path == "/api/messages")
            {
                ++contextReads;
                QCOMPARE(QUrlQuery(url).queryItemValue("before"), "7");
                value = QJsonArray{};
            }
            else if (path == "/api/groups/A/announcement")
            {
                heldAnnouncement = socket;
                return;
            }
            else if (path == "/api/groups/B/announcement")
                value = QJsonObject{{"group", "B"}, {"available", true}, {"set", false}};
            RecordsApiFixture::respond(socket, value);
        };
        Window isolated(false);
        isolated.show();
        auto *transport = isolated.findChild<Transport *>();
        transport->configure(api.url(), "offline-records-client");
        transport->connectionChanged(true);
        auto *groups = isolated.findChild<QListWidget *>("groups");
        auto *view = isolated.findChild<MessageView *>();
        auto *model = isolated.findChild<MessageModel *>();
        auto *history = isolated.findChild<QPushButton *>("historyButton");
        auto *announcement = isolated.findChild<QPushButton *>("announcementButton");
        QTRY_COMPARE(groups->count(), 2);
        QTRY_VERIFY(view->property("canDownload").toBool());
        QTRY_COMPARE(model->rowCount(), 1);
        QVERIFY(history->isEnabled() && announcement->isEnabled());
        QTest::mouseClick(history, Qt::LeftButton);
        auto *browser = isolated.findChild<HistoryBrowser *>();
        QVERIFY(browser && browser->isVisible());
        auto *records = browser->findChild<QListView *>("historyRecords");
        QTRY_COMPARE(records->model()->rowCount(), 1);
        const auto capture = qEnvironmentVariable("WECHAT_LITE_CAPTURE_UI");
        if (!capture.isEmpty() && QGuiApplication::platformName() == "windows")
        {
            QDir().mkpath(capture);
            browser->grab().save(QDir(capture).filePath("history-browser.png"));
        }
        QTest::mouseClick(records->viewport(), Qt::LeftButton, {},
            records->visualRect(records->model()->index(0, 0)).center());
        QTRY_COMPARE(sourceReads, 1);
        QTRY_COMPARE(contextReads, 1);
        QTRY_COMPARE(model->rows.last()["id"].toString(), "original-record");
        QTRY_COMPARE(view->itemDelegate()->property("locatedId").toString(), "original-record");
        QVERIFY(!browser->isVisible());
        QTest::mouseClick(history, Qt::LeftButton);
        QVERIFY(browser->isVisible());
        QTest::mouseClick(announcement, Qt::LeftButton);
        QPointer<AnnouncementDialog> oldAnnouncement(isolated.findChild<AnnouncementDialog *>());
        QPointer<HistoryBrowser> oldHistory(browser);
        QVERIFY(oldAnnouncement);
        QTRY_VERIFY(heldAnnouncement);
        groups->setCurrentRow(1);
        QTRY_VERIFY(oldAnnouncement.isNull() && oldHistory.isNull());
        RecordsApiFixture::respond(heldAnnouncement, QJsonObject{{"group", "A"}, {"available", true},
            {"set", true}, {"text", "late group A announcement"}});
        QTest::qWait(30);
        QVERIFY(!isolated.findChild<AnnouncementDialog *>());
        QTest::mouseClick(announcement, Qt::LeftButton);
        QPointer<AnnouncementDialog> current(isolated.findChild<AnnouncementDialog *>());
        QVERIFY(current && current->group() == "B");
        QTRY_COMPARE(current->property("announcementState").toString(), "unset");
        allowed = false;
        transport->event({{"type", "groups"}});
        QTRY_COMPARE(groups->count(), 0);
        QTRY_VERIFY(current.isNull());
        QVERIFY(!history->isEnabled() && !announcement->isEnabled());
        QCOMPARE(model->rowCount(), 0);
        QVERIFY(api.clientCredentialOnly);
        for (const auto &method : api.methods)
            QCOMPARE(method, QString("GET"));
        transport->disconnectService();
    }
    void chatPreferencesPersistAndSeparateLogicalServersAndKeys()
    {
        using Mode = ChatPreferences::NotificationMode;
        const auto logical = "p2p://" + QString(64, 'a');
        ChatPreferences preferences;
        QVERIFY(preferences.configure(logical, "offline-client-key-a"));
        QVERIFY(!preferences.pinned("group") && !preferences.muted("group"));
        QCOMPARE(preferences.notificationMode("group"), Mode::AllMessages);
        preferences.setPinned("group", true);
        preferences.setNotificationMode("group", Mode::MentionsOnly);
        preferences.setNotificationMode("ignored", Mode::Ignore);
        QSettings().sync();
        ChatPreferences restarted;
        QVERIFY(restarted.configure(logical, "offline-client-key-a"));
        QVERIFY(restarted.pinned("group") && !restarted.muted("group"));
        QCOMPARE(restarted.notificationMode("group"), Mode::MentionsOnly);
        QCOMPARE(restarted.notificationMode("ignored"), Mode::Ignore);
        QVERIFY(!restarted.configure(logical, "offline-client-key-a"));
        QVERIFY(restarted.configure(logical, "offline-client-key-b"));
        QVERIFY(!restarted.pinned("group") && !restarted.muted("group"));
        QVERIFY(restarted.configure("p2p://" + QString(64, 'b'), "offline-client-key-a"));
        QVERIFY(!restarted.pinned("group") && !restarted.muted("group"));
        QVERIFY(restarted.configure(logical, "offline-client-key-a"));
        restarted.setPinned("group", false);
        QVERIFY(!restarted.pinned("group"));
        QCOMPARE(restarted.notificationMode("group"), Mode::MentionsOnly);
        restarted.setNotificationMode("group", Mode::AllMessages);
        ChatPreferences defaults;
        defaults.configure(logical, "offline-client-key-a");
        QVERIFY(!defaults.pinned("group") && !defaults.muted("group"));
        QCOMPARE(defaults.notificationMode("group"), Mode::AllMessages);
        ChatPreferences legacy;
        legacy.configure(logical, "legacy-notification-key");
        QSettings().setValue(legacy.scope(), QJsonDocument(QJsonObject{
            {"old-muted", QJsonObject{{"muted", true}, {"pinned", true}}},
            {"old-unmuted", QJsonObject{{"muted", false}}},
            {"old-default", QJsonObject{{"pinned", true}}}}).toJson(QJsonDocument::Compact));
        ChatPreferences migrated;
        migrated.configure(logical, "legacy-notification-key");
        QCOMPARE(migrated.notificationMode("old-muted"), Mode::Ignore);
        QVERIFY(migrated.pinned("old-muted"));
        QCOMPARE(migrated.notificationMode("old-unmuted"), Mode::AllMessages);
        QCOMPARE(migrated.notificationMode("old-default"), Mode::AllMessages);
        QVERIFY(migrated.pinned("old-default"));
        QVERIFY(!QSettings().value(migrated.scope()).toByteArray().contains("\"muted\""));
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
        QCOMPARE(menu->actions().size(), 5);
        QCOMPARE(menu->actions().first()->text(), QString("置顶"));
        QCOMPARE(menu->findChild<QAction *>("groupNotificationsIgnore")->text(), QString("忽略通知"));
        QCOMPARE(menu->findChild<QAction *>("groupNotificationsMentions")->text(), QString("仅被 @ 时通知"));
        QCOMPARE(menu->findChild<QAction *>("groupNotificationsAll")->text(), QString("所有新消息通知"));
        QVERIFY(menu->findChild<QAction *>("groupNotificationsAll")->isChecked());
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
        menu->findChild<QAction *>("groupNotificationsIgnore")->trigger();
        delete menu;
        QSignalSpy refreshed(list->model(), &QAbstractItemModel::rowsInserted);
        transport->event({{"type", "groups"}});
        QTRY_VERIFY_WITH_TIMEOUT(!refreshed.isEmpty(), 8000);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toJsonObject().value("id").toString(), target);
        menu = window->groupContextMenuFor(target);
        QVERIFY(menu->findChild<QAction *>("groupNotificationsIgnore")->isChecked());
        menu->resize(menu->sizeHint());
        const auto ignoredMenu = menu->grab().toImage();
        menu->findChild<QAction *>("groupNotificationsMentions")->trigger();
        QVERIFY(menu->findChild<QAction *>("groupNotificationsMentions")->isChecked());
        QVERIFY(!menu->findChild<QAction *>("groupNotificationsIgnore")->isChecked());
        QVERIFY(!menu->findChild<QAction *>("groupNotificationsAll")->isChecked());
        QVERIFY(ignoredMenu != menu->grab().toImage()); // Selection is visible in the custom painted menu.
        delete menu;
        menu = window->groupContextMenuFor(target);
        QVERIFY(menu->findChild<QAction *>("groupNotificationsMentions")->isChecked());
        menu->findChild<QAction *>("groupNotificationsAll")->trigger();
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
        QVERIFY(menu->findChild<QAction *>("groupNotificationsAll")->isChecked());
        menu->findChild<QAction *>("groupNotificationsIgnore")->trigger();
        delete menu;
        const auto muted = event("muted", current);
        transport->event(muted);
        QCOMPARE(notifications.size(), 1);
        bool receivedWhileMuted = false;
        for (const auto &row : window->findChild<MessageModel *>()->rows)
            receivedWhileMuted |= row.value("id") == muted.value("message").toObject().value("id");
        QVERIFY(receivedWhileMuted);
        menu = window->groupContextMenuFor(current);
        QVERIFY(menu->findChild<QAction *>("groupNotificationsIgnore")->isChecked());
        menu->findChild<QAction *>("groupNotificationsAll")->trigger();
        delete menu;
        window->showNormal();
        QApplication::setActiveWindow(window);
        QTRY_VERIFY(window->isActiveWindow());
        transport->event(event("active-current", current));
        QCOMPARE(notifications.size(), 2);
        transport->event(event("active-other", other));
        QCOMPARE(notifications.size(), 3);
        window->hide();
        auto media = event("media", current);
        auto mediaMessage = media.value("message").toObject();
        mediaMessage["text"] = "";
        mediaMessage["media"] = QJsonArray{QJsonObject{{"kind", "gif"}, {"id", "new-gif"}}};
        media["message"] = mediaMessage;
        transport->event(media);
        QCOMPARE(notifications.size(), 4);
        QVERIFY(notifications.last().at(2).toString().contains("[GIF]"));
        transport->connectionChanged(false);
        transport->event(event("disconnected", current));
        transport->connectionChanged(true);
        auto reconnectReplay = event("reconnect-replay", current);
        transport->event(reconnectReplay);
        transport->event({{"type", "synced"}, {"seq", seq}});
        reconnectReplay["seq"] = ++seq;
        transport->event(reconnectReplay);
        QCOMPARE(notifications.size(), 4);
        transport->event(event("after-reconnect", current));
        QCOMPARE(notifications.size(), 5);
        transport->event(event("unknown-group", "not-authorized"));
        QCOMPARE(notifications.size(), 5);
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
    void mentionNotificationsUseStructuredOwnIdentityAndNeverNamesOrText()
    {
        auto *transport = window->findChild<Transport *>();
        auto *list = window->findChild<QListWidget *>("groups");
        QTRY_VERIFY(list->currentItem());
        const auto current = list->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString();
        const auto choose = [this, &current](const QString &action)
        {
            auto *menu = window->groupContextMenuFor(current);
            menu->findChild<QAction *>(action)->trigger();
            delete menu;
        };
        choose("groupNotificationsMentions");
        window->hide();
        transport->event({{"type", "status"}, {"ready", true}, {"self_id", "wxid_own_one"}});
        qint64 seq = 3000000;
        transport->event({{"type", "synced"}, {"seq", seq}});
        QSignalSpy notifications(window, &Window::systemNotificationRequested);
        const auto event = [&seq, &current](const QString &id, const QJsonArray &mentions)
        {
            return QJsonObject{{"type", "message"}, {"seq", ++seq}, {"created", true},
                {"message", QJsonObject{{"id", "mention-notification-" + id}, {"group", current},
                    {"sender_id", "wxid_other"}, {"sender", "桌面测试"},
                    {"text", "@桌面测试 @所有人 wxid_own_one"}, {"mentions", mentions},
                    {"status", "received"}, {"timestamp", 1}}}};
        };
        const auto ownMention = QJsonArray{QJsonObject{{"id", "wxid_own_one"}, {"name", "同名昵称"}}};
        const auto allMention = QJsonArray{QJsonObject{{"id", "notify@all"}, {"name", "所有人"}}};
        transport->event(event("plain", {}));
        transport->event(event("same-name", {QJsonObject{{"id", "someone-else"}, {"name", "桌面测试"}}}));
        transport->event(event("client-id", {QJsonObject{{"id", "fixture-client"}, {"name", "我"}}}));
        transport->event(event("text-all", {QJsonObject{{"id", "@all"}, {"name", "所有人"}}}));
        transport->event(event("malformed", {QJsonValue("wxid_own_one"), QJsonObject{{"id", 1}}}));
        QVERIFY(notifications.isEmpty());
        auto matched = event("self", ownMention);
        transport->event(matched);
        QCOMPARE(notifications.size(), 1);
        matched["seq"] = ++seq;
        transport->event(matched);
        QCOMPARE(notifications.size(), 1); // One creation cannot notify twice on a later receipt.
        transport->event(event("all", allMention));
        QCOMPARE(notifications.size(), 2);
        auto own = event("own-sender-id", ownMention);
        auto body = own["message"].toObject();
        body["sender_id"] = "wxid_own_one";
        own["message"] = body;
        transport->event(own);
        auto historical = event("historical", ownMention);
        historical["historical"] = true;
        transport->event(historical);
        auto update = event("update", ownMention);
        update["created"] = false;
        transport->event(update);
        auto outgoing = event("outgoing", ownMention);
        body = outgoing["message"].toObject();
        body["outgoing"] = true;
        outgoing["message"] = body;
        transport->event(outgoing);
        QCOMPARE(notifications.size(), 2);
        window->showNormal();
        QApplication::setActiveWindow(window);
        QTRY_VERIFY(window->isActiveWindow());
        transport->event(event("active", ownMention));
        QCOMPARE(notifications.size(), 3);
        window->hide();
        choose("groupNotificationsIgnore");
        transport->event(event("ignored-self", ownMention));
        transport->event(event("ignored-all", allMention));
        QCOMPARE(notifications.size(), 3);
        choose("groupNotificationsAll");
        transport->event(event("all-mode-plain", {}));
        QCOMPARE(notifications.size(), 4);
        choose("groupNotificationsMentions");
        transport->event({{"type", "status"}, {"ready", false}, {"self_id", "wxid_own_one"}});
        transport->event(event("disconnected-wechat-identity", ownMention));
        QCOMPARE(notifications.size(), 4);
        transport->event({{"type", "status"}, {"ready", true}});
        transport->event(event("unknown-self", ownMention));
        QCOMPARE(notifications.size(), 4);
        transport->event(event("unknown-all", allMention));
        QCOMPARE(notifications.size(), 5);
        transport->event({{"type", "status"}, {"ready", true}, {"self_id", "wxid_own_two"}});
        transport->event(event("old-identity", ownMention));
        QCOMPARE(notifications.size(), 5);
        const auto newMention = QJsonArray{QJsonObject{{"id", "wxid_own_two"}, {"name", "另一个昵称"}}};
        transport->event(event("new-identity", newMention));
        QCOMPARE(notifications.size(), 6);
        transport->connectionChanged(false);
        transport->event(event("offline", newMention));
        transport->connectionChanged(true);
        transport->event(event("before-sync", newMention));
        transport->event({{"type", "synced"}, {"seq", seq}});
        transport->event(event("no-reused-identity", newMention));
        QCOMPARE(notifications.size(), 6);
        transport->event(event("reconnected-all", allMention));
        QCOMPARE(notifications.size(), 7);
        choose("groupNotificationsAll");
        struct DemoStatus { bool done = false; QJsonObject value; QString error; };
        auto status = std::make_shared<DemoStatus>();
        transport->get("/api/status", [status](const QJsonValue &value, const QString &error)
        {
            status->value = value.toObject(); status->error = error; status->done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(status->done, 8000);
        QVERIFY2(status->error.isEmpty(), qPrintable(status->error));
        status->value.insert("type", "status");
        transport->event(status->value);
        transport->event({{"type", "synced"}, {"seq", 0}});
        window->showNormal();
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
        // A delayed refresh must preserve where the user scrolls while it is pending.
        holdNextHistory = true;
        transport->event({{"type", "reset"}});
        QTRY_VERIFY(heldHistory);
        view->scrollTo(history->index(60), QAbstractItemView::PositionAtTop);
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->value() + 11);
        const auto refreshAnchorIndex = view->indexAt(QPoint(1, 0));
        const auto refreshAnchor = refreshAnchorIndex.data(Qt::UserRole).toJsonObject()["id"].toString();
        const auto refreshOffset = view->visualRect(refreshAnchorIndex).top();
        RecordsApiFixture::respond(heldHistory, QJsonObject{{"messages", sourceHistory},
            {"has_more", true}, {"pending", false}});
        QTRY_VERIFY(view->property("locatedHistory").toBool());
        QTest::qWait(40);
        const auto refreshedIndex = view->indexAt(QPoint(1, 0));
        QCOMPARE(refreshedIndex.data(Qt::UserRole).toJsonObject()["id"].toString(), refreshAnchor);
        QCOMPARE(view->visualRect(refreshedIndex).top(), refreshOffset);
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
