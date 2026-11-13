#include "window.h"
#include "announcement_dialog.h"
#include "composer.h"
#include "connection_info.h"
#include "history_browser.h"
#include "messages.h"
#include "media_storage.h"
#include "preferences_dialog.h"
#include "theme.h"
#include "smooth_scroll.h"
#include "voice_player.h"
#include <QApplication>
#include <QActionGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QCursor>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>
#include <QStandardPaths>
#include <QThreadPool>
#include <QScopeGuard>
#include <QSvgRenderer>
#include <QTcpServer>
#include <QToolButton>
#include <QUrlQuery>
#include <QUuid>
#include <QWindow>
#include <algorithm>
#include <atomic>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <windows.h>
#endif

static QPushButton *button(const QString &text, const QString &name = {})
{
    auto *result = new QPushButton(text);
    result->setObjectName(name);
    result->setCursor(Qt::PointingHandCursor);
    return result;
}
QRect chatWindowBounds(const QRect &available, const QRect &requested)
{
    if (!available.isValid()) return {0, 0, 960, 720};
    const int margin = qMin(12, qMin(available.width(), available.height()) / 20);
    const auto area = available.adjusted(margin, margin, -margin, -margin);
    const QSize minimum(qMin(760, area.width()), qMin(520, area.height()));
    const auto preferred = requested.isValid() ? requested.size() : QSize(1095, 850);
    const auto size = preferred.boundedTo(area.size()).expandedTo(minimum);
    auto position = requested.isValid() ? requested.topLeft() : area.center() - QPoint(size.width() / 2, size.height() / 2);
    position.setX(qBound(area.left(), position.x(), area.right() - size.width() + 1));
    position.setY(qBound(area.top(), position.y(), area.bottom() - size.height() + 1));
    return {position, size};
}
static QFrame *line()
{
    auto *result = new QFrame;
    result->setFrameShape(QFrame::HLine);
    result->setObjectName("divider");
    result->setFixedHeight(1);
    return result;
}
static QIcon appIcon()
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor("#2e986b"));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRect(3, 3, 58, 58), 17, 17);
    p.setBrush(Qt::white);
    p.drawRoundedRect(QRect(14, 17, 36, 27), 10, 10);
    p.drawPolygon(QPolygon{{23, 41}, {23, 51}, {33, 41}});
    p.setBrush(QColor("#2e986b"));
    p.drawEllipse(23, 28, 4, 4);
    p.drawEllipse(37, 28, 4, 4);
    return QIcon(pixmap);
}

static QIcon toolbarIcon(const QString &name)
{
    QPixmap pixmap(22, 22);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(Theme::isDark() ? "#c9cccb" : "#4b4b4b"), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (name == "pin")
    {
        p.drawPolyline(QPolygonF{{8, 5}, {14, 5}, {13, 9}, {15, 12}, {7, 12}, {9, 9}, {8, 5}});
        p.drawLine(11, 12, 11, 18);
    }
    else if (name == "minimize")
        p.drawLine(7, 11, 15, 11);
    else if (name == "maximize")
        p.drawRoundedRect(QRectF(7, 7, 8, 8), .5, .5);
    else if (name == "close")
    {
        p.drawLine(7, 7, 15, 15);
        p.drawLine(15, 7, 7, 15);
    }
    return QIcon(pixmap);
}

static QPushButton *iconButton(const QString &icon, const QString &tip,
                               const QString &objectName = "iconButton")
{
    auto *result = button({}, objectName);
    result->setIcon(toolbarIcon(icon));
    result->setIconSize({22, 22});
    result->setFixedSize(26, 26);
    result->setAccessibleName(tip);
    result->setProperty("toolbarIcon", icon);
    return result;
}

static QIcon svgIcon(const QString &resource)
{
    QFile file(resource);
    file.open(QIODevice::ReadOnly);
    auto bytes = file.readAll();
    if (Theme::isDark()) bytes.replace("#4b4b4b", "#c9cccb");
    QSvgRenderer svg(bytes);
    QIcon icon;
    for (int scale : {1, 2, 3})
    {
        QPixmap pixmap(20 * scale, 20 * scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        svg.render(&painter);
        painter.end();
        pixmap.setDevicePixelRatio(scale);
        icon.addPixmap(pixmap);
    }
    return icon;
}

Window::Window(bool demo, const QString &tunnelExecutable)
    : transport(this), tunnel(this, tunnelExecutable), attachments(this), demoServer(this), tray(this)
{
    connect(this, &QObject::destroyed, [generation = cacheRecoveryGeneration] { ++*generation; });
    demoMode = demo;
#ifdef Q_OS_WIN
    demoServer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments)
                                                 { arguments->flags |= 0x08000000; });
#endif
    QSettings settings;
    serverUrl = settings.value("serverUrl").toString();
    secret = settings.value("token").toString();
    nickname = settings.value("nickname", "我").toString();
    // Store the canonical form of the selected root. ~/Downloads is a symlink onto
    // another volume on many Macs, and every index write rejects aliased paths, so
    // keeping the alias made each download impossible to remember and therefore
    // fetched again on every click.
    const auto configuredDownloads =
        settings
            .value("downloads",
                   QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/微信轻聊")
            .toString();
    downloadRoot = MediaStorage::resolveRoot(configuredDownloads);
    // Write the resolved form back so the setting, the delivered paths and the media
    // index all agree from now on, instead of silently differing on every launch.
    if (downloadRoot != configuredDownloads) settings.setValue("downloads", downloadRoot);
    pin = settings.value("certificatePin").toString();
    p2pSettings = QJsonDocument::fromJson(settings.value("p2p").toByteArray()).object();
    chatPreferences.configure(serverUrl, secret);
    localFiles = std::make_shared<MediaStorage::LocalFiles>(chatPreferences.scope());
    initializeMessageCache();
    clientId = settings.value("clientId").toString();
    if (clientId.isEmpty())
    {
        clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue("clientId", clientId);
    }
    recallDrafts.configure(serverUrl + '\n' + secret + '\n' + clientId);
    setWindowTitle("微信轻聊");
    setWindowIcon(appIcon());
    setWindowFlag(Qt::FramelessWindowHint);
    composeHeightPreference = qBound(0, settings.value("composeHeight", 0).toInt(), 2000);
    setAcceptDrops(true);
    makeUi();
    fitToScreen(true);
    refreshThemeIcons();
    voicePlayer = new VoicePlayer(this);
    connect(voicePlayer, &VoicePlayer::playingChanged, this, [this](bool playing)
    {
        if (!playing && !playingVoiceId.isEmpty())
        {
            if (requestedVoiceId == playingVoiceId)
                requestedVoiceId.clear();
            playingVoiceId.clear();
            playingVoicePath.clear();
            if (requestedVoiceId.isEmpty())
                messages->setVoiceState({}, {});
            updateCachePins();
        }
    });
    connect(voicePlayer, &VoicePlayer::error, this,
        [this](const QString &text) { lastError = text; });
    clearConversation();
    draftTimer.setInterval(400);
    draftTimer.setSingleShot(true);
    connect(&draftTimer, &QTimer::timeout, this, &Window::saveDraft);
    thumbnailTimer.setInterval(180);
    thumbnailTimer.setSingleShot(true);
    connect(&thumbnailTimer, &QTimer::timeout, this, &Window::visibleThumbnails);
    cacheWriteTimer.setSingleShot(true);
    cacheWriteTimer.setInterval(800);
    connect(&cacheWriteTimer, &QTimer::timeout, this, &Window::storeCachedMessages);
    cachePruneTimer.setSingleShot(true);
    cachePruneTimer.setInterval(1000);
    connect(&cachePruneTimer, &QTimer::timeout, this, [this]
    {
        if (downloading.isEmpty()) pruneMediaCache(MediaStorage::cacheLimit());
    });
    connect(composer, &Composer::filesAttached, &attachments, &AttachmentStore::addFiles);
    connect(composer, &Composer::imageAttached, this,
            [this](const QMimeData *mime) { attachments.addImage(mime); });
    connect(&attachments, &AttachmentStore::changed, this,
            [this]
            {
                refreshAttachments();
                if (!restoring)
                {
                    pendingId.clear();
                    draftTimer.start();
                }
            });
    connect(&attachments, &AttachmentStore::warning, this,
            [this](const QString &text) { notify(text, true); });
    // Backup reminder for the case this whole change exists for: the client is configured
    // but nothing connects and no error ever arrives. One banner and one system
    // notification, so a window left in the background still reports the problem.
    startupWatchdog.setSingleShot(true);
    startupWatchdog.setInterval(20000);
    connect(&startupWatchdog, &QTimer::timeout, this,
            [this]
            {
                if (connected || quitting || startupAlerted)
                    return;
                startupAlerted = true;
                const auto reason = lastError.isEmpty()
                                        ? QStringLiteral("连接服务器超时，请检查网络或连接设置。")
                                        : lastError;
                // The transport may already have reported the precise cause; the banner
                // stays useful either way, but the OS notification is only for the silent
                // case so a certificate error is not announced twice.
                if (lastError.isEmpty())
                    setConnectionState(reason, true);
                if (!demoMode && !quitting && !QStandardPaths::isTestModeEnabled())
                    emit statusNotificationRequested(
                        QStringLiteral("微信轻聊 · 无法连接服务器"),
                        reason.size() > 160 ? reason.left(160) + QStringLiteral("…") : reason);
            });
    connect(composer, &QTextEdit::textChanged, this,
            [this]
            {
                if (!restoring)
                {
                    pendingId.clear();
                    draftTimer.start();
                }
                updateSendButton();
            });
    connect(composer, &Composer::sendRequested, this, &Window::send);
    connect(composer, &Composer::scrollRequested, this,
            [this](int direction, bool page) { messages->scrollByKeyboard(direction, page); });
    // Keyboard focus decides which region the arrows move: the editor keeps them for the
    // caret, while the conversation and the group list scroll themselves when focused.
    // Tab walks editor -> conversation -> group list -> editor. QTextEdit would otherwise
    // insert a literal tab and QListView would keep Tab for itself, so the three widgets
    // hand focus to each other explicitly; Escape returns here from the conversation.
    connect(composer, &Composer::focusMoveRequested, this,
            [this](bool backward)
            {
                auto *target = backward ? static_cast<QWidget *>(groups) : static_cast<QWidget *>(messages);
                target->setFocus(Qt::TabFocusReason);
            });
    connect(sendButton, &QPushButton::clicked, this, &Window::send);
    connect(&transport, &Transport::event, this, &Window::handleEvent);
    connect(&transport, &Transport::warning, this, [this](const QString &text) { notify(text, true); });
    connect(&tunnel, &TunnelSession::ready, this, [this](const QString &url)
    {
        if (quitting || QUrl(serverUrl).scheme() != "p2p")
            return;
        transport.configure(url, secret, pin);
        transport.connectEvents();
        loadGroups();
    });
    connect(&tunnel, &TunnelSession::pathChanged, this, [this](const QString &path)
    {
        setProperty("connectionPath", path);
        connection->setText(path == "direct" ? "● 自动连接 · 直连" : "● 自动连接 · 中继");
    });
    connect(&tunnel, &TunnelSession::failed, this, [this](const QString &text)
    {
        if (quitting || QUrl(serverUrl).scheme() != "p2p")
            return;
        connected = false;
        selfMemberId.clear();
        canSend = canDownload = false;
        ++permissionsGeneration;
        transport.configure({}, secret, pin);
        clearConversation();
        subtitle->setText(text);
        notify("自动连接失败：" + text, true);
        setProperty("connectionPath", "failed");
    });
    connect(&transport, &Transport::connectionChanged, this,
            [this](bool value)
            {
                notificationsSynced = false;
                notificationSyncSeq = 0;
                connected = value;
                connection->setText(value ? "● 已连接服务器" : "○ 正在连接服务器…");
                if (value)
                {
                    startupWatchdog.stop();
                    hideBanner();
                    lastError.clear();
                    setWindowTitle("微信轻聊");
                    loadPermissions();
                    loadGroups();
                }
                else
                {
                    storeCachedMessages();
                    selfMemberId.clear();
                    invalidateAuxiliaryWindows();
                    historyButton->setEnabled(false);
                    announcementButton->setEnabled(false);
                    stopVoice();
                    openRequests.clear();
                    saveRequests.clear();
                    pendingMediaCopy = {};
                    mediaQueue.clear();
                    queuedMediaIds.clear();
                    ++permissionsGeneration;
                    canSend = canDownload = false;
                    messages->setProperty("canDownload", false);
                    sendButton->setEnabled(false);
                    ready = false;
                    subtitle->setText("服务器连接已断开，正在自动重连");
                    // Visible in the window list and Mission Control too, so a backgrounded
                    // window still shows that it is not connected.
                    setWindowTitle("微信轻聊 · 未连接");
                    // A transport that cannot succeed (wrong pin, plain http with a pin) stays
                    // disconnected on purpose; do not promise a retry that will not happen.
                    if (transport.isBlocked())
                    {
                        if (lastError.isEmpty())
                            setConnectionState("无法连接服务器，请检查连接设置后重试。", false);
                    }
                    else if (!quitting)
                    {
                        // The reason (timeout, refused, DNS) is more useful than a generic
                        // "reconnecting"; the transport already reported it via warning().
                        if (lastError.isEmpty())
                            setConnectionState("无法连接服务器，正在自动重连。", true);
                        else
                            setConnectionState(lastError, true);
                    }
                }
            });
    connect(&transport, &Transport::progress, this,
            [this](qint64 done, qint64 total)
            {
                transferLabel->setText(
                    total > 0 ? QString("传输 %1% · %2 / %3")
                                    .arg(done * 100 / total)
                                    .arg(AttachmentStore::sizeLabel(done), AttachmentStore::sizeLabel(total))
                              : "正在传输…");
            });
    connect(messages, &MessageView::mediaClicked, this,
            [this](const QJsonObject &media, bool save) { fetchMedia(media, save); });
    connect(messages, &MessageView::mediaCopyRequested, this, &Window::copyMedia);
    connect(messages, &MessageView::recordClicked, this, &Window::showForwardRecord);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            [this] { pendingMediaCopy = {}; });
    connect(messages, &MessageView::mentionRequested, this,
            [this](QJsonObject member)
            {
                if (busy || groupId.isEmpty())
                    return;
                for (const auto &entry : memberCache)
                    if (entry.toObject()["id"] == member["id"])
                    {
                        member = entry.toObject();
                        break;
                    }
                composer->insertMention(member);
            });
    connect(messages, &MessageView::quoteRequested, this, &Window::quoteMessage);
    connect(messages, &MessageView::recallRequested, this, &Window::recallMessage);
    connect(messages, &MessageView::reeditRequested, this, &Window::reeditMessage);
    connect(messages, &MessageView::locateRequested, this, &Window::locateQuote);
    connect(messages, &MessageView::latestRequested, this,
            [this]
            {
                messages->stopScrollAnimation();
                messages->scrollToBottom();
                ++historyGeneration;
                loading = false;
                historyUpdates.clear();
                messages->setProperty("locatedHistory", false);
                loadHistory();
            });
    connect(messages, &MessageView::viewportChanged, this, [this]
        { updateCachePins(); thumbnailTimer.start(); });
    connect(messages, &MessageView::editorFocusRequested, this, [this] { composer->setFocus(); });
    connect(model, &QAbstractItemModel::modelReset, this, &Window::updateCachePins);
    connect(model, &QAbstractItemModel::dataChanged, this, &Window::updateCachePins);
    connect(model, &QAbstractItemModel::modelReset, this, &Window::refreshForwardRecord);
    connect(model, &QAbstractItemModel::dataChanged, this, &Window::refreshForwardRecord);
    // Everything that changes the list also refreshes the snapshot opened with, but
    // coalesced: a burst of live messages must not rewrite the file per row.
    connect(model, &QAbstractItemModel::modelReset, this, &Window::scheduleCacheWrite);
    connect(model, &QAbstractItemModel::dataChanged, this, &Window::scheduleCacheWrite);
    connect(model, &QAbstractItemModel::rowsInserted, this, &Window::scheduleCacheWrite);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &Window::scheduleCacheWrite);
    connect(&demoServer, &QProcess::readyReadStandardError, this,
            [this]
            {
                auto text = QString::fromUtf8(demoServer.readAllStandardError()).trimmed();
                if (!text.isEmpty())
                {
                    demoFailure = (demoFailure + "\n" + text).right(2000);
                    notify(text.left(400), true);
                }
            });
    connect(&demoServer, &QProcess::finished, this,
            [this](int code)
            {
                if (!quitting && code != 0)
                    notify(demoFailure.isEmpty() ? "演示服务已退出"
                                                 : "演示服务已退出：" + demoFailure.trimmed().left(500),
                           true);
            });
    tray.setIcon(appIcon());
    auto *menu = new QMenu(this);
    menu->addAction("打开微信轻聊", this,
                    [this]
                    {
                        showNormal();
                        raise();
                        activateWindow();
                    });
    menu->addAction("连接与下载设置", this, &Window::showSettings);
    menu->addAction("设置", this, [this] { showPreferences(); });
    menu->addSeparator();
    menu->addAction("退出客户端", this,
                    [this]
                    {
                        quitting = true;
                        QApplication::quit();
                    });
    tray.setContextMenu(menu);
    if (QSystemTrayIcon::isSystemTrayAvailable())
        tray.show();
    connect(&tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason)
            {
                if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
                {
                    showNormal();
                    raise();
                    activateWindow();
                }
            });
    connect(qApp, &QApplication::aboutToQuit, this,
            [this]
            {
                quitting = true;
                saveDraft();
                storeCachedMessages();
                transport.disconnectService();
                tunnel.stop();
                if (demoServer.state() != QProcess::NotRunning)
                    transport.shutdownLocalServer(&demoServer);
            });
    if (demo)
        QTimer::singleShot(0, this, &Window::startDemo);
    else if (!serverUrl.isEmpty() && secret.size() >= 24)
    {
        startupWatchdog.start();
        QTimer::singleShot(0, this, &Window::connectService);
    }
}

