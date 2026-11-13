#include "preferences_dialog.h"
#include "connection_info.h"
#include "media_storage.h"
#include "update_widget.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPalette>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
QLabel *description(const QString &text, QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    auto font = label->font();
    font.setPixelSize(12);
    label->setFont(font);
    return label;
}

QVBoxLayout *pageLayout(QWidget *page, const QString &heading)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 8);
    layout->setSpacing(14);
    auto *title = new QLabel(heading);
    auto font = title->font();
    font.setPixelSize(18);
    title->setFont(font);
    layout->addWidget(title);
    layout->addSpacing(10);
    return layout;
}

QString byteSize(qint64 bytes)
{
    return bytes < 0 ? QString("暂不可读") : QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat);
}

QString cleanError(QString error)
{
    for (auto &character : error)
        if (character.isSpace() || character.category() == QChar::Other_Control)
            character = QLatin1Char(' ');
    return error.simplified().left(240);
}
}

PreferencesDialog::PreferencesDialog(const Values &initial, const Connection &connection,
                                     QWidget *parent)
    : QDialog(parent, Qt::Window), p2p(connection.p2p), connected(connection.connected),
      reconnecting(connection.reconnecting)
{
    setObjectName("preferencesDialog");
    setWindowTitle("设置");
    resize(730, 570);
    setMinimumSize(650, 470);
    // All colors resolve through the application's palette, including dark mode.
    setStyleSheet(
        "#preferencesNavigation { border:0; outline:0; background:palette(window); }"
        "#preferencesNavigation::item { height:38px; margin:2px 8px; padding-left:14px; border-radius:5px; }"
        "#preferencesNavigation::item:selected { background:palette(button); color:palette(button-text); }"
        "#preferencesNavigation::item:hover { background:palette(alternate-base); }");
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // Inherit the item stylesheet before the first layout caches row heights.
    navigation = new QListWidget(this);
    navigation->setObjectName("preferencesNavigation");
    navigation->setFixedWidth(128);
    navigation->setFrameShape(QFrame::NoFrame);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation->setContentsMargins(0, 16, 0, 0);
    navigation->addItems({"连接", "存储", "外观", "通知", "更新"});
    outer->addWidget(navigation);
    auto *divider = new QFrame;
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedWidth(1);
    outer->addWidget(divider);
    auto *right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(0);
    auto *pages = new QStackedWidget;
    pages->setObjectName("preferencesPages");
    right->addWidget(pages, 1);
    outer->addLayout(right, 1);

    // ---- 连接 -------------------------------------------------------------
    auto *connectionPage = new QWidget;
    connectionPage->setObjectName("preferencesConnectionPage");
    auto *link = pageLayout(connectionPage, "连接");

    connectionStatus = new QLabel;
    connectionStatus->setObjectName("preferencesConnectionStatus");
    connectionStatus->setTextFormat(Qt::PlainText);
    connectionStatus->setWordWrap(true);
    link->addWidget(connectionStatus);

    connectionMode = new QComboBox;
    connectionMode->setObjectName("preferencesConnectionMode");
    connectionMode->addItem("直接地址", "direct");
    connectionMode->addItem("自动连接（直连优先）", "p2p");
    connectionMode->setCurrentIndex(connection.mode == "p2p" ? 1 : 0);

    serverUrl = new QLineEdit(connection.serverUrl);
    serverUrl->setObjectName("preferencesServerUrl");
    serverUrl->setPlaceholderText("https://服务器地址:8765");
    accessKey = new QLineEdit(connection.token);
    accessKey->setObjectName("preferencesAccessKey");
    accessKey->setEchoMode(QLineEdit::Password);
    accessKey->setPlaceholderText("服务端管理员创建的客户端接入密钥");
    showKey = new QToolButton;
    showKey->setObjectName("preferencesShowKey");
    showKey->setText("显示");
    showKey->setCheckable(true);
    showKey->setCursor(Qt::PointingHandCursor);
    connect(showKey, &QToolButton::toggled, this, [this](bool shown)
    {
        accessKey->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
        showKey->setText(shown ? "隐藏" : "显示");
    });
    nickname = new QLineEdit(connection.nickname);
    nickname->setObjectName("preferencesNickname");
    nickname->setMaxLength(64);
    certificatePin = new QLineEdit(connection.certificatePin);
    certificatePin->setObjectName("preferencesCertificatePin");
    certificatePin->setPlaceholderText("自签名证书填写服务端提供的 SHA-256 指纹");

    auto *fields = new QFormLayout;
    fields->setSpacing(12);
    fields->addRow("连接方式", connectionMode);
    fields->addRow("服务器地址", serverUrl);
    auto *keyRow = new QHBoxLayout;
    keyRow->setSpacing(8);
    keyRow->addWidget(accessKey, 1);
    keyRow->addWidget(showKey);
    fields->addRow("客户端接入密钥", keyRow);
    fields->addRow("我的显示名", nickname);
    fields->addRow("证书指纹", certificatePin);
    connectionInfo = new QLabel;
    connectionInfo->setObjectName("preferencesConnectionInfo");
    connectionInfo->setTextFormat(Qt::PlainText);
    connectionInfo->setWordWrap(true);
    fields->addRow(connectionInfo);
    link->addLayout(fields);

    const auto updateMode = [this]
    {
        const bool automatic = connectionMode->currentData() == "p2p";
        connectionInfo->setVisible(automatic);
        connectionInfo->setText(p2p.isEmpty() ? "请粘贴服务端提供的自动连接信息。" :
            "服务器设备：" + p2p.value("server_id").toString() +
            "\n公网入口：" + p2p.value("relay_url").toString());
        serverUrl->setReadOnly(automatic);
        serverUrl->setPlaceholderText(automatic ? "p2p://服务器设备 ID" : "https://服务器地址:8765");
    };
    connect(connectionMode, &QComboBox::currentIndexChanged, this, updateMode);
    updateMode();

    auto *connectionActions = new QHBoxLayout;
    connectionActions->setSpacing(8);
    pasteButton = new QPushButton("粘贴连接信息");
    pasteButton->setObjectName("preferencesPasteConnection");
    connectButton = new QPushButton("连接");
    connectButton->setObjectName("preferencesConnect");
    disconnectButton = new QPushButton("断开连接");
    disconnectButton->setObjectName("preferencesDisconnect");
    connectionActions->addWidget(pasteButton);
    connectionActions->addStretch();
    connectionActions->addWidget(disconnectButton);
    connectionActions->addWidget(connectButton);
    link->addLayout(connectionActions);

    pasteResult = description({});
    pasteResult->setObjectName("preferencesPasteResult");
    link->addWidget(pasteResult);
    link->addWidget(description(
        "地址和密钥由服务端管理员提供。其他电脑上的服务端请使用 HTTPS。"
        "断开连接会停止重连并保留这里填写的信息；连接会立即使用当前这些值。"));
    link->addStretch();
    pages->addWidget(connectionPage);

    auto *storagePage = new QWidget;
    storagePage->setObjectName("preferencesStoragePage");
    auto *storage = pageLayout(storagePage, "存储");
    storage->setSpacing(8);
    storage->addWidget(new QLabel("下载位置"));
    auto *location = new QHBoxLayout;
    location->setSpacing(8);
    directory = new QLineEdit(QDir::toNativeSeparators(initial.downloads));
    directory->setObjectName("preferencesDownloads");
    directory->setReadOnly(true);
    directory->setPlaceholderText("请选择下载目录");
    directory->setAccessibleName("下载位置");
    choose = new QPushButton("更改…");
    choose->setObjectName("preferencesChooseDownloads");
    location->addWidget(directory, 1);
    location->addWidget(choose);
    storage->addLayout(location);
    storage->addWidget(description("接收的文件保存在此位置，并按下载日期建立文件夹。"));
    storage->addSpacing(6);
    storage->addWidget(new QLabel("存储空间"));
    downloadsUsage = new QLabel("已保存文件：未统计");
    downloadsUsage->setObjectName("preferencesUsageDownloads");
    downloadsUsage->setTextFormat(Qt::PlainText);
    cacheUsage = new QLabel("自动预览和语音缓存：未统计");
    cacheUsage->setObjectName("preferencesUsageCache");
    cacheUsage->setTextFormat(Qt::PlainText);
    storage->addWidget(downloadsUsage);
    storage->addWidget(cacheUsage);
    auto *usageActions = new QHBoxLayout;
    refresh = new QPushButton("刷新用量");
    refresh->setObjectName("preferencesRefreshUsage");
    clear = new QPushButton("清理缓存");
    clear->setObjectName("preferencesClearCache");
    usageActions->addWidget(refresh);
    usageActions->addWidget(clear);
    usageActions->addStretch();
    storage->addLayout(usageActions);
    storage->addWidget(description("清理自动预览和语音缓存，不删除已保存的文件。正在显示或播放的缓存会暂时保留。"));
    auto *quota = new QHBoxLayout;
    quota->addWidget(new QLabel("缓存上限"));
    quota->addStretch();
    cacheLimit = new QComboBox;
    cacheLimit->setObjectName("preferencesCacheLimit");
    cacheLimit->setAccessibleName("自动预览和语音缓存上限");
    cacheLimit->setMinimumWidth(120);
    constexpr qint64 mib = 1024LL * 1024;
    cacheLimit->addItem("256 MiB", 256 * mib);
    cacheLimit->addItem("512 MiB", 512 * mib);
    cacheLimit->addItem("1 GiB", 1024 * mib);
    cacheLimit->addItem("2 GiB", 2048 * mib);
    const auto savedLimit = cacheLimit->findData(initial.cacheLimitBytes);
    cacheLimit->setCurrentIndex(savedLimit < 0 ? 1 : savedLimit);
    quota->addWidget(cacheLimit);
    storage->addLayout(quota);
    storage->addWidget(description("上限仅用于自动预览和语音缓存，不影响已保存的文件。"));
    usageStatus = description({});
    usageStatus->setObjectName("preferencesUsageStatus");
    storage->addWidget(usageStatus);
    storage->addStretch();
    pages->addWidget(storagePage);

    auto *appearancePage = new QWidget;
    appearancePage->setObjectName("preferencesAppearancePage");
    auto *appearance = pageLayout(appearancePage, "外观");
    auto *themeRow = new QHBoxLayout;
    themeRow->addWidget(new QLabel("主题"));
    themeRow->addStretch();
    theme = new QComboBox;
    theme->setObjectName("preferencesTheme");
    theme->setAccessibleName("主题外观");
    theme->setMinimumWidth(150);
    theme->addItem("跟随系统", "system");
    theme->addItem("浅色", "light");
    theme->addItem("深色", "dark");
    const auto savedTheme = theme->findData(initial.theme);
    theme->setCurrentIndex(savedTheme < 0 ? 0 : savedTheme);
    themeRow->addWidget(theme);
    appearance->addLayout(themeRow);
    auto *wheelRow = new QHBoxLayout;
    wheelRow->addWidget(new QLabel("滚轮速度"));
    wheelRow->addStretch();
    wheelSpeed = new QComboBox;
    wheelSpeed->setObjectName("preferencesWheelSpeed");
    wheelSpeed->setAccessibleName("鼠标滚轮滚动速度");
    wheelSpeed->setMinimumWidth(150);
    wheelSpeed->addItem("最慢", 0.15);
    wheelSpeed->addItem("较慢", 0.25);
    wheelSpeed->addItem("标准", 0.35);
    wheelSpeed->addItem("较快", 0.5);
    wheelSpeed->addItem("最快（系统原生）", 1.0);
    const auto savedWheel = wheelSpeed->findData(initial.wheelDamping);
    wheelSpeed->setCurrentIndex(savedWheel < 0 ? 2 : savedWheel);
    wheelRow->addWidget(wheelSpeed);
    appearance->addLayout(wheelRow);
    appearance->addWidget(description("保存后立即应用；跟随系统会随系统外观切换。滚轮速度只影响鼠标滚轮，触控板手势保持系统原生手感。"));
    appearance->addStretch();
    pages->addWidget(appearancePage);

    auto *notificationPage = new QWidget;
    notificationPage->setObjectName("preferencesNotificationPage");
    auto *notifications = pageLayout(notificationPage, "通知");
    auto *soundRow = new QHBoxLayout;
    sound = new QCheckBox("新消息通知声音");
    sound->setObjectName("preferencesSound");
    sound->setChecked(initial.notificationSound);
    soundRow->addWidget(sound);
    soundRow->addStretch();
    preview = new QPushButton("试听");
    preview->setObjectName("preferencesSoundPreview");
    soundRow->addWidget(preview);
    notifications->addLayout(soundRow);
    notifications->addWidget(description("仅为允许通知的新消息播放提示音；忽略消息通知的群仍保持静音。"));
    notifications->addStretch();
    pages->addWidget(notificationPage);

    auto *updatePage = new UpdateWidget(ReleaseUpdate::Role::Client);
    updatePage->setObjectName("preferencesUpdatePage");
    pages->addWidget(updatePage);

    auto *footer = new QVBoxLayout;
    footer->setContentsMargins(24, 8, 24, 18);
    validation = description({});
    validation->setObjectName("preferencesValidation");
    validation->hide();
    footer->addWidget(validation);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText("保存");
    buttons->button(QDialogButtonBox::Save)->setObjectName("preferencesSave");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    buttons->button(QDialogButtonBox::Cancel)->setObjectName("preferencesCancel");
    footer->addWidget(buttons);
    right->addLayout(footer);
    connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    navigation->setCurrentRow(0);
    connect(choose, &QPushButton::clicked, this, &PreferencesDialog::chooseDirectory);
    connect(clear, &QPushButton::clicked, this, &PreferencesDialog::clearCache);
    connect(refresh, &QPushButton::clicked, this, [this]
    {
        actionFeedback.clear();
        refreshUsage();
    });
    connect(preview, &QPushButton::clicked, this, &PreferencesDialog::previewSoundRequested);
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(serverUrl, &QLineEdit::textChanged, this, [this] { updateConnectionButtons(); });
    connect(pasteButton, &QPushButton::clicked, this, &PreferencesDialog::pasteConnectionInfo);
    connect(connectButton, &QPushButton::clicked, this, &PreferencesDialog::connectRequested);
    connect(disconnectButton, &QPushButton::clicked, this, &PreferencesDialog::disconnectRequested);
    updateConnectionButtons();
    QTimer::singleShot(0, this, &PreferencesDialog::refreshUsage);
}

