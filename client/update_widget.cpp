#include "update_widget.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
ReleaseUpdate *sharedUpdater(ReleaseUpdate::Role role, bool create)
{
    static QPointer<ReleaseUpdate> client, server;
    auto &service = role == ReleaseUpdate::Role::Client ? client : server;
    if (!service && create)
        service = new ReleaseUpdate(role, QCoreApplication::applicationVersion(), QCoreApplication::instance());
    return service;
}
}

UpdateWidget::UpdateWidget(ReleaseUpdate::Role applicationRole, QWidget *parent)
    : QWidget(parent), role(applicationRole)
{
    setObjectName("softwareUpdate");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 8);
    layout->setSpacing(14);
    auto *heading = new QLabel("软件更新");
    auto font = heading->font();
    font.setPixelSize(18);
    heading->setFont(font);
    layout->addWidget(heading);
    layout->addWidget(new QLabel("当前版本：" + QCoreApplication::applicationVersion()));
    auto *description = new QLabel("点击检查更新后，有新版本会自动从 GitHub 下载适合这台电脑的安装包。");
    description->setWordWrap(true);
    layout->addWidget(description);
    status = new QLabel("尚未检查更新");
    status->setObjectName("updateStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    progress = new QProgressBar;
    progress->setObjectName("updateProgress");
    progress->hide();
    layout->addWidget(progress);
    auto *actions = new QHBoxLayout;
    checkButton = new QPushButton("检查更新");
    checkButton->setObjectName("updateCheck");
    cancelButton = new QPushButton("取消下载");
    cancelButton->setObjectName("updateCancel");
    cancelButton->hide();
    folderButton = new QPushButton("打开下载目录");
    folderButton->setObjectName("updateOpenFolder");
    folderButton->hide();
    actions->addWidget(checkButton);
    actions->addWidget(cancelButton);
    actions->addWidget(folderButton);
    actions->addStretch();
    layout->addLayout(actions);
    auto *release = new QPushButton("查看发布说明");
    release->setObjectName("updateReleaseNotes");
    layout->addWidget(release, 0, Qt::AlignLeft);
    auto *install = new QLabel("下载完成后，退出对应应用，再打开新版本。聊天数据和连接设置保存在原用户目录。");
    install->setWordWrap(true);
    layout->addWidget(install);
    layout->addStretch();
    connect(checkButton, &QPushButton::clicked, this, &UpdateWidget::check);
    connect(cancelButton, &QPushButton::clicked, this, [this] { if (updater) updater->cancel(); });
    connect(folderButton, &QPushButton::clicked, this, [this]
    {
        if (updater && !updater->filePath().isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(updater->filePath()).absolutePath()));
    });
    connect(release, &QPushButton::clicked, this, []
        { QDesktopServices::openUrl(QUrl("https://github.com/Difficulty-in-naming/WeChatLite/releases/latest")); });
    bind(sharedUpdater(role, false));
}

void UpdateWidget::check()
{
    bind(sharedUpdater(role, true));
    updater->check();
}

void UpdateWidget::bind(ReleaseUpdate *service)
{
    if (!service || updater) return;
    updater = service;
    connect(updater, &ReleaseUpdate::stateChanged, this, &UpdateWidget::refresh);
    connect(updater, &ReleaseUpdate::downloadProgress, this, [this](qint64 received, qint64 total)
    {
        progress->setRange(0, total > 0 ? 1000 : 0);
        if (total > 0) progress->setValue(int(qMin(qint64(1000), received * 1000 / total)));
        status->setText("正在下载 " + updater->version() + "：" +
            QLocale().formattedDataSize(received) + (total > 0 ? " / " + QLocale().formattedDataSize(total) : ""));
    });
    refresh();
}

void UpdateWidget::refresh()
{
    using State = ReleaseUpdate::State;
    const auto current = updater->state();
    const bool busy = current == State::Checking || current == State::Downloading;
    checkButton->setEnabled(!busy);
    cancelButton->setVisible(busy);
    cancelButton->setText(current == State::Checking ? "取消" : "取消下载");
    progress->setVisible(busy);
    folderButton->setVisible(current == State::Ready);
    if (busy) progress->setRange(0, 0);
    switch (current)
    {
    case State::Checking: status->setText("正在检查 GitHub 上的新版本…"); break;
    case State::Downloading: status->setText("发现新版本 " + updater->version() + "，正在下载…"); break;
    case State::UpToDate: status->setText("当前已是最新版本。"); break;
    case State::Ready: status->setText("已下载 " + updater->version() + "\n" + updater->filePath()); break;
    case State::Failed: status->setText("更新未完成：" + updater->errorString() + "\n可以点击“检查更新”重试。"); break;
    case State::Cancelled: status->setText("已取消，可重新检查更新。"); break;
    case State::Idle: status->setText("尚未检查更新"); break;
    }
}