void Window::makeUi()
{
    auto *central = new QWidget;
    setCentralWidget(central);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *titleBar = new QFrame;
    titleBar->setObjectName("titleBar");
    titleBar->setFixedHeight(27);
    titleBar->installEventFilter(this);
    auto *windowControls = new QHBoxLayout(titleBar);
    windowControls->setContentsMargins(15, 0, 0, 0);
    windowControls->setSpacing(0);
    demoLabel = new QLabel;
    demoLabel->setObjectName("demoLabel");
    demoLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    windowControls->addWidget(demoLabel);
    windowControls->addStretch();
    auto *pinButton = iconButton("pin", "窗口置顶", "windowButton");
    pinButton->setCheckable(true);
    auto *minimize = iconButton("minimize", "最小化", "windowButton");
    auto *maximize = iconButton("maximize", "最大化或还原", "windowButton");
    auto *close = iconButton("close", "关闭", "closeWindow");
    for (auto *control : {pinButton, minimize, maximize, close})
    {
        control->setFixedSize(44, 27);
        windowControls->addWidget(control);
    }
    connect(pinButton, &QPushButton::toggled, this,
            [this](bool checked)
            {
                setWindowFlag(Qt::WindowStaysOnTopHint, checked);
                show();
            });
    connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(maximize, &QPushButton::clicked, this,
            [this] { isMaximized() ? showNormal() : showMaximized(); });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    outer->addWidget(titleBar);

    auto *splitter = new QSplitter;
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    outer->addWidget(splitter, 1);
    auto *side = new QFrame;
    side->setObjectName("sidebar");
    side->setMinimumWidth(250);
    side->setMaximumWidth(420);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);
    auto *sideTop = new QWidget;
    sideTop->setFixedHeight(47);
    auto *sideTopLayout = new QHBoxLayout(sideTop);
    sideTopLayout->setContentsMargins(15, 10, 13, 9);
    sideTopLayout->setSpacing(12);
    preferencesButton = button({}, "preferencesButton");
    preferencesButton->setAccessibleName("设置");
    preferencesButton->setFixedSize(26, 26);
    preferencesButton->setIconSize({20, 20});
    preferencesButton->setIcon(svgIcon(":/client/icons/settings.svg"));
    connect(preferencesButton, &QPushButton::clicked, this, [this] { showPreferences(); });
    sideTopLayout->addWidget(preferencesButton);
    groupSearch = new QLineEdit;
    groupSearch->setObjectName("search");
    groupSearch->setPlaceholderText("⌕ 搜索");
    groupSearch->setAlignment(Qt::AlignCenter);
    groupSearch->setFixedHeight(28);
    groupSearch->setClearButtonEnabled(true);
    sideTopLayout->addWidget(groupSearch, 1);
    sideLayout->addWidget(sideTop);
    groups = new QListWidget;
    groups->setObjectName("groups");
    groups->setItemDelegate(new GroupDelegate(groups));
    groups->setMouseTracking(true);
    groups->setFrameShape(QFrame::NoFrame);
    groups->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    groups->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    groups->setSpacing(0);
    groups->setContextMenuPolicy(Qt::CustomContextMenu);
    // Make a click in the list establish it as the keyboard target, so its arrows move the
    // highlighted conversation and scroll the list instead of editing a draft the user is
    // no longer looking at. The filter also closes the Tab loop back to the editor.
    groups->viewport()->installEventFilter(this);
    groups->installEventFilter(this);
    new SmoothScroll(groups);
    sideLayout->addWidget(groups, 1);
    connect(groups, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position)
    {
        auto *item = groups->itemAt(position);
        if (!item && position.x() < 0 && position.y() < 0)
            item = groups->currentItem();
        if (!item)
            return;
        auto *menu = groupContextMenuFor(item->data(Qt::UserRole).toJsonObject().value("id").toString());
        if (!menu)
            return;
        execChatContextMenu(menu, groups->viewport()->mapToGlobal(position));
        delete menu;
    });
    connect(groupSearch, &QLineEdit::textChanged, this,
            [this](const QString &query)
            {
                groupSearch->setAlignment(query.isEmpty() ? Qt::AlignCenter : Qt::AlignLeft);
                for (int i = 0; i < groups->count(); ++i)
                {
                    auto *item = groups->item(i);
                    item->setHidden(!item->data(Qt::UserRole)
                                         .toJsonObject()["name"]
                                         .toString()
                                         .contains(query, Qt::CaseInsensitive));
                }
            });
    connect(groups, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current)
            {
                if (current)
                {
                    auto obj = current->data(Qt::UserRole).toJsonObject();
                    selectGroup(obj["id"].toString(), obj["name"].toString());
                }
            });
    connection = new QLabel("○ 尚未连接", side);
    connection->hide();
    splitter->addWidget(side);

    auto *main = new QFrame;
    main->setObjectName("main");
    auto *layout = new QVBoxLayout(main);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *header = new QWidget;
    header->setFixedHeight(47);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 0, 17, 0);
    headerLayout->setSpacing(12);
    title = new QLabel("微信轻聊");
    title->setObjectName("chatTitle");
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerLayout->addWidget(title, 1);
    historyButton = button({}, "historyButton");
    historyButton->setAccessibleName("聊天记录");
    historyButton->setIcon(svgIcon(":/client/icons/history.svg"));
    announcementButton = button({}, "announcementButton");
    announcementButton->setAccessibleName("群公告");
    announcementButton->setIcon(svgIcon(":/client/icons/announcement.svg"));
    for (auto *entry : {historyButton, announcementButton})
    {
        entry->setFixedSize(26, 26);
        entry->setIconSize({20, 20});
        entry->setStyleSheet(
            "QPushButton { background:transparent; border:0; border-radius:3px; padding:0; }"
            "QPushButton:hover { background:#eeeef0; }"
            "QPushButton:disabled { color:#b0b0b0; }");
        headerLayout->addWidget(entry);
    }
    connect(historyButton, &QPushButton::clicked, this, &Window::showHistoryBrowser);
    connect(announcementButton, &QPushButton::clicked, this, &Window::showAnnouncement);
    subtitle = new QLabel(header);
    subtitle->hide();
    layout->addWidget(header);
    layout->addWidget(line());
    model = new MessageModel(this);
    messages = new MessageView;
    messages->setModel(model);
    messages->installEventFilter(this);
    layout->addWidget(messages, 1);
    connect(messages, &MessageView::olderRequested, this, [this] { loadHistory(true); });

    composeResizeHandle = new QWidget;
    composeResizeHandle->setObjectName("composeResizeHandle");
    composeResizeHandle->setFixedHeight(5);
    composeResizeHandle->setCursor(Qt::SizeVerCursor);
    composeResizeHandle->setAccessibleName("调整输入框高度");
    composeResizeHandle->installEventFilter(this);
    layout->addWidget(composeResizeHandle);
    auto *compose = new QWidget;
    compose->setObjectName("composeArea");
    compose->setFixedHeight(303);
    auto *composeOuter = new QVBoxLayout(compose);
    composeOuter->setContentsMargins(7, 1, 11, 12);
    composeOuter->setSpacing(0);
    auto *composeFrame = new QFrame;
    composeFrame->setObjectName("composeFrame");
    auto *composeLayout = new QVBoxLayout(composeFrame);
    composeLayout->setContentsMargins(12, 10, 11, 8);
    composeLayout->setSpacing(8);
    attachmentArea = new QScrollArea;
    attachmentArea->setFrameShape(QFrame::NoFrame);
    attachmentArea->setWidgetResizable(true);
    attachmentArea->setFixedHeight(84);
    attachmentArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *attachmentWidget = new QWidget;
    attachmentLayout = new QHBoxLayout(attachmentWidget);
    attachmentLayout->setContentsMargins(0, 0, 0, 3);
    attachmentLayout->setAlignment(Qt::AlignLeft);
    attachmentArea->setWidget(attachmentWidget);
    attachmentArea->hide();
    composeLayout->addWidget(attachmentArea);
    quoteArea = new QFrame;
    quoteArea->setObjectName("quotePreview");
    quoteArea->setFixedHeight(18);
    quoteArea->setStyleSheet(
        "#quotePreview { background:transparent; border-left:2px solid #e2e2e2; }"
        "#quoteText { color:#9f9fa6; font-size:12px; }"
        "#cancelQuote { background:#d9d9d9; border:0; color:white; font-size:12px; border-radius:7px; }"
        "#cancelQuote:hover { background:#bcbcbc; }");
    auto *quoteLayout = new QHBoxLayout(quoteArea);
    quoteLayout->setContentsMargins(10, 0, 0, 0);
    quoteLayout->setSpacing(8);
    quoteLabel = new QLabel;
    quoteLabel->setObjectName("quoteText");
    quoteLabel->setTextFormat(Qt::PlainText);
    quoteLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    quoteLayout->addWidget(quoteLabel);
    cancelQuoteButton = new QToolButton;
    cancelQuoteButton->setObjectName("cancelQuote");
    cancelQuoteButton->setText("×");
    cancelQuoteButton->setAccessibleName("取消引用");
    cancelQuoteButton->setAccessibleName("取消引用");
    cancelQuoteButton->setFixedSize(14, 14);
    quoteLayout->addWidget(cancelQuoteButton);
    quoteLayout->addStretch(1);
    connect(cancelQuoteButton, &QToolButton::clicked, this, [this] { quoteMessage({}); });
    quoteArea->hide();
    composer = new Composer;
    composer->setPlaceholderText({});
    composer->setMinimumHeight(26);
    composeLayout->addWidget(composer, 1);
    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(10);
    auto *footerLeft = new QWidget;
    auto *footerLeftLayout = new QHBoxLayout(footerLeft);
    footerLeftLayout->setContentsMargins(0, 0, 0, 0);
    footerLeftLayout->addWidget(quoteArea, 1, Qt::AlignVCenter);
    attachmentCount = new QLabel;
    attachmentCount->setObjectName("muted");
    footerLeftLayout->addWidget(attachmentCount);
    toolbar->addWidget(footerLeft, 1);
    transferLabel = new QLabel;
    transferLabel->setObjectName("muted");
    toolbar->addWidget(transferLabel);
    sendButton = button("发送", "sendButton");
    sendButton->setEnabled(false);
    sendButton->setFixedSize(48, 26);
    toolbar->addWidget(sendButton);
    composeLayout->addLayout(toolbar);
    composeOuter->addWidget(composeFrame);
    layout->addWidget(compose);
    splitter->addWidget(main);
    splitter->setSizes({323, 771});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    const QList<Qt::Edges> edges{Qt::TopEdge,
                                 Qt::BottomEdge,
                                 Qt::LeftEdge,
                                 Qt::RightEdge,
                                 Qt::TopEdge | Qt::LeftEdge,
                                 Qt::TopEdge | Qt::RightEdge,
                                 Qt::BottomEdge | Qt::LeftEdge,
                                 Qt::BottomEdge | Qt::RightEdge};
    for (auto edge : edges)
    {
        auto *handle = new QWidget(this);
        handle->setProperty("resizeEdge", int(edge));
        handle->setCursor(edge == (Qt::TopEdge | Qt::LeftEdge) || edge == (Qt::BottomEdge | Qt::RightEdge)
                              ? Qt::SizeFDiagCursor
                          : edge == (Qt::TopEdge | Qt::RightEdge) || edge == (Qt::BottomEdge | Qt::LeftEdge)
                              ? Qt::SizeBDiagCursor
                          : edge.testFlag(Qt::TopEdge) || edge.testFlag(Qt::BottomEdge) ? Qt::SizeVerCursor
                                                                                        : Qt::SizeHorCursor);
        handle->installEventFilter(this);
        resizeEdges.append(handle);
    }
    // Overlay banner: raised above the content and shown only when there is something the
    // user must know (unreachable server, rejected certificate, lost link). Keeping it out
    // of the layout means showing or hiding it never moves the compose box.
    banner = new QFrame(this);
    banner->setObjectName("statusBanner");
    banner->hide();
    auto *bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(12, 8, 8, 8);
    bannerLayout->setSpacing(10);
    bannerLabel = new QLabel(banner);
    bannerLabel->setObjectName("statusBannerText");
    bannerLabel->setTextFormat(Qt::PlainText);
    bannerLabel->setWordWrap(true);
    bannerLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bannerLayout->addWidget(bannerLabel, 1);
    // QToolButton rather than QPushButton: the window's action buttons are deliberately
    // limited to the chat/header set, and these two belong to the transient strip.
    bannerAction = new QToolButton(banner);
    bannerAction->setObjectName("statusBannerAction");
    bannerAction->setCursor(Qt::PointingHandCursor);
    bannerAction->setAutoRaise(true);
    bannerAction->hide();
    bannerLayout->addWidget(bannerAction, 0, Qt::AlignVCenter);
    // While retrying, the primary action stays "重试"; this secondary one guarantees a
    // way to correct the address without hunting through the tray menu.
    bannerRetrySettings = new QToolButton(banner);
    bannerRetrySettings->setObjectName("statusBannerSettings");
    bannerRetrySettings->setText("连接设置");
    bannerRetrySettings->setCursor(Qt::PointingHandCursor);
    bannerRetrySettings->setAutoRaise(true);
    bannerRetrySettings->hide();
    bannerLayout->addWidget(bannerRetrySettings, 0, Qt::AlignVCenter);
    auto *bannerClose = new QToolButton(banner);
    bannerClose->setObjectName("statusBannerClose");
    bannerClose->setText("×");
    bannerClose->setAccessibleName("关闭提示");
    bannerClose->setCursor(Qt::PointingHandCursor);
    bannerClose->setAutoRaise(true);
    bannerClose->setFixedSize(18, 18);
    bannerLayout->addWidget(bannerClose, 0, Qt::AlignVCenter);
    connect(bannerClose, &QToolButton::clicked, this, &Window::hideBanner);
    connect(bannerAction, &QToolButton::clicked, this,
            [this]
            {
                // "retry" retries once; "settings" opens the connection page so a
                // wrong address can be corrected instead of retried forever.
                if (bannerAction->property("action").toString() == "retry")
                    transport.reconnectNow();
                else
                    showSettings();
            });
    connect(bannerRetrySettings, &QToolButton::clicked, this, &Window::showSettings);
}