PreferencesDialog::Values PreferencesDialog::values() const
{
    return {QDir::fromNativeSeparators(directory->text()), theme->currentData().toString(),
            sound->isChecked(), cacheLimit->currentData().toLongLong(),
            wheelSpeed->currentData().toReal()};
}

PreferencesDialog::Connection PreferencesDialog::connection() const
{
    return {serverUrl->text().trimmed(), accessKey->text().trimmed(), nickname->text().trimmed(),
            certificatePin->text().trimmed(), connectionMode->currentData().toString(), p2p,
            connected, reconnecting, connectionStatus->text()};
}

void PreferencesDialog::setConnectionState(bool isConnected, bool isReconnecting,
                                           const QString &status)
{
    connected = isConnected;
    reconnecting = isReconnecting;
    connectionStatus->setText(status);
    // The window owns the transport, so it reports the same three states the chat
    // banner uses: connected, retrying, and hard-blocked.
    const auto palette = connectionStatus->palette();
    connectionStatus->setStyleSheet(connected
        ? QStringLiteral("color:%1;").arg(palette.color(QPalette::Text).name())
        : QStringLiteral("color:%1;").arg(palette.color(QPalette::PlaceholderText).name()));
    updateConnectionButtons();
}

void PreferencesDialog::selectPage(int row)
{
    if (navigation && row >= 0 && row < navigation->count())
        navigation->setCurrentRow(row);
}

