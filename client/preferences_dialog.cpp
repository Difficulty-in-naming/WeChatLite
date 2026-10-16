#include "preferences_dialog.h"
#include "media_storage.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPalette>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
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

PreferencesDialog::PreferencesDialog(const Values &initial, QWidget *parent)
    : QDialog(parent, Qt::Window)
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
    auto *navigation = new QListWidget(this);
    navigation->setObjectName("preferencesNavigation");
    navigation->setFixedWidth(128);
    navigation->setFrameShape(QFrame::NoFrame);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation->setContentsMargins(0, 16, 0, 0);
    navigation->addItems({"存储", "外观", "通知"});
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
    appearance->addWidget(description("保存后立即应用；跟随系统会随系统外观切换。"));
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
    QTimer::singleShot(0, this, &PreferencesDialog::refreshUsage);
}

PreferencesDialog::Values PreferencesDialog::values() const
{
    return {QDir::fromNativeSeparators(directory->text()), theme->currentData().toString(),
            sound->isChecked(), cacheLimit->currentData().toLongLong()};
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