bool Window::eventFilter(QObject *watched, QEvent *event)
{
    if (groups && watched == groups->viewport() && event->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
    {
        groupSelectionFromKeyboard = false;
        groups->setFocus(Qt::MouseFocusReason);
    }
    if (groups && watched == groups && event->type() == QEvent::FocusOut)
        groupSelectionFromKeyboard = false;
    // The filter is installed while makeUi() is still building the compose area, so the
    // editor and conversation must be checked before use; early focus events do arrive.
    if (messages && watched == messages && event->type() == QEvent::KeyPress)
    {
        // Neither list would advance focus with Tab on its own, so complete the loop here.
        // Escape also returns to the editor, so typing never requires the mouse again.
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab)
        {
            groups->setFocus(Qt::TabFocusReason);
            return true;
        }
        if (key->key() == Qt::Key_Backtab)
        {
            composer->setFocus(Qt::TabFocusReason);
            return true;
        }
    }
    if (groups && composer && messages && watched == groups && event->type() == QEvent::KeyPress)
    {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab)
        {
            composer->setFocus(Qt::TabFocusReason);
            return true;
        }
        if (key->key() == Qt::Key_Backtab)
        {
            messages->setFocus(Qt::TabFocusReason);
            return true;
        }
        if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down || key->key() == Qt::Key_PageUp ||
            key->key() == Qt::Key_PageDown || key->key() == Qt::Key_Home || key->key() == Qt::Key_End)
            groupSelectionFromKeyboard = true;
        if (key->key() == Qt::Key_Escape)
        {
            composer->setFocus(Qt::OtherFocusReason);
            return true;
        }
        // Up/Down keep the list's own meaning: move the highlighted conversation, which
        // also scrolls it into view. That is exactly the focus rule used elsewhere.
    }
    if (watched == composeResizeHandle)
    {
        if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
        {
            composeDragging = true;
            composeDragStart = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
            composeDragHeight = findChild<QWidget *>("composeArea")->height();
            return true;
        }
        if (event->type() == QEvent::MouseMove && composeDragging)
        {
            composeHeightPreference = qMax(1, composeDragHeight + composeDragStart.y() -
                static_cast<QMouseEvent *>(event)->globalPosition().toPoint().y());
            updateComposeHeight();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && composeDragging)
        {
            composeDragging = false;
            composeHeightPreference = findChild<QWidget *>("composeArea")->height();
            QSettings().setValue("composeHeight", composeHeightPreference);
            return true;
        }
    }
    if (watched->objectName() == "titleBar")
    {
        if (event->type() == QEvent::MouseButtonDblClick)
        {
            isMaximized() ? showNormal() : showMaximized();
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton && windowHandle())
        {
            windowHandle()->startSystemMove();
            return true;
        }
    }
    if (watched->property("resizeEdge").isValid() && event->type() == QEvent::MouseButtonPress &&
        !isMaximized() && windowHandle())
    {
        windowHandle()->startSystemResize(Qt::Edges(watched->property("resizeEdge").toInt()));
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}
void Window::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateComposeHeight();
    refreshQuote();
    const int w = width(), h = height(), edge = 4;
    const QList<QRect> rects{{edge, 0, w - edge * 2, edge},
                             {edge, h - edge, w - edge * 2, edge},
                             {0, edge, edge, h - edge * 2},
                             {w - edge, edge, edge, h - edge * 2},
                             {0, 0, edge, edge},
                             {w - edge, 0, edge, edge},
                             {0, h - edge, edge, edge},
                             {w - edge, h - edge, edge, edge}};
    for (int i = 0; i < resizeEdges.size(); ++i)
    {
        resizeEdges[i]->setGeometry(rects[i]);
        resizeEdges[i]->setVisible(!isMaximized());
        resizeEdges[i]->raise();
    }
    if (banner && banner->isVisible())
        positionBanner();
}
void Window::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (!screenWatching && windowHandle())
    {
        screenWatching = true;
        connect(windowHandle(), &QWindow::screenChanged, this,
            [this] { QTimer::singleShot(0, this, [this] { fitToScreen(); }); });
        for (auto *screen : QGuiApplication::screens()) watchScreen(screen);
        connect(qApp, &QGuiApplication::screenAdded, this, &Window::watchScreen);
    }
    fitToScreen();
    updateNativeFrame();
}
void Window::watchScreen(QScreen *screen)
{
    const auto update = [this, screen]
    {
        if (windowHandle() && windowHandle()->screen() == screen)
            QTimer::singleShot(0, this, [this] { fitToScreen(); });
    };
    connect(screen, &QScreen::availableGeometryChanged, this, update);
    connect(screen, &QScreen::logicalDotsPerInchChanged, this, update);
}
void Window::fitToScreen(bool initial)
{
    if (isMinimized()) return;
    const auto requested = initial ? QSettings().value("windowBounds").toRect() : geometry();
    auto *target = initial && requested.isValid() ? QGuiApplication::screenAt(requested.center()) : nullptr;
    if (!target && !initial && windowHandle()) target = windowHandle()->screen();
    if (!target) target = QGuiApplication::screenAt(QCursor::pos());
    if (!target) target = QGuiApplication::primaryScreen();
    if (!target) return;
    // Qt screen geometry is already expressed in logical pixels, including system scaling.
    const auto available = target->availableGeometry();
    const auto fitted = chatWindowBounds(available, requested);
    setMinimumSize(qMax(1, qMin(760, available.width() - 24)), qMax(1, qMin(520, available.height() - 24)));
    const int pixels = available.height() < 800 || available.width() < 1100 ? 12 : 14;
    if (property("contentPixelSize").toInt() != pixels)
    {
        setProperty("contentPixelSize", pixels);
        auto density = font(); density.setPixelSize(pixels); setFont(density);
        composer->setStyleSheet(QString("font-size:%1px;").arg(pixels));
        title->setStyleSheet(QString("font-size:%1px;").arg(pixels));
    }
    if (!isMaximized() && geometry() != fitted) setGeometry(fitted);
    updateComposeHeight();
}
void Window::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
    {
        refreshThemeIcons();
        updateNativeFrame();
    }
    if (event->type() == QEvent::WindowStateChange && isMinimized())
        stopVoice();
    if (event->type() == QEvent::WindowStateChange && !isMinimized())
        QTimer::singleShot(0, this, [this] { fitToScreen(); });
    if (event->type() == QEvent::ActivationChange || event->type() == QEvent::WindowStateChange)
        updateNativeFrame();
    // Messages that arrived while the window sat in the background were counted as unread even
    // for the open conversation, so clear that conversation now that the user is looking at it.
    if (event->type() == QEvent::ActivationChange && isActiveWindow())
        markConversationRead();
}

void Window::markConversationRead()
{
    if (groupId.isEmpty() || !unread.contains(groupId))
        return;
    unread.remove(groupId);
    unreadMentions.remove(groupId);
    updateUnreadTotal();
    for (int row = 0; row < groups->count(); ++row)
    {
        auto *item = groups->item(row);
        auto group = item->data(Qt::UserRole).toJsonObject();
        if (group.value("id").toString() != groupId || group.value("unread").toInt() == 0)
            continue;
        group["unread"] = 0;
        item->setData(Qt::UserRole, group);
        break;
    }
}
void Window::refreshThemeIcons()
{
    if (!preferencesButton || !historyButton || !announcementButton) return;
    preferencesButton->setIcon(svgIcon(":/client/icons/settings.svg"));
    historyButton->setIcon(svgIcon(":/client/icons/history.svg"));
    announcementButton->setIcon(svgIcon(":/client/icons/announcement.svg"));
    const auto hover = Theme::isDark() ? "#393b3c" : "#eeeef0";
    for (auto *entry : {preferencesButton, historyButton, announcementButton})
        entry->setStyleSheet(QString("QPushButton {background:transparent;border:0;border-radius:3px;padding:0;}"
            "QPushButton:hover {background:%1;}").arg(hover));
    for (auto *entry : findChildren<QPushButton *>())
        if (!entry->property("toolbarIcon").toString().isEmpty())
            entry->setIcon(toolbarIcon(entry->property("toolbarIcon").toString()));
    if (quoteArea)
        quoteArea->setStyleSheet(Theme::isDark() ?
            "#quotePreview {background:transparent;border-left:2px solid #545658;}"
            "#quoteText {color:#a8aaad;font-size:12px;}"
            "#cancelQuote {background:#55585a;border:0;color:#dddddd;font-size:12px;border-radius:7px;}"
            "#cancelQuote:hover {background:#707477;}" :
            "#quotePreview {background:transparent;border-left:2px solid #e2e2e2;}"
            "#quoteText {color:#9f9fa6;font-size:12px;}"
            "#cancelQuote {background:#d9d9d9;border:0;color:white;font-size:12px;border-radius:7px;}"
            "#cancelQuote:hover {background:#bcbcbc;}");
}
void Window::updateNativeFrame()
{
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != "windows" || !testAttribute(Qt::WA_WState_Created))
        return;
    const auto hwnd = reinterpret_cast<HWND>(winId());
    const auto oldStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const auto style = oldStyle | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    if (style != oldStyle)
    {
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    const DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    const COLORREF border = Theme::isDark() ? RGB(80, 82, 82) :
        (isActiveWindow() ? RGB(170, 170, 170) : RGB(190, 190, 190));
    // Numeric IDs keep this compatible with SDKs that predate Windows 11's frame attributes.
    const auto borderResult = DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));
    const DWORD corners = isMaximized() ? 1 : 3; // Do not round maximized windows; use small normal corners.
    DwmSetWindowAttribute(hwnd, 33, &corners, sizeof(corners));
    const MARGINS shadow = isMaximized() ? MARGINS{0, 0, 0, 0} : MARGINS{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &shadow);
    if (centralWidget() && centralWidget()->layout())
    {
        const int fallback = FAILED(borderResult) && !isMaximized() ? 1 : 0;
        centralWidget()->layout()->setContentsMargins(fallback, fallback, fallback, fallback);
        if (fallback)
        {
            centralWidget()->setAutoFillBackground(true);
            auto colors = centralWidget()->palette();
            colors.setColor(QPalette::Window, isActiveWindow() ? QColor("#aaaaaa") : QColor("#bebebe"));
            centralWidget()->setPalette(colors);
        }
    }
#endif
}
bool Window::nativeEvent(const QByteArray &type, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    const auto native = static_cast<MSG *>(message);
    if (native->message == WM_NCCALCSIZE && native->wParam)
    {
        auto *rect = &reinterpret_cast<NCCALCSIZE_PARAMS *>(native->lParam)->rgrc[0];
        if (IsZoomed(native->hwnd))
        {
            const auto dpi = GetDpiForWindow(native->hwnd);
            const int horizontal =
                GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int vertical =
                GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            rect->left += horizontal;
            rect->right -= horizontal;
            rect->top += vertical;
            rect->bottom -= vertical;
        }
        *result = 0;
        return true;
    }
#endif
    return QMainWindow::nativeEvent(type, message, result);
}