void PreferencesDialog::updateConnectionButtons()
{
    // Only a live session locks the fields. A retrying client still has to let the
    // user correct a wrong address, otherwise the retry loop becomes a dead end.
    const bool live = connected;
    const bool automatic = connectionMode->currentData() == "p2p";
    serverUrl->setReadOnly(live || automatic);
    accessKey->setReadOnly(live);
    certificatePin->setReadOnly(live);
    nickname->setReadOnly(live);
    connectionMode->setEnabled(!live);
    pasteButton->setEnabled(!live);
    connectButton->setText(live ? "重新连接" : "连接");
    connectButton->setEnabled(!serverUrl->text().trimmed().isEmpty());
    // Offered whenever a connection is active or being retried, so the user can stop
    // an endless reconnect instead of only quitting the application.
    disconnectButton->setEnabled(connected || reconnecting);
    disconnectButton->setVisible(connected || reconnecting);
}

void PreferencesDialog::pasteConnectionInfo()
{
    QJsonObject info;
    QString error;
    const auto text = QApplication::clipboard()->text();
    if (!ConnectionInfo::parse(text.toUtf8(), info, error))
    {
        pasteResult->setStyleSheet("color:#b54c3e;");
        pasteResult->setText(error.isEmpty() ? "剪贴板不是有效的连接信息。" : error);
        return;
    }
    p2p = info.value("p2p").toObject();
    connectionMode->setCurrentIndex(info.value("mode") == "p2p" ? 1 : 0);
    serverUrl->setText(info["serverUrl"].toString().trimmed());
    accessKey->setText(info["token"].toString().trimmed());
    certificatePin->setText(info["certificatePin"].toString().trimmed());
    pasteResult->setStyleSheet({});
    pasteResult->setText("已填入，请核对后点“连接”。");
    updateConnectionButtons();
}

