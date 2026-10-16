#include "announcement_dialog.h"
#include "messages.h"
#include "transport.h"
#include <QCloseEvent>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
void showPlainAnnouncement(QTextBrowser *view, const QString &text)
{
    view->setPlainText(text);
    // Only literal HTTP(S) links receive anchors. The announcement is never parsed
    // as HTML, so markup cannot load images, stylesheets or other remote resources.
    static const QRegularExpression links(R"(https?://[^\s<>"']+)");
    auto matches = links.globalMatch(text);
    while (matches.hasNext())
    {
        const auto match = matches.next();
        auto target = match.captured();
        while (!target.isEmpty() && QString("。，、；！!）)]}").contains(target.back()))
            target.chop(1);
        const QUrl url(target, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
            (url.scheme() != "https" && url.scheme() != "http"))
            continue;
        QTextCursor cursor(view->document());
        cursor.setPosition(int(match.capturedStart()));
        cursor.setPosition(int(match.capturedStart() + target.size()), QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(url.toString(QUrl::FullyEncoded));
        format.setForeground(qApp->palette().color(QPalette::Link));
        format.setFontUnderline(true);
        cursor.mergeCharFormat(format);
    }
    auto cursor = view->textCursor();
    cursor.setPosition(0);
    view->setTextCursor(cursor);
}
}

AnnouncementDialog::AnnouncementDialog(Transport *service, const QString &group, const QString &name,
                                       QWidget *parent)
    : QDialog(parent, Qt::Window), transport(service), groupId(group)
{
    setObjectName("announcementDialog");
    setWindowTitle("“" + name + "”的群公告");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(552, 642);
    setMinimumSize(400, 360);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 22, 24, 20);
    outer->setSpacing(0);
    auto *heading = new QHBoxLayout;
    heading->setSpacing(9);
    avatar = new QLabel;
    avatar->setObjectName("announcementAvatar");
    avatar->setFixedSize(36, 36);
    heading->addWidget(avatar);
    auto *identity = new QVBoxLayout;
    identity->setSpacing(2);
    author = new QLabel;
    author->setObjectName("announcementAuthor");
    author->setTextFormat(Qt::PlainText);
    updated = new QLabel;
    updated->setObjectName("announcementUpdated");
    updated->setTextFormat(Qt::PlainText);
    identity->addWidget(author);
    identity->addWidget(updated);
    heading->addLayout(identity, 1);
    outer->addLayout(heading);
    outer->addSpacing(36);
    state = new QLabel("正在读取群公告…");
    state->setObjectName("announcementState");
    state->setTextFormat(Qt::PlainText);
    state->setWordWrap(true);
    outer->addWidget(state);
    body = new QTextBrowser;
    body->setObjectName("announcementBody");
    body->setReadOnly(true);
    body->setOpenLinks(false);
    body->setOpenExternalLinks(false);
    body->viewport()->installEventFilter(this);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard |
                                  Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    body->setFrameShape(QFrame::NoFrame);
    body->document()->setDocumentMargin(0);
    outer->addWidget(body, 1);
    retry = new QPushButton("重新读取");
    retry->setObjectName("announcementRetry");
    retry->setCursor(Qt::PointingHandCursor);
    outer->addWidget(retry, 0, Qt::AlignHCenter);
    outer->addSpacing(8);
    auto *readOnly = new QLabel("只读群公告");
    readOnly->setObjectName("announcementReadOnly");
    outer->addWidget(readOnly, 0, Qt::AlignHCenter);
    connect(retry, &QPushButton::clicked, this, &AnnouncementDialog::reload);
    connect(body, &QTextBrowser::anchorClicked, this, [](const QUrl &url)
    {
        if (url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
            (url.scheme() == "https" || url.scheme() == "http"))
            QDesktopServices::openUrl(url);
    });
    QTimer::singleShot(0, this, &AnnouncementDialog::reload);
}

void AnnouncementDialog::reload()
{
    if (!valid || !transport)
        return;
    const auto requestedGeneration = ++generation;
    retry->setEnabled(false);
    author->clear();
    updated->clear();
    avatar->clear();
    body->clear();
    state->setText("正在读取群公告…");
    state->show();
    QPointer<AnnouncementDialog> self(this);
    transport->get("/api/groups/" + QString::fromLatin1(QUrl::toPercentEncoding(groupId)) + "/announcement",
        [self, requestedGeneration](const QJsonValue &value, const QString &error)
        {
            if (!self || !self->valid || self->generation != requestedGeneration)
                return;
            self->retry->setEnabled(true);
            const auto announcement = value.toObject();
            const auto text = announcement["text"].toString();
            if (!error.isEmpty() || announcement["group"] != self->groupId ||
                !announcement["available"].toBool() || text.size() > 256 * 1024)
            {
                self->setProperty("announcementState", "unavailable");
                self->state->setText("群公告暂不可读，请稍后重试");
                return;
            }
            if (!announcement["set"].toBool())
            {
                self->setProperty("announcementState", "unset");
                self->state->setText("此群尚未设置群公告");
                return;
            }
            self->setProperty("announcementState", "set");
            const QJsonObject identity{{"id", announcement["author_id"]}, {"name", announcement["author"]},
                                       {"avatar", announcement["avatar"]}};
            const auto author = announcement["author"].toString();
            self->author->setText(author.isEmpty() ? "发布者暂不可读" : author.left(160));
            if (!author.isEmpty() || !announcement["avatar"].toString().isEmpty())
                self->avatar->setPixmap(chatAvatar(identity, 36));
            const auto stamp = announcement["updated_at"].toInteger();
            self->updated->setText(stamp > 0 ? QDateTime::fromMSecsSinceEpoch(stamp).toString("yyyy.MM.dd HH:mm") :
                                              "更新时间暂不可读");
            self->state->hide();
            showPlainAnnouncement(self->body, text);
        }, 20000);
}

void AnnouncementDialog::invalidate()
{
    valid = false;
    ++generation;
    close();
}

bool AnnouncementDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == body->viewport() && event->type() == QEvent::ToolTip)
        return true;
    return QDialog::eventFilter(watched, event);
}

void AnnouncementDialog::closeEvent(QCloseEvent *event)
{
    valid = false;
    ++generation;
    QDialog::closeEvent(event);
}