void Window::showPreferences(PreferencesPage page)
{
    if (preferencesDialog)
    {
        preferencesDialog->selectPage(static_cast<int>(page));
        preferencesDialog->show(); preferencesDialog->raise(); preferencesDialog->activateWindow();
        return;
    }
    invalidateAuxiliaryWindows();
    const PreferencesDialog::Connection connection{
        serverUrl, secret, nickname, pin,
        QSettings().value("connectionMode", QUrl(serverUrl).scheme() == "p2p" ? "p2p" : "direct").toString(),
        p2pSettings, ready, connected && !ready, subtitle->text()};
    auto *dialog = new PreferencesDialog({downloadRoot, Theme::mode(),
        QSettings().value("notificationSound", true).toBool(), MediaStorage::cacheLimit(),
        SmoothScroll::savedDamping()}, connection, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    preferencesDialog = dialog;
    const auto scanGeneration = std::make_shared<std::atomic<quint64>>(0);
    connect(dialog, &QObject::destroyed, this, [scanGeneration] { scanGeneration->store(0); });
    connect(dialog, &PreferencesDialog::usageRequested, this, [dialog, scanGeneration](const QString &path, quint64 id)
    {
        scanGeneration->store(id);
        QPointer<PreferencesDialog> target(dialog);
        QThreadPool::globalInstance()->start([target, path, id, scanGeneration]
        {
            const auto usage = MediaStorage::measure(path, [scanGeneration, id] { return scanGeneration->load() != id; });
            QMetaObject::invokeMethod(qApp, [target, usage, id]
            { if (target) target->setUsage(id, usage.downloads, usage.cache, usage.error); });
        });
    });
    connect(dialog, &PreferencesDialog::clearCacheRequested, this,
        [this, dialog, scanGeneration](const QString &, quint64 id)
        {
            scanGeneration->store(0);
            QPointer<PreferencesDialog> target(dialog);
            pruneMediaCache(0, [target, id](const QString &error)
                { if (target) target->cacheCleared(id, error); });
        });
    connect(dialog, &PreferencesDialog::previewSoundRequested, this, &Window::notificationSoundPreviewRequested);
    // Applying a connection is immediate: the dialog stays open so the user can see
    // the result, and the chat banner reflects the same state.
    connect(dialog, &PreferencesDialog::connectRequested, this, [this, dialog]
    {
        const auto connection = dialog->connection();
        QJsonObject input{{"version", 1}, {"mode", connection.mode},
            {"serverUrl", connection.serverUrl}, {"token", connection.token},
            {"certificatePin", connection.certificatePin}, {"p2p", connection.p2p}};
        QJsonObject validated;
        QString error;
        if (!ConnectionInfo::parse(QJsonDocument(input).toJson(QJsonDocument::Compact), validated, error))
        {
            dialog->setConnectionState(false, false, error);
            return;
        }
        if (ready || connected || !serverUrl.isEmpty())
            transport.disconnectService();
        clearConversation();
        serverUrl = connection.serverUrl;
        secret = connection.token;
        nickname = connection.nickname.isEmpty() ? QStringLiteral("我") : connection.nickname.left(64);
        pin = connection.certificatePin;
        p2pSettings = connection.mode == "p2p" ? connection.p2p : QJsonObject{};
        QSettings settings;
        settings.setValue("serverUrl", serverUrl);
        settings.setValue("token", secret);
        settings.setValue("nickname", nickname);
        settings.setValue("certificatePin", pin);
        settings.setValue("connectionMode", connection.mode);
        settings.setValue("p2p", QJsonDocument(p2pSettings).toJson(QJsonDocument::Compact));
        chatPreferences.configure(serverUrl, secret);
        localFiles = std::make_shared<MediaStorage::LocalFiles>(chatPreferences.scope());
        dialog->setConnectionState(false, true, "正在连接…");
        connectService();
    });
    connect(dialog, &PreferencesDialog::disconnectRequested, this, [this, dialog]
    {
        quitting = false;
        transport.disconnectService();
        dialog->setConnectionState(false, false, "已断开；地址与密钥已保留，可随时重新连接。");
        setConnectionState("已断开连接。可在设置中重新连接。", false);
    });
    connect(dialog, &PreferencesDialog::applied, this, [this, dialog]
    {
        const auto values = dialog->values();
        if (MediaStorage::isCachePath(values.downloads) ||
            QDir::cleanPath(values.downloads) == QDir::cleanPath(MediaStorage::cacheDirectory())) return;
        if (downloadRoot != values.downloads)
        {
            invalidateAuxiliaryWindows();
            localFiles = std::make_shared<MediaStorage::LocalFiles>(chatPreferences.scope());
        }
        downloadRoot = MediaStorage::resolveRoot(values.downloads);
        QSettings settings;
        settings.setValue("downloads", downloadRoot);
        settings.setValue("notificationSound", values.notificationSound);
        settings.setValue("mediaCacheLimit", values.cacheLimitBytes);
        settings.setValue("wheelDamping", values.wheelDamping);
        SmoothScroll::refreshAll();
        Theme::apply(values.theme);
        refreshThemeIcons();
        cachePruneTimer.start();
    });
    dialog->selectPage(static_cast<int>(page));
    dialog->open();
}

void Window::pruneMediaCache(qint64 limit, std::function<void(const QString &)> callback)
{
    if (cacheWorking || !downloading.isEmpty() || transport.hasActiveDownloads())
    {
        if (callback) callback("缓存正在使用，请稍后重试。");
        return;
    }
    const auto root = QDir::cleanPath(MediaStorage::cacheDirectory());
    if (MediaStorage::isCachePath(downloadRoot) || QDir::cleanPath(downloadRoot) == root ||
        (!QFileInfo(downloadRoot).canonicalFilePath().isEmpty() &&
         (MediaStorage::isCachePath(QFileInfo(downloadRoot).canonicalFilePath()) ||
          QFileInfo(downloadRoot).canonicalFilePath() == QDir(root).canonicalPath())))
    {
        if (callback) callback("下载目录与缓存重叠，请先选择其他下载位置。");
        return;
    }
    QSet<QString> protectedPaths;
    const auto bounds = messages->viewport()->rect();
    const auto first = messages->indexAt(QPoint(1, 0));
    for (int row = qMax(0, first.row()); row < model->rowCount(); ++row)
    {
        const auto rect = messages->visualRect(model->index(row));
        if (rect.top() > bounds.bottom()) break;
        if (rect.intersects(bounds))
            for (const auto &entry : model->rows[row]["media"].toArray())
                for (const auto &field : {"_local", "_poster"})
                {
                    const auto path = entry.toObject()[field].toString();
                    if (!path.isEmpty()) protectedPaths.insert(QFileInfo(path).absoluteFilePath());
                }
    }
    const auto requestedPath = localFiles->pathForId(voiceGroup, requestedVoiceId);
    if (!requestedPath.isEmpty()) protectedPaths.insert(requestedPath);
    if (!playingVoicePath.isEmpty()) protectedPaths.insert(QFileInfo(playingVoicePath).absoluteFilePath());
    if (historyBrowser) protectedPaths.unite(historyBrowser->protectedCachePaths());
    updateCachePins();
    const auto files = localFiles;
    cacheWorking = true;
    if (historyBrowser) historyBrowser->setProperty("cacheMaintenance", true);
    QPointer<Window> self(this);
    QThreadPool::globalInstance()->start([self, limit, protectedPaths, files, callback]
    {
        const auto result = MediaStorage::prune(limit, protectedPaths, files);
        files->discard(result.removed);
        QMetaObject::invokeMethod(qApp, [self, files, result, callback]
        {
            if (!self) return;
            self->cacheWorking = false;
            if (self->localFiles == files)
            {
                QSet<QString> ids;
                QSet<QString> posterIds;
                for (const auto &row : self->model->rows)
                    for (const auto &entry : row["media"].toArray())
                    {
                        if (result.removed.contains(entry.toObject()["_local"].toString()))
                            ids.insert(entry.toObject()["id"].toString());
                        if (result.removed.contains(entry.toObject()["_poster"].toString()))
                            posterIds.insert(entry.toObject()["id"].toString());
                    }
                for (const auto &id : ids) self->model->setLocalMedia(id, {});
                for (const auto &id : posterIds) self->model->setLocalPoster(id, {});
            }
            if (self->historyBrowser)
            {
                self->historyBrowser->setProperty("cacheMaintenance", false);
                self->historyBrowser->discardCached(result.removed);
            }
            self->thumbnailTimer.start();
            self->drainMediaQueue();
            if (callback) callback(result.error);
        });
    });
}
void Window::updateCachePins()
{
    QSet<QString> paths;
    const auto bounds = messages->viewport()->rect();
    const auto first = messages->indexAt(QPoint(1, 0));
    for (int row = qMax(0, first.row()); row < model->rowCount(); ++row)
    {
        const auto rect = messages->visualRect(model->index(row));
        if (rect.top() > bounds.bottom()) break;
        if (rect.intersects(bounds))
            for (const auto &entry : model->rows[row]["media"].toArray())
            {
                for (const auto &field : {"_local", "_poster"})
                {
                    const auto path = entry.toObject()[field].toString();
                    if (!path.isEmpty()) paths.insert(QFileInfo(path).absoluteFilePath());
                }
            }
    }
    if (!playingVoicePath.isEmpty()) paths.insert(playingVoicePath);
    if (forwardDialog && forwardModel)
        for (const auto &row : forwardModel->rows)
            for (const auto &entry : row["media"].toArray())
                for (const auto &field : {"_local", "_poster"})
                {
                    const auto path = entry.toObject()[field].toString();
                    if (!path.isEmpty()) paths.insert(QFileInfo(path).absoluteFilePath());
                }
    const auto requested = localFiles->pathForId(voiceGroup, requestedVoiceId);
    if (!requested.isEmpty()) paths.insert(requested);
    localFiles->pin(this, paths);
}
QJsonArray Window::hydrateMessages(const QJsonArray &snapshot) const
{
    QJsonArray result;
    for (const auto &message : snapshot)
        result.append(localFiles->hydrate(message.toObject(), groupId));
    return result;
}
void Window::initializeMessageCache()
{
    // The test binaries drive fixtures that expect an empty conversation right after a switch,
    // and a cache shared between runs would make those expectations depend on history. Off means
    // off: no disk file, and no in-process rows either.
    cacheWriteTimer.stop();
    if (QStandardPaths::isTestModeEnabled())
    {
        messageCache.configure(QString());
        cacheReady = false;
        return;
    }
    cacheReady = messageCache.configure(chatPreferences.scope());
}
void Window::scheduleCacheWrite()
{
    // Restarting the timer per row would let a busy conversation postpone the write
    // forever, so a burst is coalesced into the first 800 ms window.
    // While the reader is looking at an older window the model no longer holds the
    // newest page; keeping the previous snapshot is better than caching that window.
    if (!cacheReady || groupId.isEmpty() || cacheWriteTimer.isActive() ||
        messages->property("locatedHistory").toBool())
        return;
    cacheWriteTimer.start();
}
void Window::storeCachedMessages()
{
    cacheWriteTimer.stop();
    if (!cacheReady || groupId.isEmpty() || messages->property("locatedHistory").toBool())
        return;
    QList<QJsonObject> rows;
    rows.reserve(model->rows.size());
    for (const auto &row : model->rows)
        if (row["group"].toString() == groupId)
            rows.append(row);
    // An empty model usually means the first page has not arrived (or failed), not
    // that the conversation has no messages, so it must not erase a usable snapshot.
    if (rows.isEmpty())
        return;
    // Written as-is: this is the window the list actually shows, so reopening paints
    // the same rows instead of a union with a previous session's window.
    messageCache.write(groupId, rows);
}
void Window::restoreCachedMessages()
{
    if (!cacheReady || groupId.isEmpty() || !model->rows.isEmpty())
        return;
    const auto cached = messageCache.read(groupId);
    if (cached.isEmpty())
        return;
    QJsonArray snapshot;
    for (const auto &row : cached)
    {
        // Counted as seen so a replay of these rows after reconnect cannot notify again.
        rememberMessage(row);
        snapshot.append(row);
    }
    model->replace(hydrateMessages(snapshot));
    messages->setProperty("locatedHistory", false);
    // Leaves the list exactly where a blank open would be: at the newest row, so the
    // first history page takes the "replace the window" branch and never merges into it.
    messages->scrollToBottom();
    refreshReeditLinks();
    thumbnailTimer.start();
}
void Window::recoverCachedMessages(const QJsonArray &snapshot, std::function<void()> callback)
{
    bool missing = false;
    for (const auto &entry : snapshot)
    {
        const auto message = entry.toObject();
        if (message["status"] == "recalled") continue;
        const auto group = message["group"].toString(groupId);
        for (const auto &value : message["media"].toArray())
            if (localFiles->find(group, value.toObject()).isEmpty()) { missing = true; break; }
        if (missing) break;
    }
    if (!missing) { callback(); return; }
    const auto files = localFiles;
    const auto directory = downloadRoot;
    const auto group = groupId;
    const auto cancellation = cacheRecoveryGeneration;
    const auto generation = cancellation->load();
    QPointer<Window> self(this);
    QThreadPool::globalInstance()->start([self, files, snapshot, directory, group, cancellation, generation,
                                        callback = std::move(callback)]
    {
        files->recover(snapshot, directory, group, [cancellation, generation] { return cancellation->load() != generation; });
        QMetaObject::invokeMethod(qApp, [self, files, snapshot, callback, cancellation, generation]
        {
            if (!self) return;
            if (cancellation->load() != generation) { callback(); return; }
            if (files != self->localFiles) { self->recoverCachedMessages(snapshot, callback); return; }
            callback();
        });
    });
}
bool Window::hasCurrentMedia(const QJsonObject &media) const
{
    if (forwardDialog && forwardMessage["group"] == groupId && forwardMessage["status"] != "recalled")
        for (const auto &entry : forwardMessage["media"].toArray())
            if (sameMediaDescriptor(entry.toObject(),media)) return true;
    for (const auto &row : model->rows)
        if (row["group"] == groupId && row["status"] != "recalled")
            for (const auto &entry : row["media"].toArray())
                if (sameMediaDescriptor(entry.toObject(), media)) return true;
    return false;
}

void Window::showSettings()
{
    // Connection settings live in the preferences dialog now, so every entry point
    // (banner, tray menu, initial setup) lands on the same page instead of a second
    // dialog that could drift from it.
    showPreferences(PreferencesPage::Connection);
}

void Window::startDemo()
{
    if (demoServer.state() != QProcess::NotRunning)
    {
        notify("演示服务已在运行", true);
        return;
    }
    QDir appDir(QCoreApplication::applicationDirPath());
    QString binary = appDir.filePath("wechat-relay");
#ifdef Q_OS_WIN
    binary += ".exe";
#endif
    QDir project = appDir;
    for (int i = 0; i < 6 && !QFileInfo::exists(project.filePath("Cargo.toml")); ++i)
        if (!project.cdUp())
            break;
    QString root = project.absolutePath();
    if (!QFileInfo::exists(binary))
        binary = QDir(root).filePath("target/release/wechat-relay"
#ifdef Q_OS_WIN
                                     ".exe"
#endif
        );
    if (!QFileInfo::exists(binary))
        binary = QDir(root).filePath("target/debug/wechat-relay"
#ifdef Q_OS_WIN
                                     ".exe"
#endif
        );
    if (!QFileInfo::exists(binary))
    {
        notify("找不到演示服务组件 wechat-relay，请安装演示组件后再使用 --demo", true);
        return;
    }
    auto folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/demo-server";
    QDir().mkpath(folder);
    auto configPath = folder + "/server.json";
    QJsonObject config;
    QFile old(configPath);
    if (old.open(QIODevice::ReadOnly))
        config = QJsonDocument::fromJson(old.readAll()).object();
    old.close();
    secret = config["token"].toString();
    config = {}; // Demo never reuses bridge, listener or certificate settings from another service.
    if (secret.size() < 24)
        secret = QUuid::createUuid().toString(QUuid::WithoutBraces) +
                 QUuid::createUuid().toString(QUuid::WithoutBraces);
    config["token"] = secret;
    {
        QTcpServer port;
        if (!port.listen(QHostAddress::LocalHost, 0))
        {
            notify("无法分配演示服务端口", true);
            return;
        }
        const auto number = port.serverPort();
        port.close();
        config["listen"] = "127.0.0.1:" + QString::number(number);
    }
    config["data"] = folder + "/data";
    QUrl localUrl("http://" + config["listen"].toString());
    if (!localUrl.isValid() || localUrl.host().isEmpty() || localUrl.port() < 1)
    {
        notify("演示服务监听地址无效", true);
        return;
    }
    QSaveFile file(configPath);
    if (!file.open(QIODevice::WriteOnly))
    {
        notify(file.errorString(), true);
        return;
    }
    file.write(QJsonDocument(config).toJson());
    if (!file.commit())
    {
        notify(file.errorString(), true);
        return;
    }
    demoFailure.clear();
    demoServer.setProgram(binary);
    demoServer.setArguments({"--config", configPath, "--demo"});
    demoServer.setWorkingDirectory(folder);
    demoServer.start();
    if (!demoServer.waitForStarted(3000))
    {
        notify("无法启动演示服务：" + demoServer.errorString(), true);
        return;
    }
    serverUrl = localUrl.toString();
    pin.clear();
    transport.setOwnedServer(serverUrl, secret);
    if (QSystemTrayIcon::isSystemTrayAvailable())
        tray.show();
    notify("演示模式 · 可以测试附件和客户端同步，不会发送到真实微信");
    QTimer::singleShot(500, this, &Window::connectService);
}

void Window::cancelSend()
{
    ++sendGeneration;
    transport.cancelSend();
    busy = false;
    attachments.setLocked(false);
    for (auto &item : attachments.items)
        item.uploaded.clear();
    composer->setReadOnly(false);
    sendButton->setText("发送");
    transferLabel->clear();
}
void Window::clearConversation()
{
    // Before `locatedHistory` is cleared below: a conversation showing an older window
    // must not be saved as if that window were the newest page.
    storeCachedMessages();
    invalidateAuxiliaryWindows();
    historyButton->setEnabled(false);
    announcementButton->setEnabled(false);
    stopVoice();
    openRequests.clear();
    saveRequests.clear();
    pendingMediaCopy = {};
    mediaQueue.clear();
    queuedMediaIds.clear();
    locatingId.clear();
    unavailableOriginals.clear();
    failedPosters.clear();
    historyUpdates.clear();
    ++locateGeneration;
    messages->setProperty("locatedHistory", false);
    messages->stopScrollAnimation();
    cancelSend();
    saveDraft();
    ++conversationGeneration;
    ++*cacheRecoveryGeneration;
    ++historyGeneration;
    loading = false;
    hasOlder = true;
    groupId.clear();
    pendingId.clear();
    quotedMessage = {};
    refreshQuote();
    memberCache = {};
    restoring = true;
    composer->clear();
    composer->setMembers({});
    attachments.restore({});
    restoring = false;
    model->replace({});
    composer->setReadOnly(true);
    findChild<QWidget *>("composeArea")->setEnabled(false);
    sendButton->setEnabled(false);
    title->setText("暂无转发群");
}
void Window::connectService()
{
    // Persist the conversation still on screen before the identity (and with it the
    // cache scope) changes, so one server's rows never land in another's file.
    storeCachedMessages();
    selfMemberId.clear();
    notificationsSynced = false;
    notificationSyncSeq = 0;
    if (chatPreferences.configure(serverUrl, secret))
    {
        localFiles = std::make_shared<MediaStorage::LocalFiles>(chatPreferences.scope());
        initializeMessageCache();
        seenMessages.clear();
        seenMessageOrder.clear();
    }
    ++permissionsGeneration;
    canSend = canDownload = false;
    messages->setProperty("canDownload", false);
    recallDrafts.configure(serverUrl + '\n' + secret + '\n' + clientId);
    ++groupsGeneration;
    clearConversation();
    unread.clear();
    unreadMentions.clear();
    updateUnreadTotal();
    groups->clear();
    ready = false;
    tunnel.stop();
    setProperty("connectionPath", QVariant());
    if (QUrl(serverUrl).scheme() == "p2p")
    {
        transport.configure({}, secret, pin);
        QJsonObject validated;
        QString error;
        const QJsonObject input{{"version", 1}, {"mode", "p2p"}, {"serverUrl", serverUrl},
            {"token", secret}, {"certificatePin", pin}, {"p2p", p2pSettings}};
        if (!ConnectionInfo::parse(QJsonDocument(input).toJson(QJsonDocument::Compact), validated, error))
        {
            notify(error, true);
            subtitle->setText(error);
            return;
        }
        setConnectionState("正在通过公网入口连接服务器…", true);
        tunnel.start(p2pSettings);
        return;
    }
    transport.configure(serverUrl, secret, pin);
    setConnectionState("正在连接服务器…", true);
    transport.connectEvents();
    loadGroups();
}
void Window::loadPermissions()
{
    const auto generation = ++permissionsGeneration;
    const auto recallGeneration = recallCapabilityGeneration;
    transport.get(
        "/api/status",
        [this, generation, recallGeneration](const QJsonValue &value, const QString &error)
        {
            if (generation != permissionsGeneration)
                return;
            if (!error.isEmpty())
            {
                canSend = canDownload = false;
                notify("无法读取客户端权限：" + error, true);
            }
            else
            {
                const auto info = value.toObject();
                if (recallGeneration == recallCapabilityGeneration)
                    messages->setRecallCapability(info);
                const auto permissions = info["permissions"].toObject();
                canSend = !info.contains("permissions") || permissions["can_send"].toBool();
                canDownload = !info.contains("permissions") || permissions["can_download"].toBool();
            }
            messages->setProperty("canDownload", canDownload);
            if (!canDownload)
            {
                invalidateAuxiliaryWindows();
                stopVoice();
                openRequests.clear();
                saveRequests.clear();
                pendingMediaCopy = {};
                mediaQueue.clear();
                queuedMediaIds.clear();
            }
            if (!canSend && busy)
            {
                cancelSend();
                saveDraft();
            }
            refreshAttachments();
            if (canDownload)
            {
                failedPosters.clear();
                thumbnailTimer.start();
            }
        },
        10000);
}
void Window::loadGroups()
{
    const auto generation = ++groupsGeneration;
    transport.get("/api/groups",
                  [this, generation](const QJsonValue &value, const QString &error)
                  {
                      if (generation != groupsGeneration)
                          return;
                      if (!error.isEmpty())
                      {
                          notify(error, true);
                          return;
                      }
                      const auto previous = groupId;
                      groups->blockSignals(true);
                      groups->clear();
                      QListWidgetItem *selected = nullptr;
                      QListWidgetItem *activated = nullptr;
                      QSet<QString> accessibleGroups;
                      int normalOrder = 0;
                      for (const auto &entry : value.toArray())
                      {
                          auto obj = entry.toObject();
                          accessibleGroups.insert(obj["id"].toString());
                          obj["_normalOrder"] = normalOrder++;
                          obj["unread"] = unread.value(obj["id"].toString());
                          auto *item = new QListWidgetItem(groups);
                          item->setData(Qt::UserRole, obj);
                          item->setData(Qt::AccessibleTextRole, obj["name"].toString());
                          item->setHidden(
                              !obj["name"].toString().contains(groupSearch->text(), Qt::CaseInsensitive));
                          if (obj["id"] == previous)
                              selected = item;
                          if (obj["id"] == pendingActivatedGroup)
                              activated = item;
                      }
                      for (auto it = unread.begin(); it != unread.end();)
                          if (!accessibleGroups.contains(it.key()))
                          {
                              unreadMentions.remove(it.key());
                              it = unread.erase(it);
                          }
                          else
                              ++it;
                      updateUnreadTotal();
                      sortGroups();
                      if (!previous.isEmpty() && !selected)
                      {
                          // The admin revoked this conversation: its rows are no longer
                          // readable, so the local copy goes with it.
                          messageCache.remove(previous);
                          clearConversation();
                      }
                      groups->blockSignals(false);
                      if (!activated)
                          pendingActivatedGroup.clear();
                      if (activated && !busy)
                      {
                          pendingActivatedGroup.clear();
                          groups->setCurrentItem(activated);
                      }
                      else if (selected)
                      {
                          groups->setCurrentItem(selected);
                          loadMembers();
                      }
                      else if (groups->count() > 0)
                          groups->setCurrentRow(0);
                      else
                      {
                          clearConversation();
                          notify("服务端尚未勾选转发群，请联系服务端管理员选择允许此客户端访问的群。");
                      }
                      historyButton->setEnabled(connected && !groupId.isEmpty());
                      announcementButton->setEnabled(connected && !groupId.isEmpty());
                  });
}

void Window::invalidateAuxiliaryWindows()
{
    if (forwardDialog) forwardDialog->close();
    forwardDialog.clear();
    forwardMessage = {};
    forwardModel = nullptr;
    forwardView = nullptr;
    if (historyBrowser)
        historyBrowser->invalidate();
    if (announcementDialog)
        announcementDialog->invalidate();
    historyBrowser.clear();
    announcementDialog.clear();
    pendingRecordId.clear();
    pendingRecordMedia = {};
}

void Window::showForwardRecord(const QJsonObject &message)
{
    if (!connected || message["group"] != groupId || message["status"] == "recalled" || !message["record"].isObject()) return;
    forwardMessage = localFiles->hydrate(message,groupId);
    if (!forwardDialog)
    {
        forwardDialog = new QDialog(this);
        forwardDialog->setObjectName("forwardRecordDialog");
        forwardDialog->setAttribute(Qt::WA_DeleteOnClose);
        forwardDialog->resize(600,720);
        auto *layout = new QVBoxLayout(forwardDialog);
        auto *state = new QLabel(forwardDialog);
        state->setObjectName("forwardRecordState");
        layout->addWidget(state);
        forwardModel = new MessageModel(forwardDialog);
        forwardModel->setProperty("preserveOrder",true);
        forwardModel->setProperty("readOnlyRecord",true);
        forwardView = new MessageView(forwardDialog);
        forwardView->setObjectName("forwardRecordMessages");
        forwardView->setProperty("readOnlyRecord",true);
        forwardView->setProperty("canDownload",canDownload);
        forwardView->setModel(forwardModel);
        layout->addWidget(forwardView);
        connect(forwardView,&MessageView::mediaClicked,this,[this](const QJsonObject &media,bool save){fetchMedia(media,save);});
        connect(forwardView,&MessageView::mediaCopyRequested,this,&Window::copyMedia);
        connect(forwardView,&MessageView::viewportChanged,this,[this]{updateCachePins();thumbnailTimer.start();});
        connect(forwardDialog,&QDialog::finished,this,[this]{
            forwardDialog.clear();forwardMessage={};forwardModel=nullptr;forwardView=nullptr;
            updateCachePins();
        });
    }
    refreshForwardRecord();
    forwardDialog->show();
    forwardDialog->raise();
    forwardDialog->activateWindow();
    thumbnailTimer.start();
}

void Window::refreshForwardRecord()
{
    if (!forwardDialog || !forwardModel) return;
    for (const auto &row : model->rows)
        if (row["group"] == groupId && row["id"] == forwardMessage["id"]) {forwardMessage=row;break;}
    if (forwardMessage["group"] != groupId || forwardMessage["status"] == "recalled") {forwardDialog->close();return;}
    forwardMessage=localFiles->hydrate(forwardMessage,groupId);
    const auto record=forwardMessage["record"].toObject();
    forwardDialog->setWindowTitle(record["title"].toString("聊天记录"));
    const auto items=record["items"].toArray();
    auto *state=forwardDialog->findChild<QLabel *>("forwardRecordState");
    state->setText(QString("%1 条消息%2").arg(items.size()).arg(record["pending"].toBool() ? " · 附件后台下载中，会自动补齐" : ""));
    QJsonArray rows;
    const auto recordMedia=forwardMessage["media"].toArray();
    for (const auto &value : items)
    {
        auto item=value.toObject();
        QJsonArray media;
        for (const auto &index : item["media"].toArray())
            if (index.toInt(-1)>=0 && index.toInt()<recordMedia.size()) media.append(recordMedia[index.toInt()]);
        item["media"]=media;
        item["group"]=groupId;
        item["status"]="received";
        item["outgoing"]=false;
        rows.append(item);
    }
    forwardModel->replace(rows);
    updateCachePins();
    thumbnailTimer.start();
}

void Window::showHistoryBrowser()
{
    if (!connected || groupId.isEmpty())
        return;
    if (historyBrowser)
    {
        historyBrowser->showNormal();
        historyBrowser->raise();
        historyBrowser->activateWindow();
        return;
    }
    historyBrowser = new HistoryBrowser(&transport, groupId, title->text(), downloadRoot, canDownload, this, localFiles);
    connect(historyBrowser, &HistoryBrowser::recordActivated, this, &Window::activateHistoryRecord);
    connect(historyBrowser, &HistoryBrowser::cacheUpdated, this, [this] { cachePruneTimer.start(); });
    connect(historyBrowser, &QDialog::finished, this, [this] { cachePruneTimer.start(); });
    historyBrowser->setProperty("cacheMaintenance", cacheWorking);
    historyBrowser->show();
}

void Window::showAnnouncement()
{
    if (!connected || groupId.isEmpty())
        return;
    if (announcementDialog)
    {
        announcementDialog->showNormal();
        announcementDialog->raise();
        announcementDialog->activateWindow();
        return;
    }
    announcementDialog = new AnnouncementDialog(&transport, groupId, title->text(), this);
    announcementDialog->show();
}

void Window::activateHistoryRecord(const QJsonObject &message, const QJsonObject &media)
{
    if (!connected || message["group"] != groupId || groupId.isEmpty() ||
        message["id"].toString().isEmpty() || !locatingId.isEmpty())
        return;
    if (message["record"].isObject() && media.isEmpty()) {showForwardRecord(message);return;}
    if (historyBrowser)
        historyBrowser->hide();
    showNormal();
    raise();
    activateWindow();
    pendingRecordId = !media.isEmpty() && canDownload && message["status"] != "recalled"
        ? message["id"].toString() : QString();
    pendingRecordMedia = pendingRecordId.isEmpty() ? QJsonObject{} : media;
    locateQuote({{"id", message["id"]}, {"group", groupId},
                 {"unavailable", message["status"] == "recalled"}});
    if (!pendingRecordId.isEmpty())
        playLocatedRecord(pendingRecordId);
}

void Window::playLocatedRecord(const QString &id)
{
    if (pendingRecordId != id || pendingRecordMedia.isEmpty())
        return;
    for (const auto &message : model->rows)
        if (message["id"] == id && message["group"] == groupId)
        {
            if (message["status"] != "recalled")
                for (const auto &value : message["media"].toArray())
                {
                    const auto media = value.toObject();
                    if (sameMediaDescriptor(media, pendingRecordMedia))
                    {
                        clearPendingRecord(id);
                        fetchMedia(media);
                        return;
                    }
                }
            clearPendingRecord(id); // The source arrived but no longer contains this attachment.
            return;
        }
}
void Window::clearPendingRecord(const QString &id)
{
    if (pendingRecordId != id) return;
    pendingRecordId.clear();
    pendingRecordMedia = {};
}

void Window::activateConversation(const QString &id)
{
    showNormal();
    raise();
    activateWindow();
    if (id.isEmpty() || id.size() > 512)
        return;
    pendingActivatedGroup = id;
    for (int row = 0; row < groups->count(); ++row)
    {
        auto *item = groups->item(row);
        if (item->data(Qt::UserRole).toJsonObject().value("id").toString() == id)
        {
            groups->setCurrentItem(item);
            if (groupId == id)
                pendingActivatedGroup.clear();
            return;
        }
    }
    if (connected)
        loadGroups();
}

void Window::updateUnreadTotal()
{
    // The Dock badge mirrors what the group's notification level actually alerts on: an ignored
    // conversation contributes nothing and a mentions-only conversation only contributes the
    // unread messages that mentioned this account. unread itself keeps the plain new-message
    // marker drawn in the conversation list, whatever the level.
    int total = 0;
    for (auto it = unread.cbegin(); it != unread.cend(); ++it)
    {
        const auto mentions = unreadMentions.value(it.key());
        switch (chatPreferences.notificationMode(it.key()))
        {
        case ChatPreferences::NotificationMode::Ignore:
            break;
        case ChatPreferences::NotificationMode::MentionsOnly:
            total += mentions;
            break;
        case ChatPreferences::NotificationMode::AllMessages:
            total += it.value();
            break;
        }
    }
    emit unreadTotalChanged(total);
}

void Window::sortGroups()
{
    QList<QListWidgetItem *> ordered;
    for (int row = 0; row < groups->count(); ++row)
    {
        auto *item = groups->item(row);
        auto group = item->data(Qt::UserRole).toJsonObject();
        const auto id = group.value("id").toString();
        group["pinned"] = chatPreferences.pinned(id);
        group["muted"] = chatPreferences.muted(id);
        item->setData(Qt::UserRole, group);
        ordered.append(item);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const QListWidgetItem *left, const QListWidgetItem *right)
    {
        const auto a = left->data(Qt::UserRole).toJsonObject(), b = right->data(Qt::UserRole).toJsonObject();
        if (a.value("pinned").toBool() != b.value("pinned").toBool())
            return a.value("pinned").toBool();
        if (a.value("timestamp").toInteger() != b.value("timestamp").toInteger())
            return a.value("timestamp").toInteger() > b.value("timestamp").toInteger();
        return a.value("_normalOrder").toInt() < b.value("_normalOrder").toInt();
    });
    bool changed = false;
    for (int row = 0; row < ordered.size(); ++row)
        changed |= ordered[row] != groups->item(row);
    auto *selected = groups->currentItem();
    if (changed)
    {
        const bool blocked = groups->blockSignals(true);
        const int scroll = groups->verticalScrollBar()->value();
        while (groups->count())
            groups->takeItem(0);
        for (auto *item : ordered)
            groups->addItem(item);
        if (selected)
            groups->setCurrentItem(selected);
        groups->verticalScrollBar()->setValue(scroll);
        groups->blockSignals(blocked);
    }
    for (auto *item : ordered)
        item->setHidden(!item->data(Qt::UserRole).toJsonObject().value("name").toString()
                             .contains(groupSearch->text(), Qt::CaseInsensitive));
}