void PreferencesDialog::chooseDirectory()
{
    const auto selected = QFileDialog::getExistingDirectory(this, "选择下载目录", values().downloads);
    if (selected.isEmpty() || clearing)
        return;
    directory->setText(QDir::toNativeSeparators(selected));
    validation->hide();
    actionFeedback.clear();
    refreshUsage();
}

void PreferencesDialog::updateUsageState()
{
    choose->setEnabled(!clearing);
    refresh->setEnabled(!clearing && !scanning);
    clear->setEnabled(!clearing);
    clear->setText(clearing ? "正在清理…" : "清理缓存");
}

void PreferencesDialog::refreshUsage()
{
    if (clearing)
        return;
    usageRequest = ++sequence;
    scanning = true;
    downloadsUsage->setText("已保存文件：正在统计…");
    cacheUsage->setText("自动预览和语音缓存：正在统计…");
    usageStatus->setText(actionFeedback.isEmpty() ? "正在统计存储用量…" : actionFeedback + "\n正在统计存储用量…");
    updateUsageState();
    emit usageRequested(values().downloads, usageRequest);
}

void PreferencesDialog::setUsage(quint64 requestId, qint64 downloadsBytes, qint64 cacheBytes, const QString &error)
{
    if (!usageRequest || requestId != usageRequest || clearing)
        return;
    scanning = false;
    downloadsUsage->setText("已保存文件：" + byteSize(downloadsBytes));
    cacheUsage->setText("自动预览和语音缓存：" + byteSize(cacheBytes));
    const auto failure = cleanError(error);
    usageStatus->setText(failure.isEmpty() ? actionFeedback :
        (actionFeedback.isEmpty() ? QString() : actionFeedback + '\n') + "部分存储用量暂不可读：" + failure);
    updateUsageState();
}