QMenu *Window::groupContextMenuFor(const QString &id)
{
    bool present = false;
    for (int row = 0; row < groups->count(); ++row)
        present |= groups->item(row)->data(Qt::UserRole).toJsonObject().value("id").toString() == id;
    if (!present)
        return nullptr;
    const auto scope = chatPreferences.scope();
    auto *menu = createChatContextMenu(groups);
    menu->setObjectName("groupContextMenu");
    menu->setToolTipsVisible(false);
    const bool pinned = chatPreferences.pinned(id);
    auto *pinAction = menu->addAction(pinned ? "取消置顶" : "置顶", this, [this, id, pinned, scope]
    {
        if (scope != chatPreferences.scope())
            return;
        chatPreferences.setPinned(id, !pinned);
        sortGroups();
    });
    pinAction->setObjectName("pinGroup");
    menu->addSeparator();
    auto *notificationActions = new QActionGroup(menu);
    notificationActions->setExclusive(true);
    using Mode = ChatPreferences::NotificationMode;
    const auto addNotificationMode = [&](const QString &text, const QString &name, Mode mode)
    {
        auto *action = menu->addAction(text);
        action->setObjectName(name);
        action->setCheckable(true);
        action->setChecked(chatPreferences.notificationMode(id) == mode);
        notificationActions->addAction(action);
        connect(action, &QAction::triggered, this, [this, id, scope, mode]
        {
            if (scope != chatPreferences.scope())
                return;
            chatPreferences.setNotificationMode(id, mode);
            // The level also decides what the Dock badge counts, so the aggregate follows a change
            // made after the messages arrived instead of staying stuck on the old level.
            updateUnreadTotal();
            sortGroups();
        });
    };
    addNotificationMode("忽略通知", "groupNotificationsIgnore", Mode::Ignore);
    addNotificationMode("仅被 @ 时通知", "groupNotificationsMentions", Mode::MentionsOnly);
    addNotificationMode("所有新消息通知", "groupNotificationsAll", Mode::AllMessages);
    return menu;
}

bool Window::rememberMessage(const QJsonObject &message)
{
    const auto id = message.value("id").toString(), group = message.value("group").toString();
    if (id.isEmpty() || group.isEmpty() || id.size() > 512 || group.size() > 512)
        return false;
    auto identity = group.toUtf8();
    identity.append('\0');
    identity.append(id.toUtf8());
    const auto key = QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
    if (seenMessages.contains(key))
        return false;
    seenMessages.insert(key);
    seenMessageOrder.enqueue(key);
    if (seenMessageOrder.size() > 4096)
        seenMessages.remove(seenMessageOrder.dequeue());
    return true;
}

void Window::maybeNotifyMessage(const QJsonObject &event, bool firstSeen)
{
    const auto message = event.value("message").toObject();
    const auto id = message.value("group").toString();
    if (!firstSeen || !connected || !notificationsSynced ||
        event.value("seq").toInteger() <= notificationSyncSeq ||
        !event.value("created").toBool() || event.value("historical").toBool() ||
        message.value("outgoing").toBool() || message.value("status") != "received" ||
        (!selfMemberId.isEmpty() && message.value("sender_id").toString() == selfMemberId) ||
        !chatPreferences.allowsNotification(id, message.value("mentions").toArray(), selfMemberId))
        return;
    QString name;
    for (int row = 0; row < groups->count(); ++row)
    {
        const auto group = groups->item(row)->data(Qt::UserRole).toJsonObject();
        if (group.value("id").toString() == id)
        {
            name = group.value("name").toString();
            break;
        }
    }
    if (name.isEmpty())
        return;
    auto text = message.value("text").toString();
    if (text.trimmed().isEmpty())
    {
        const auto media = message.value("media").toArray();
        const auto kind = media.isEmpty() ? QString() : media.first().toObject().value("kind").toString();
        text = kind == "image" ? "[图片]" : kind == "gif" ? "[GIF]" : kind == "video" ? "[视频]" :
            kind == "voice" ? "[语音]" : kind == "file" ? "[文件]" :
            message.value("quote").isObject() ? "[引用消息]" : "[新消息]";
    }
    const auto clean = [](QString value, int limit)
    {
        value.replace(QRegularExpression("[\\x00-\\x1f\\x7f]+"), " ");
        return value.simplified().left(limit);
    };
    const auto sender = clean(message.value("sender").toString(), 60);
    const auto body = (sender.isEmpty() ? QString() : sender + "：") + clean(text, 220);
    emit systemNotificationRequested(id, clean(name, 120), body);
}

void Window::selectGroup(const QString &id, const QString &name)
{
    if (id == groupId)
        return;
    // restoreDraft() focuses the editor, which is right for a mouse click but would eject
    // a keyboard user from the group list after the first arrow key. Remember where the
    // selection was driven from and give focus back once the switch is complete.
    const bool keepListFocus = groupSelectionFromKeyboard;
    groupSelectionFromKeyboard = false;
    if (busy)
    {
        groups->blockSignals(true);
        for (int i = 0; i < groups->count(); ++i)
            if (groups->item(i)->data(Qt::UserRole).toJsonObject()["id"] == groupId)
                groups->setCurrentRow(i);
        groups->blockSignals(false);
        notify("附件发送完成后可切换会话", true);
        return;
    }
    invalidateAuxiliaryWindows();
    saveDraft();
    storeCachedMessages();
    stopVoice();
    openRequests.clear();
    saveRequests.clear();
    pendingMediaCopy = {};
    mediaQueue.clear();
    queuedMediaIds.clear();
    locatingId.clear();
    unavailableOriginals.clear();
    historyUpdates.clear();
    failedPosters.clear();
    ++locateGeneration;
    messages->setProperty("locatedHistory", false);
    messages->stopScrollAnimation();
    ++conversationGeneration;
    ++*cacheRecoveryGeneration;
    ++historyGeneration;
    loading = false;
    hasOlder = true;
    groupId = id;
    findChild<QWidget *>("composeArea")->setEnabled(true);
    composer->setReadOnly(false);
    unread.remove(id);
    unreadMentions.remove(id);
    updateUnreadTotal();
    if (auto *item = groups->currentItem())
    {
        auto group = item->data(Qt::UserRole).toJsonObject();
        group["unread"] = 0;
        item->setData(Qt::UserRole, group);
    }
    pendingId.clear();
    title->setText(name);
    historyButton->setEnabled(connected);
    announcementButton->setEnabled(connected);
    memberCache = {};
    composer->setMembers({});
    restoreDraft();
    model->replace({});
    restoreCachedMessages();
    loadHistory();
    loadMembers();
    activateCurrentGroup();
    if (keepListFocus)
        groups->setFocus(Qt::OtherFocusReason);
}
void Window::activateCurrentGroup()
{
    if (!ready || groupId.isEmpty())
        return;
    const auto id = groupId;
    const auto generation = conversationGeneration;
    transport.post("/api/groups/" + id + "/activate", {},
                   [this, id, generation](const QJsonValue &, const QString &error)
                   {
                       if (id == groupId && generation == conversationGeneration && !error.isEmpty())
                           notify(error, true);
                   });
}
void Window::loadHistory(bool older, int pendingAttempt)
{
    if (groupId.isEmpty() || loading || (older && !hasOlder))
        return;
    loading = true;
    historyUpdates.clear();
    const auto generation = ++historyGeneration;
    auto id = groupId;
    QUrlQuery query;
    query.addQueryItem("group", id);
    if (older && !model->rows.isEmpty())
        query.addQueryItem("before", QString::number(model->rows.first()["seq"].toInteger()));
    auto applyPage = [this, id, older, pendingAttempt, generation](const QJsonValue &value, const QString &error)
        {
            if (id != groupId || generation != historyGeneration)
                return;
            loading = false;
            const auto updates = historyUpdates;
            historyUpdates.clear();
            if (id != groupId)
                return;
            if (!error.isEmpty())
            {
                notify(error, true);
                return;
            }
            const auto page = value.toObject();
            const auto entries = value.isArray() ? value.toArray() : page.value("messages").toArray();
            for (const auto &entry : entries)
                rememberMessage(entry.toObject());
            const bool pending = page.value("pending").toBool();
            hasOlder = value.isArray() ? !entries.isEmpty() : page.value("has_more").toBool() || pending;
            if (older)
            {
                if (!entries.isEmpty())
                {
                    model->prepend(hydrateMessages(entries));
                    messages->setProperty("locatedHistory", true);
                }
            }
            else
            {
                const auto *bar = messages->verticalScrollBar();
                // Cached rows are already painted, so "was at the bottom" is the only signal that
                // the newest page may replace them; a reader scrolled up keeps their window.
                const bool followLatest = model->rows.isEmpty() ||
                    (!messages->property("locatedHistory").toBool() &&
                     !messages->scrollAnimationActive() && bar->maximum() - bar->value() <= 2);
                auto snapshot = entries;
                for (const auto &update : updates)
                {
                    bool replaced = false;
                    for (int row = 0; row < snapshot.size(); ++row)
                        if (snapshot[row].toObject()["id"] == update["id"])
                        {
                            snapshot[row] = update;
                            replaced = true;
                            break;
                        }
                    if (!replaced)
                        snapshot.append(update);
                }
                if (!followLatest)
                {
                    // A retry/reconnect must not replace a reading window with the latest page.
                    // Refresh existing rows and fill unused capacity, retaining every current ID.
                    QJsonArray retained;
                    QHash<QString, int> positions;
                    // Only this conversation: switching groups leaves the previous rows in place
                    // until the model is emptied, and carrying them over would duplicate them.
                    for (const auto &message : model->rows)
                    {
                        if (message["group"].toString() != id)
                            continue;
                        positions.insert(message["id"].toString(), int(retained.size()));
                        retained.append(message);
                    }
                    for (const auto &entry : snapshot)
                    {
                        const auto key = entry.toObject()["id"].toString();
                        if (key.isEmpty())
                            continue;
                        if (positions.contains(key))
                            retained[positions[key]] = entry;
                        else if (retained.size() < 300)
                        {
                            positions.insert(key, int(retained.size()));
                            retained.append(entry);
                        }
                    }
                    snapshot = retained;
                }
                messages->setProperty("locatedHistory", !followLatest);
                model->replace(hydrateMessages(snapshot));
                if (followLatest)
                    messages->scrollToBottom();
            }
            refreshReeditLinks();
            for (const auto &message : model->rows)
                if (message["status"] == "recalled" && message["id"] == quotedMessage.value("id"))
                {
                    quotedMessage["unavailable"] = true;
                    quotedMessage["text"] = "";
                    refreshQuote();
                    saveDraft();
                    break;
                }
            thumbnailTimer.start();
            // Bounded retries keep a slow native page from being mistaken for end of history.
            if (pending && pendingAttempt < 4)
                QTimer::singleShot(500 << pendingAttempt, this, [this, id, older, generation, pendingAttempt]
                {
                    if (id == groupId && generation == historyGeneration && !loading)
                        loadHistory(older, pendingAttempt + 1);
                });
        };
    transport.get("/api/history?" + query.toString(QUrl::FullyEncoded),
        [this, id, generation, applyPage](const QJsonValue &value, const QString &error)
        {
            if (id != groupId || generation != historyGeneration) return;
            if (!error.isEmpty()) { applyPage(value, error); return; }
            const auto entries = value.isArray() ? value.toArray() : value.toObject()["messages"].toArray();
            recoverCachedMessages(entries, [applyPage, value, error] { applyPage(value, error); });
        });
}
void Window::loadMembers()
{
    auto id = groupId;
    if (id.isEmpty())
        return;
    const auto generation = conversationGeneration;
    transport.get("/api/groups/" + id + "/members",
                  [this, id, generation](const QJsonValue &value, const QString &error)
                  {
                      if (id != groupId || generation != conversationGeneration || !error.isEmpty())
                          return;
                      memberCache = value.toArray();
                      composer->setMembers(memberCache);
                      if (!memberCache.isEmpty())
                          subtitle->setText(QString("%1 位成员  ·  %2")
                                                .arg(memberCache.size())
                                                .arg(ready ? "消息实时同步" : "等待服务端微信连接"));
                  });
}
void Window::handleEvent(const QJsonObject &event)
{
    auto type = event["type"].toString();
    if (type == "status")
    {
        ++recallCapabilityGeneration;
        messages->setRecallCapability(event);
        const bool wasReady = ready;
        ready = event["ready"].toBool();
        selfMemberId = ready ? event.value("self_id").toString() : QString();
        if (selfMemberId.toUtf8().size() > 1024 || selfMemberId != selfMemberId.trimmed() ||
            selfMemberId == "notify@all" || std::any_of(selfMemberId.cbegin(), selfMemberId.cend(),
                [](QChar character) { return character.category() == QChar::Other_Control; }))
            selfMemberId.clear();
        if (ready && !wasReady && !groupId.isEmpty())
        {
            activateCurrentGroup();
            loadMembers();
        }
        subtitle->setText(event["text"].toString());
        updateSendButton();
        if (event["demo"].toBool())
            notify("演示模式 · 此处发送的消息不会进入真实微信");
    }
    else if (type == "reset")
    {
        selfMemberId.clear();
        invalidateAuxiliaryWindows();
        notificationsSynced = false;
        notificationSyncSeq = 0;
        restoreCachedMessages();
        loadHistory();
        loadGroups();
    }
    else if (type == "synced")
    {
        notificationSyncSeq = qMax(qint64(0), event.value("seq").toInteger());
        notificationsSynced = connected;
    }
    else if (type == "groups")
        loadGroups();
    else if (type == "members" && event["group"] == groupId)
        loadMembers();
    else if (type == "warning")
        notify(event["text"].toString(), true);
    else if (type == "message")
    {
        auto message = localFiles->hydrate(event["message"].toObject(), groupId);
        if (forwardDialog && message["group"] == groupId && message["id"] == forwardMessage["id"])
        {
            forwardMessage=message;
            if (message["status"] == "recalled") forwardDialog->close();
            else refreshForwardRecord();
        }
        if (historyBrowser)
            historyBrowser->updateMessage(message);
        if (message["status"] == "recalled" && message["id"] == pendingRecordId)
        {
            pendingRecordId.clear();
            pendingRecordMedia = {};
        }
        const bool firstSeen = rememberMessage(message);
        const bool historical = event["historical"].toBool();
        const bool visibleHistory = std::any_of(model->rows.cbegin(), model->rows.cend(),
            [&message](const QJsonObject &row) { return row["id"] == message["id"]; });
        if (message["status"] == "recalled" && !requestedVoiceId.isEmpty())
            for (const auto &row : model->rows)
                if (row["id"] == message["id"])
                    for (const auto &entry : row["media"].toArray())
                        if (entry.toObject()["id"] == requestedVoiceId)
                            stopVoice();
        if (message["status"] == "recalled" && quotedMessage.value("id") == message["id"])
        {
            quotedMessage["unavailable"] = true;
            quotedMessage["text"] = "";
            refreshQuote();
            saveDraft();
        }
        if (message["group"] == groupId && (!historical || visibleHistory))
        {
            if (loading)
            {
                historyUpdates.insert(message["id"].toString(), message);
                if (historyUpdates.size() > 300)
                {
                    const auto oldest = std::min_element(
                        historyUpdates.cbegin(), historyUpdates.cend(),
                        [](const QJsonObject &left, const QJsonObject &right)
                        { return left["timestamp"].toInteger() < right["timestamp"].toInteger(); });
                    historyUpdates.remove(oldest.key());
                }
            }
            if (message["status"] == "recalled")
            {
                unavailableOriginals.insert(message["id"].toString());
                if (unavailableOriginals.size() > 512)
                    unavailableOriginals.erase(unavailableOriginals.begin());
            }
            const auto recall = message["recall_status"].toString();
            if ((recall == "failed" || recall == "uncertain") &&
                !message["recall_error"].toString().isEmpty())
                notify((recall == "failed" ? "撤回失败：" : "撤回结果待核对：") +
                           message["recall_error"].toString(),
                       true);
            model->upsert(message);
            if (message["status"] == "recalled")
                model->setCanReedit(message["id"].toString(),
                                    !recallDrafts.get(message["id"].toString(), groupId).isEmpty());
            thumbnailTimer.start();
        }
        for (int i = 0; !historical && i < groups->count(); ++i)
        {
            auto *item = groups->item(i);
            auto obj = item->data(Qt::UserRole).toJsonObject();
            if (obj["id"] == message["group"])
            {
                if (message["timestamp"].toInteger() >= obj["timestamp"].toInteger())
                {
                    obj["preview"] =
                        message["status"] == "recalled"
                            ? QString("[消息已撤回]")
                            : message["sender"].toString() + "：" +
                                  (message["text"].toString().isEmpty() ? "[附件]"
                                                                        : message["text"].toString());
                    obj["timestamp"] = message["timestamp"];
                }
                // A message counts as unread unless the user is actually looking at it: the
                // conversation has to be open *and* the window has to be active. Otherwise a
                // message that lands in the open conversation while the app sits in the
                // background would never reach the Dock badge.
                if (firstSeen && message["status"] == "received" && !message["outgoing"].toBool() &&
                    (message["group"] != groupId || !isActiveWindow()))
                {
                    const auto group = message["group"].toString();
                    obj["unread"] = ++unread[group];
                    // The Dock badge derives its total from this subset and the current level, so
                    // the quieter levels can drop these messages and a later switch back to "all
                    // messages" still restores them. The list marker above stays level-independent.
                    if (ChatPreferences::mentionsSelf(message["mentions"].toArray(), selfMemberId))
                        ++unreadMentions[group];
                    updateUnreadTotal();
                }
                item->setData(Qt::UserRole, obj);
                break;
            }
        }
        if (!historical)
            sortGroups();
        maybeNotifyMessage(event, firstSeen);
    }
}
void Window::refreshAttachments()
{
    while (auto *item = attachmentLayout->takeAt(0))
    {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    attachmentArea->setVisible(!attachments.items.isEmpty());
    attachmentCount->setText(
        attachments.items.isEmpty() ? "" : QString("%1 个待发送附件").arg(attachments.items.size()));
    for (int i = 0; i < attachments.items.size(); ++i)
    {
        auto item = attachments.items[i];
        auto *card = new QFrame;
        card->setObjectName("attachmentCard");
        card->setFixedSize(230, 64);
        auto *row = new QHBoxLayout(card);
        row->setContentsMargins(12, 9, 8, 9);
        auto *type = new QLabel(item.kind == "gif" ? "GIF" : item.kind == "video" ? "▶" : "▤");
        type->setObjectName("fileType");
        row->addWidget(type);
        auto *labels = new QVBoxLayout;
        labels->setSpacing(4);
        auto *name = new QLabel(QFontMetrics(font()).elidedText(item.name, Qt::ElideMiddle, 134));
        name->setAccessibleDescription(item.path);
        auto *size = new QLabel(AttachmentStore::sizeLabel(item.size));
        size->setObjectName("footnote");
        labels->addWidget(name);
        labels->addWidget(size);
        row->addLayout(labels, 1);
        auto *remove = button("×", "removeAttachment");
        remove->setFixedSize(22, 26);
        remove->setEnabled(!busy);
        connect(remove, &QPushButton::clicked, this, [this, i] { attachments.remove(i); });
        row->addWidget(remove);
        attachmentLayout->addWidget(card);
    }
    updateSendButton();
    updateComposeHeight();
}
void Window::updateSendButton()
{
    sendButton->setEnabled(canSend && ready && connected && !busy && !groupId.isEmpty() &&
                           (!composer->toPlainText().trimmed().isEmpty() || !attachments.items.isEmpty()));
}
void Window::quoteMessage(const QJsonObject &requested)
{
    if (busy || groupId.isEmpty())
        return;
    auto message = requested;
    for (const auto &row : model->rows)
        if (!requested.isEmpty() && row["id"] == requested["id"])
            message = row;
    if (!message.isEmpty() && (message["id"].toString().isEmpty() || message["status"] == "recalled" ||
                               (!message["group"].toString().isEmpty() && message["group"] != groupId)))
        return;
    quotedMessage =
        message.isEmpty()
            ? QJsonObject{}
            : QJsonObject{{"id", message["id"]}, {"sender", message["sender"]},
                          {"text", messageQuoteText(message)}};
    pendingId.clear();
    refreshQuote();
    saveDraft();
    composer->setFocus();
}
void Window::refreshQuote()
{
    if (!quoteArea)
        return;
    const bool visible = !quotedMessage.value("id").toString().isEmpty();
    quoteArea->setVisible(visible);
    if (visible)
    {
        const auto text = messageQuoteLabel(quotedMessage);
        quoteLabel->setText(QFontMetrics(quoteLabel->font())
                                .elidedText(text, Qt::ElideRight, qMax(40, quoteArea->width() - 40)));
        quoteLabel->setAccessibleName(text);
    }
    cancelQuoteButton->setEnabled(!busy);
    updateComposeHeight();
}
void Window::updateComposeHeight()
{
    if (auto *compose = findChild<QWidget *>("composeArea"))
    {
        const int minimum = qMax(100, compose->layout()->minimumSize().height());
        const int maximum = qMax(minimum, height() * 65 / 100);
        const int requested = composeHeightPreference > 0 ? composeHeightPreference : qMin(303, height() * 24 / 100);
        compose->setFixedHeight(qBound(minimum, requested, maximum));
    }
}
void Window::recallMessage(const QJsonObject &message)
{
    if (!messages->recallSupported())
        return;
    const auto id = message["id"].toString();
    QJsonObject current;
    for (const auto &row : model->rows)
        if (row["id"] == id)
            current = row;
    if (!canRecallMessage(current, QDateTime::currentMSecsSinceEpoch()))
    {
        notify("该消息当前无法撤回，请核对发送状态和三分钟时限", true);
        return;
    }
    cacheMessageDraft(current);
    transport.post("/api/messages/" + QString::fromLatin1(QUrl::toPercentEncoding(id)) + "/recall", {},
                   [this](const QJsonValue &value, const QString &error)
                   {
                       if (!error.isEmpty())
                       {
                           notify(error, true);
                           return;
                       }
                       const auto message = value.toObject();
                       if (!message["id"].toString().isEmpty())
                           handleEvent({{"type", "message"}, {"message", message}});
                   });
}
void Window::cacheMessageDraft(const QJsonObject &message)
{
    const auto id = message["id"].toString();
    if (!recallDrafts.get(id, groupId).isEmpty())
        return;
    AttachmentStore local;
    for (const auto &entry : message["media"].toArray())
    {
        const auto media = entry.toObject();
        const auto path = media["_local"].toString(localFiles->find(groupId, media));
        if (!path.isEmpty() && QFileInfo::exists(path))
            local.addFiles({path});
    }
    recallDrafts.remember(id, groupId,
                          {{"text", message["text"]},
                           {"quote", message["quote"].toObject()},
                           {"mentions", QJsonArray{}},
                           {"attachments", local.serialize()}});
}
void Window::refreshReeditLinks()
{
    for (const auto &message : model->rows)
        if (message["status"] == "recalled")
            model->setCanReedit(message["id"].toString(),
                                !recallDrafts.get(message["id"].toString(), groupId).isEmpty());
}
void Window::reeditMessage(const QString &id)
{
    if (busy || groupId.isEmpty())
        return;
    bool recalled = false;
    for (const auto &message : model->rows)
        if (message["id"] == id && message["status"] == "recalled" && message["outgoing"].toBool())
            recalled = true;
    if (!recalled)
        return;
    auto draft = recallDrafts.get(id, groupId);
    if (draft.isEmpty())
    {
        model->setCanReedit(id, false);
        notify("本地原稿已过期或附件不可用", true);
        return;
    }
    if (draft["text"].toString().isEmpty() && draft["attachments"].toArray().isEmpty())
    {
        recallDrafts.consume(id);
        model->setCanReedit(id, false);
        notify("原稿附件超过本地缓存上限或已不可用，当前草稿已保留", true);
        return;
    }
    auto media = attachments.serialize();
    for (const auto &entry : draft["attachments"].toArray())
    {
        const auto item = entry.toObject();
        bool duplicate = false;
        for (const auto &existing : media)
            duplicate |= existing.toObject()["path"] == item["path"];
        if (!duplicate)
            media.append(item);
    }
    const auto previous = composer->toPlainText();
    const auto separator = previous.isEmpty() || previous.endsWith('\n') ? QString() : QString("\n");
    const auto restored = previous + separator + draft["text"].toString();
    if (media.size() > 12 || restored.toUtf8().size() > 24000)
    {
        notify("当前草稿空间不足，请先处理后再重新编辑；原稿已保留", true);
        return;
    }
    const auto savedMedia = draft["attachments"].toArray();
    if (!recallDrafts.release(id, draft))
    {
        notify("暂时无法恢复原稿附件，当前草稿和原稿均已保留", true);
        return;
    }
    const auto releasedMedia = draft["attachments"].toArray();
    for (int i = 0; i < media.size(); ++i)
        for (int j = 0; j < savedMedia.size(); ++j)
            if (media[i].toObject()["path"] == savedMedia[j].toObject()["path"])
                media[i] = releasedMedia[j];
    auto mentions = composer->mentions();
    for (const auto &entry : draft["mentions"].toArray())
    {
        auto token = entry.toObject();
        token["_start"] = token["_start"].toInt() + previous.size() + separator.size();
        mentions.append(token);
    }
    restoring = true;
    composer->restore(restored, mentions);
    attachments.restore(media);
    const bool keptQuote = !quotedMessage.isEmpty();
    if (!keptQuote)
    {
        quotedMessage = draft["quote"].toObject();
        const auto original = quotedMessage["id"].toString();
        bool unavailable = unavailableOriginals.contains(original);
        for (const auto &message : model->rows)
            unavailable |= (message["id"] == original && message["status"] == "recalled") ||
                           (message["quote"].toObject()["id"] == original &&
                            message["quote"].toObject()["unavailable"].toBool());
        if (unavailable)
        {
            quotedMessage["unavailable"] = true;
            quotedMessage["text"] = "";
        }
    }
    pendingId.clear();
    refreshQuote();
    restoring = false;
    recallDrafts.consume(id);
    model->setCanReedit(id, false);
    saveDraft();
    composer->setFocus();
    notify(draft["_missingAttachments"].toInt() ? "已恢复文字和可用附件；部分附件超出本地缓存上限或已不可用"
           : keptQuote && !draft["quote"].toObject().isEmpty() ? "已追加原稿并保留当前引用"
                                                               : "已恢复原稿，可继续编辑");
}
void Window::refreshQuoteSource(const QJsonObject &quote)
{
    const auto replyId = quote["_reply_id"].toString();
    const auto id = quote["id"].toString();
    if (replyId.isEmpty() || replyId.size() > 512 || !locatingId.isEmpty() || groupId.isEmpty())
        return;
    const bool currentReply = std::any_of(model->rows.cbegin(), model->rows.cend(), [&](const QJsonObject &row)
    {
        return row["id"] == replyId && row["group"] == groupId && row["status"] != "recalled" &&
               row["quote"].toObject()["id"] == id;
    });
    if (!currentReply)
        return;
    const auto group = groupId;
    const auto conversation = conversationGeneration;
    const auto generation = ++locateGeneration;
    locatingId = id;
    QUrlQuery query;
    query.addQueryItem("group", group);
    transport.post("/api/messages/" + QString::fromLatin1(QUrl::toPercentEncoding(replyId)) +
        "/quote-source?" + query.toString(QUrl::FullyEncoded), {},
        [this, replyId, group, conversation, generation](const QJsonValue &value, const QString &error)
        {
            if (group != groupId || conversation != conversationGeneration || generation != locateGeneration)
                return;
            locatingId.clear();
            const auto response = value.toObject();
            const auto updated = response["message"].toObject();
            const auto original = response["original"].toObject();
            auto source = updated["quote"].toObject();
            const auto originalId = original["id"].toString();
            if (!error.isEmpty() || updated["id"] != replyId || updated["group"] != group ||
                updated["status"] == "recalled" || source["unavailable"].toBool() ||
                originalId.isEmpty() || originalId.startsWith("unavailable-") || source["id"] != originalId ||
                original["group"] != group || original["status"] == "recalled" ||
                original["seq"].toInteger() <= 0 || unavailableOriginals.contains(originalId))
            {
                notify(error.isEmpty() ? "引用原消息尚未同步或不可用" : error, true);
                return;
            }
            model->upsert(localFiles->hydrate(updated, group));
            source["group"] = group;
            locateQuote(source);
        }, 18000);
}
void Window::locateQuote(const QJsonObject &quote)
{
    const auto id = quote["id"].toString();
    if (id.isEmpty() || id.size() > 512 || groupId.isEmpty() || !locatingId.isEmpty() ||
        (!quote["group"].toString().isEmpty() && quote["group"] != groupId))
        return;
    if (quote["unavailable"].toBool())
    {
        clearPendingRecord(id);
        notify("原消息已撤回或不可用，无法定位", true);
        return;
    }
    for (const auto &message : model->rows)
        if (message["id"] == id)
        {
            if (message["status"] == "recalled")
            {
                clearPendingRecord(id);
                notify("原消息已撤回，无法定位", true);
            }
            else
                messages->locateMessage(id);
            return;
        }
    if (id.startsWith("unavailable-") && !quote["_reply_id"].toString().isEmpty())
    {
        refreshQuoteSource(quote);
        return;
    }
    const auto group = groupId;
    const auto generation = ++locateGeneration;
    const auto conversation = conversationGeneration;
    locatingId = id;
    QUrlQuery sourceQuery;
    sourceQuery.addQueryItem("group", group);
    transport.get(
        "/api/messages/" + QString::fromLatin1(QUrl::toPercentEncoding(id)) + '?' +
            sourceQuery.toString(QUrl::FullyEncoded),
        [this, id, group, generation, conversation, quote](const QJsonValue &value, const QString &error)
        {
            if (generation != locateGeneration || conversation != conversationGeneration || group != groupId)
                return;
            const auto original = value.toObject();
            if (!error.isEmpty() || original["id"] != id || original["group"] != group ||
                original["status"] == "recalled" || original["seq"].toInteger() <= 0 ||
                unavailableOriginals.contains(id))
            {
                locatingId.clear();
                if (!quote["_reply_id"].toString().isEmpty() && !unavailableOriginals.contains(id) &&
                    original["status"] != "recalled")
                {
                    refreshQuoteSource(quote);
                    return;
                }
                clearPendingRecord(id);
                notify(error.isEmpty() ? "原消息已撤回或不可用，无法定位" : error, true);
                return;
            }
            QUrlQuery query;
            query.addQueryItem("group", group);
            query.addQueryItem("before", QString::number(original["seq"].toInteger()));
            transport.get(
                "/api/messages?" + query.toString(QUrl::FullyEncoded),
                [this, id, group, generation, conversation, original](const QJsonValue &page,
                                                                      const QString &pageError)
                {
                    if (generation != locateGeneration || conversation != conversationGeneration ||
                        group != groupId)
                        return;
                    locatingId.clear();
                    if (unavailableOriginals.contains(id))
                    {
                        clearPendingRecord(id);
                        notify("原消息已撤回，无法定位", true);
                        return;
                    }
                    if (!pageError.isEmpty())
                    {
                        clearPendingRecord(id);
                        notify(pageError, true);
                        return;
                    }
                    auto context = page.toArray();
                    for (int i = context.size() - 1; i >= 0; --i)
                        if (context[i].toObject()["group"] != group)
                            context.removeAt(i);
                    hasOlder = !context.isEmpty();
                    context.append(original);
                    ++historyGeneration;
                    loading = true;
                    historyUpdates.clear();
                    messages->setProperty("locatedHistory", true);
                    messages->stopScrollAnimation();
                    recoverCachedMessages(context, [this, id, group, generation, conversation, context]() mutable
                    {
                        if (group != groupId || conversation != conversationGeneration || generation != locateGeneration)
                            return;
                        loading = false;
                        for (int row = 0; row < context.size(); ++row)
                        {
                            const auto key = context[row].toObject()["id"].toString();
                            if (historyUpdates.contains(key)) context[row] = historyUpdates[key];
                        }
                        historyUpdates.clear();
                        model->replace(hydrateMessages(context));
                        refreshReeditLinks();
                        if (unavailableOriginals.contains(id)) { clearPendingRecord(id); return; }
                        QTimer::singleShot(0, this, [this, id, group, conversation]
                        {
                            if (group == groupId && conversation == conversationGeneration)
                            {
                                messages->locateMessage(id);
                                playLocatedRecord(id);
                            }
                        });
                    });
                    notify("已定位原文；向下滚动到底或按 End 返回最新消息");
                },
                10000);
        },
        10000);
}
void Window::send()
{
    if (!canSend)
    {
        notify("服务端未授予此客户端发送权限，草稿已保留", true);
        return;
    }
    if (busy || groupId.isEmpty() ||
        (composer->toPlainText().trimmed().isEmpty() && attachments.items.isEmpty()))
        return;
    if (!ready || !connected)
    {
        notify("服务器尚未就绪，消息保留在输入区", true);
        return;
    }
    auto error = attachments.validate();
    if (!error.isEmpty())
    {
        notify(error, true);
        return;
    }
    busy = true;
    cancelQuoteButton->setEnabled(false);
    attachments.setLocked(true);
    composer->setReadOnly(true);
    sendButton->setEnabled(false);
    sendButton->setText("发送中…");
    refreshAttachments();
    if (pendingId.isEmpty())
        pendingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject saved{{"text", composer->toPlainText()},
                      {"mentions", composer->mentions()},
                      {"quote", quotedMessage},
                      {"attachments", attachments.serialize()}};
    recallDrafts.remember(pendingId, groupId, saved);
    saveDraft();
    uploadNext(0, ++sendGeneration, groupId);
}
void Window::uploadNext(int index, quint64 generation, const QString &targetGroup)
{
    if (generation != sendGeneration || targetGroup != groupId || !busy)
        return;
    if (!canSend)
    {
        cancelSend();
        saveDraft();
        notify("发送权限已被服务端取消，附件和文字保留在草稿中", true);
        return;
    }
    auto failure = [this, generation, targetGroup](const QString &error)
    {
        if (generation != sendGeneration || targetGroup != groupId)
            return;
        busy = false;
        cancelQuoteButton->setEnabled(true);
        attachments.setLocked(false);
        composer->setReadOnly(false);
        sendButton->setText("发送");
        refreshAttachments();
        saveDraft();
        notify(error + "；内容已保留，可再次发送核对结果", true);
    };
    if (index < attachments.items.size())
    {
        auto &item = attachments.items[index];
        if (!item.uploaded.isEmpty())
        {
            uploadNext(index + 1, generation, targetGroup);
            return;
        }
        transport.upload(
            targetGroup, item.path, item.name,
            [this, index, failure, generation, targetGroup](const QJsonValue &value, const QString &error)
            {
                if (generation != sendGeneration || targetGroup != groupId || !busy ||
                    index >= attachments.items.size())
                    return;
                if (!error.isEmpty())
                {
                    failure(error);
                    return;
                }
                attachments.items[index].uploaded = value["id"].toString();
                saveDraft();
                uploadNext(index + 1, generation, targetGroup);
            });
        return;
    }
    QJsonArray media;
    for (const auto &item : attachments.items)
        media.append(item.uploaded);
    QJsonObject body{{"id", pendingId},
                     {"group", targetGroup},
                     {"client_id", clientId},
                     {"sender", nickname},
                     {"text", composer->toPlainText()},
                     {"mentions", composer->mentions()},
                     {"media", media}};
    if (!quotedMessage.isEmpty())
        body["quote"] = quotedMessage;
    transport.post("/api/messages", body,
                   [this, failure, generation, targetGroup](const QJsonValue &value, const QString &error)
                   {
                       if (generation != sendGeneration || targetGroup != groupId || !busy)
                           return;
                       if (!error.isEmpty())
                       {
                           failure(error);
                           return;
                       }
                       const auto response = localFiles->hydrate(value.toObject(), targetGroup);
                       // A WebSocket confirmation can arrive before the original queued POST reply.
                       const bool alreadyAdvanced = response["status"] == "queued" && std::any_of(
                           model->rows.cbegin(), model->rows.cend(), [&response](const QJsonObject &row)
                           {
                               const auto status = row["status"].toString();
                               return row["id"] == response["id"] && row["group"] == response["group"] &&
                                   !status.isEmpty() && status != "queued";
                           });
                       if (!alreadyAdvanced) model->upsert(response);
                       messages->scrollToBottom();
                       restoring = true;
                       composer->clear();
                       attachments.clear();
                       pendingId.clear();
                       quotedMessage = {};
                       refreshQuote();
                       restoring = false;
                       busy = false;
                       cancelQuoteButton->setEnabled(true);
                       attachments.setLocked(false);
                       composer->setReadOnly(false);
                       sendButton->setText("发送");
                       refreshAttachments();
                       saveDraft();
                       transferLabel->clear();
                       composer->setFocus();
                   });
}
QString Window::draftKey() const
{
    return "drafts/" +
           QString::fromLatin1(
               QCryptographicHash::hash((secret + groupId).toUtf8(), QCryptographicHash::Sha256).toHex());
}
void Window::saveDraft()
{
    if (groupId.isEmpty() || restoring)
        return;
    QJsonObject object{{"text", composer->toPlainText()},
                       {"mentions", composer->mentions()},
                       {"attachments", attachments.serialize()},
                       {"id", pendingId}};
    if (!quotedMessage.isEmpty())
        object["quote"] = quotedMessage;
    QSettings().setValue(draftKey(), QJsonDocument(object).toJson(QJsonDocument::Compact));
}
void Window::restoreDraft()
{
    restoring = true;
    auto object = QJsonDocument::fromJson(QSettings().value(draftKey()).toByteArray()).object();
    composer->restore(object["text"].toString(), object["mentions"].toArray());
    attachments.restore(object["attachments"].toArray());
    pendingId = object["id"].toString();
    quotedMessage = object["quote"].toObject();
    refreshQuote();
    restoring = false;
    composer->setFocus();
}
void Window::notify(const QString &text, bool error)
{
    if (text.startsWith("演示模式"))
    {
        demoLabel->setText("演示模式");
        return;
    }
    if (error)
        lastError = text.left(1200);
    // Every failure the user needs to act on becomes visible. Previously this only
    // recorded the string, so a rejected certificate or an unreachable host looked
    // exactly like a working client that simply never updated.
    showBanner(text, error);
}

void Window::setConnectionState(const QString &text, bool reconnecting)
{
    // Both "retrying" and "blocked" need a way out: a failing address must be
    // editable without hunting for the tray menu, so the strip always offers
    // settings and keeps retry as the primary action while it still retries.
    showBanner(text, true, reconnecting ? "retry" : "settings",
               reconnecting ? "重试" : "打开设置");
    if (preferencesDialog)
        preferencesDialog->setConnectionState(ready, reconnecting, text);
}

void Window::showBanner(const QString &text, bool error, const QString &action,
                        const QString &actionText)
{
    if (!banner || text.trimmed().isEmpty())
        return;
    const auto message = text.left(600);
    // Repeated connection failures arrive on every retry; keep the strip stable instead
    // of repainting it, and never let a routine notice replace a connection error.
    if (bannerError && !error)
        return;
    if (banner->isVisible() && bannerLabel->text() == message &&
        bannerAction->property("action").toString() == action)
        return;
    bannerLabel->setText(message);
    bannerAction->setText(actionText);
    bannerAction->setProperty("action", action);
    bannerAction->show();
    // Only shown alongside "重试", where it is the escape hatch for a wrong address.
    bannerRetrySettings->setVisible(action == "retry");
    bannerError = error;
    banner->setProperty("error", error);
    banner->style()->unpolish(banner);
    banner->style()->polish(banner);
    banner->show();
    banner->raise();
    // Layout is only final after show(); re-place on the next event-loop turn so the
    // width/height derived from the compose pane are not one frame stale.
    positionBanner();
    QTimer::singleShot(0, this, [this] { positionBanner(); });
}

void Window::hideBanner()
{
    if (!banner)
        return;
    banner->hide();
}

void Window::positionBanner()
{
    if (!banner || !banner->isVisible())
        return;
    // setFixedWidth/setFixedHeight below can re-enter through layout activation. The
    // guard keeps that from becoming recursion; one placement per change is enough.
    if (positioningBanner)
        return;
    positioningBanner = true;
    const QScopeGuard release([this] { positioningBanner = false; });
    // Width follows the conversation pane, and the strip sits just above the compose
    // box so it never covers the newest message or the send button.
    const auto *composeArea = findChild<QWidget *>("composeArea");
    const int margin = 14;
    const auto right = composeArea ? mapFromGlobal(composeArea->mapToGlobal(QPoint(composeArea->width(), 0))).x()
                                   : width() - margin;
    const auto left = composeArea ? mapFromGlobal(composeArea->mapToGlobal(QPoint(0, 0))).x() : margin;
    // Span the conversation pane rather than a fixed narrow toast: long but actionable
    // causes (certificate, wrong port) must stay readable without being cut off.
    const int bannerWidth = qMax(240, right - left - margin * 2);
    banner->setFixedWidth(bannerWidth);
    // A wrapped QLabel does not report a trustworthy sizeHint, so drive the height from
    // the layout's heightForWidth with the width we just fixed.
    if (auto *bannerLayout = banner->layout())
    {
        bannerLayout->activate();
        const int height = bannerLayout->hasHeightForWidth()
                               ? bannerLayout->heightForWidth(bannerWidth)
                               : bannerLayout->sizeHint().height();
        banner->setFixedHeight(qMax(30, height));
    }
    const int bottom = composeArea
                           ? mapFromGlobal(composeArea->mapToGlobal(QPoint(0, 0))).y() - 10
                           : height() - margin;
    banner->move(left + margin, qMax(margin, bottom - banner->height()));
}
void Window::copyMedia(const QJsonObject &media)
{
    if (!canDownload || !hasCurrentMedia(media))
        return;
    pendingMediaCopy = media;
    // Use the existing download/save queue and a durable file, keeping GIFs and other originals intact.
    fetchMedia(media, true);
}
void Window::copyMediaToClipboard(const QJsonObject &media, const QString &path)
{
    if (!canDownload || !sameMediaDescriptor(pendingMediaCopy, media) || !hasCurrentMedia(media) ||
        !QFileInfo(path).isFile())
        return;
    pendingMediaCopy = {};
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    if (media["kind"] == "image")
    {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const auto size = reader.size();
        if (size.isValid() && qint64(size.width()) * size.height() <= 4 * 1024 * 1024)
        {
            const auto image = reader.read();
            if (!image.isNull()) mime->setImageData(image);
        }
    }
    QApplication::clipboard()->setMimeData(mime);
}
void Window::fetchMedia(const QJsonObject &media, bool saveOnly, bool thumbnail, bool resumed)
{
    if (!canDownload || !hasCurrentMedia(media))
        return;
    const auto id = media["id"].toString();
    if (saveOnly) saveRequests.insert(id);
    const bool voice = media["kind"] == "voice" && !saveOnly && !thumbnail;
    if (!voice && !saveOnly && !thumbnail)
        openRequests.insert(id);
    if (voice && !resumed)
    {
        if (requestedVoiceId == id)
        {
            stopVoice();
            return;
        }
        stopVoice();
        requestedVoiceId = id;
        voiceGroup = groupId;
        voiceConversation = conversationGeneration;
        messages->setVoiceState(id, "loading");
    }
    if (resumed && voice && requestedVoiceId != id) return;
    if (downloading.contains(id) || (!resumed && queuedMediaIds.contains(id))) return;
    const auto local = localFiles->find(groupId, media);
    const bool cached = !local.isEmpty();
    const bool copy = cached && MediaStorage::isCachePath(local) && (saveOnly || (!thumbnail && !voice));
    if ((!cached || copy) && (cacheWorking || downloading.size() >= 2))
    {
        if (!thumbnail && mediaQueue.size() < 16)
        {
            const auto scope = chatPreferences.scope();
            const auto conversation = conversationGeneration;
            queuedMediaIds.insert(id);
            mediaQueue.enqueue([this, media, id, thumbnail, scope, conversation]
            {
                queuedMediaIds.remove(id);
                if (scope == chatPreferences.scope() && conversation == conversationGeneration && canDownload)
                {
                    const bool wantsVoice = media["kind"] == "voice" && requestedVoiceId == id;
                    const bool wantsOpen = openRequests.contains(id);
                    if (wantsVoice || wantsOpen || saveRequests.contains(id))
                        fetchMedia(media, !wantsVoice && !wantsOpen, thumbnail, true);
                }
                else { openRequests.remove(id); saveRequests.remove(id); }
            });
        }
        return;
    }
    if (cached)
    {
        if (saveOnly || (!thumbnail && !voice))
        {
            const bool requestedOpen = openRequests.remove(id);
            saveRequests.remove(id);
            saveMediaAndMaybeOpen(media, local, requestedOpen || !saveOnly);
        }
        else
        {
            model->setLocalMedia(id, local, media);
            refreshForwardRecord();
            if (voice) openMedia(media, local);
        }
        return;
    }
    downloading.insert(id);
    const auto scope = chatPreferences.scope();
    const auto group = groupId;
    const auto files = localFiles;
    transport.download(media, (thumbnail || voice) && !saveOnly ? MediaStorage::cacheDirectory() : downloadRoot,
                       [this, id, media, thumbnail, scope, group, files](const QString &path, const QString &error)
                       {
                           downloading.remove(id);
                           QTimer::singleShot(0, this, &Window::drainMediaQueue);
                           if (scope != chatPreferences.scope() || localFiles != files || !canDownload) return;
                           transferLabel->clear();
                           if (!error.isEmpty())
                           {
                               lastError = error;
                               openRequests.remove(id);
                               saveRequests.remove(id);
                               if (sameMediaDescriptor(pendingMediaCopy, media)) pendingMediaCopy = {};
                               if (requestedVoiceId == id)
                               {
                                   stopVoice();
                                   messages->setVoiceState(id, "error");
                               }
                               return;
                           }
                           files->remember(group, media, path);
                           if (group == groupId) model->setLocalMedia(id, path, media);
                           if (group == groupId) refreshForwardRecord();
                           const bool requestedOpen = openRequests.remove(id);
                           const bool requestedSave = saveRequests.remove(id);
                           if (requestedOpen || requestedSave)
                               saveMediaAndMaybeOpen(media, path, requestedOpen);
                           else if (media["kind"] == "voice" && requestedVoiceId == id)
                               openMedia(media, path);
                           else if (thumbnail)
                               thumbnailTimer.start();
                           cachePruneTimer.start();
                       });
}
void Window::drainMediaQueue()
{
    while (!cacheWorking && downloading.size() < 2 && !mediaQueue.isEmpty())
        mediaQueue.dequeue()();
}
void Window::saveMediaAndMaybeOpen(const QJsonObject &media, const QString &path, bool open)
{
    if (!MediaStorage::isCachePath(path))
    {
        model->setLocalMedia(media["id"].toString(), path, media);
        refreshForwardRecord();
        copyMediaToClipboard(media, path);
        if (open || (media["kind"] == "voice" && requestedVoiceId == media["id"].toString() &&
                     playingVoiceId != requestedVoiceId)) openMedia(media, path);
        return;
    }
    const auto id = media["id"].toString();
    downloading.insert(id);
    const auto root = downloadRoot;
    const auto scope = chatPreferences.scope();
    const auto conversation = conversationGeneration;
    QPointer<Window> self(this);
    QThreadPool::globalInstance()->start([self, media, path, root, open, id, scope, conversation]
    {
        QString error;
        const auto saved = MediaStorage::saveCached(path, root, media["name"].toString(), error);
        QMetaObject::invokeMethod(qApp, [self, media, saved, error, open, id, root, scope, conversation]
        {
            if (!self) return;
            self->downloading.remove(id);
            QTimer::singleShot(0, self, &Window::drainMediaQueue);
            self->cachePruneTimer.start();
            if (scope != self->chatPreferences.scope() || conversation != self->conversationGeneration ||
                root != self->downloadRoot || !self->canDownload) return;
            const bool requestedOpen = self->openRequests.remove(id);
            self->saveRequests.remove(id);
            if (!error.isEmpty())
            {
                self->lastError = error;
                if (sameMediaDescriptor(self->pendingMediaCopy, media)) self->pendingMediaCopy = {};
                return;
            }
            self->localFiles->remember(self->groupId, media, saved);
            self->model->setLocalMedia(id, saved, media);
            self->refreshForwardRecord();
            self->copyMediaToClipboard(media, saved);
            if (open || requestedOpen || (media["kind"] == "voice" && self->requestedVoiceId == id &&
                                         self->playingVoiceId != id))
                self->openMedia(media, saved);
        });
    });
}
void Window::visibleThumbnails()
{
    if (!canDownload || !connected || isMinimized() || !isVisible() || cacheWorking)
        return;
    for (const auto &pair : {qMakePair(messages,model),qMakePair(forwardDialog ? forwardView : nullptr, forwardDialog ? forwardModel : nullptr)})
    {
        auto *view=pair.first; auto *source=pair.second;
        if (!view || !source || !view->isVisible()) continue;
        const auto bounds=view->viewport()->rect();
        const auto first=view->indexAt(QPoint(1,0));
        for (int i=qMax(0,first.row()); i<source->rowCount(); ++i)
        {
            if (source->rows[i]["status"] == "recalled") continue;
            const auto rect=view->visualRect(source->index(i));
            if (rect.top()>bounds.bottom()) break;
            if (!rect.intersects(bounds)) continue;
            if (source->rows[i]["record"].isObject()) continue;
            for (const auto &entry : source->rows[i]["media"].toArray())
            {
                auto media=entry.toObject(); const auto kind=media["kind"].toString();
                if (kind=="video") fetchVideoPoster(media);
                if ((kind=="image" || kind=="gif") && media["size"].toInteger()<=8*1024*1024 &&
                    (media["_local"].toString().isEmpty() || !QFileInfo::exists(media["_local"].toString()))) fetchMedia(media,false,true);
            }
        }
    }
}
void Window::fetchVideoPoster(const QJsonObject &media)
{
    const auto id = media["id"].toString();
    if (id.isEmpty() || pendingPosters.size() >= 2 || pendingPosters.contains(id) || failedPosters.contains(id)) return;
    const auto path = localFiles->findPoster(groupId, media);
    if (!path.isEmpty()) { model->setLocalPoster(id, path, media); refreshForwardRecord(); return; }
    pendingPosters.insert(id);
    const auto files = localFiles;
    const auto scope = chatPreferences.scope();
    const auto conversation = conversationGeneration;
    const auto permission = permissionsGeneration;
    const auto group = groupId;
    transport.downloadPoster(media, MediaStorage::cacheDirectory(),
        [this, media, id, files, scope, conversation, permission, group](const QString &path, const QString &error)
        {
            pendingPosters.remove(id);
            if (scope != chatPreferences.scope() || files != localFiles || !canDownload) return;
            if (permission != permissionsGeneration) { thumbnailTimer.start(); return; }
            if (error.isEmpty()) files->rememberPoster(group, media, path);
            if (conversation == conversationGeneration && group == groupId)
            {
                if (error.isEmpty()) { model->setLocalPoster(id, path, media); refreshForwardRecord(); updateCachePins(); }
                else { if (failedPosters.size() >= 256) failedPosters.erase(failedPosters.begin()); failedPosters.insert(id); }
            }
            thumbnailTimer.start();
            cachePruneTimer.start();
        });
}
void Window::stopVoice()
{
    requestedVoiceId.clear();
    playingVoiceId.clear();
    playingVoicePath.clear();
    voiceGroup.clear();
    if (voicePlayer)
        voicePlayer->stop();
    messages->setVoiceState({}, {});
    updateCachePins();
}
void Window::openMedia(const QJsonObject &media, const QString &path)
{
    if (!canDownload || !hasCurrentMedia(media))
        return;
    if (media["kind"] == "voice")
    {
        const auto id = media["id"].toString();
        if (requestedVoiceId != id || voiceGroup != groupId ||
            voiceConversation != conversationGeneration || !canDownload || !isVisible() || isMinimized())
            return;
        playingVoiceId = id;
        playingVoicePath = path;
        updateCachePins();
        if (voicePlayer->play(path))
            messages->setVoiceState(id, "playing");
        else
        {
            requestedVoiceId.clear();
            playingVoiceId.clear();
            playingVoicePath.clear();
            messages->setVoiceState(id, "error");
        }
        return;
    }
    const QFileInfo file(path);
    if (file.isFile() && !QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath())))
        lastError = "系统未能打开此文件：" + file.fileName();
}
void Window::closeEvent(QCloseEvent *event)
{
    if (!isMinimized()) QSettings().setValue("windowBounds", isMaximized() ? normalGeometry() : geometry());
    stopVoice();
    saveDraft();
    storeCachedMessages();
    if (!quitting && demoServer.state() != QProcess::NotRunning && QSystemTrayIcon::isSystemTrayAvailable())
    {
        hide();
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}
void Window::dragEnterEvent(QDragEnterEvent *event)
{
    if (!busy && !groupId.isEmpty() && (event->mimeData()->hasUrls() || event->mimeData()->hasImage()))
        event->acceptProposedAction();
}
void Window::dropEvent(QDropEvent *event)
{
    if (busy || groupId.isEmpty())
        return;
    if (event->mimeData()->hasUrls())
    {
        QStringList paths;
        for (const auto &url : event->mimeData()->urls())
            if (url.isLocalFile())
                paths << url.toLocalFile();
        attachments.addFiles(paths);
    }
    else if (event->mimeData()->hasImage())
        attachments.addImage(event->mimeData());
    event->acceptProposedAction();
}