void PreferencesDialog::clearCache()
{
    if (clearing)
        return;
    clearing = true;
    scanning = false;
    usageRequest = 0; // An older scan cannot replace the post-cleanup snapshot.
    clearRequest = ++sequence;
    usageStatus->setText("正在清理自动预览和语音缓存…");
    updateUsageState();
    emit clearCacheRequested(values().downloads, clearRequest);
}

void PreferencesDialog::cacheCleared(quint64 requestId, const QString &error)
{
    if (!clearing || requestId != clearRequest)
        return;
    clearing = false;
    const auto failure = cleanError(error);
    actionFeedback = failure.isEmpty() ? "已清理自动预览和语音缓存" : "缓存清理未完成：" + failure;
    refreshUsage();
}

void PreferencesDialog::accept()
{
    const auto path = values().downloads;
    if (path.isEmpty() || path.size() > 4096 || path.contains(QChar::Null) || !QDir::isAbsolutePath(path))
    {
        validation->setText("请选择有效的下载目录。其他设置尚未保存。");
        validation->show();
        return;
    }
    const auto cache = QDir::cleanPath(MediaStorage::cacheDirectory());
    QStringList roots{cache};
    const auto canonicalCache = QDir(cache).canonicalPath();
    if (!canonicalCache.isEmpty() && !roots.contains(canonicalCache))
        roots.append(QDir::cleanPath(canonicalCache));
    QStringList destinations{QDir::cleanPath(path)};
    const auto canonicalDestination = QDir(path).canonicalPath();
    if (!canonicalDestination.isEmpty() && !destinations.contains(canonicalDestination))
        destinations.append(QDir::cleanPath(canonicalDestination));
    for (const auto &destination : destinations)
        for (const auto &root : roots)
            if (destination.compare(root, Qt::CaseInsensitive) == 0 ||
                destination.startsWith(root + '/', Qt::CaseInsensitive))
            {
                validation->setText("下载目录不能设在自动预览和语音缓存目录内，请选择其他位置。其他设置尚未保存。");
                validation->show();
                return;
            }
    emit applied();
    QDialog::accept();
}
