#include "composer.h"
#include "messages.h"
#include <QAbstractTextDocumentLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputMethodEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextLayout>
#include <QUrl>

static constexpr int MEMBER_ID = QTextFormat::UserProperty + 1;
static constexpr int MEMBER_NAME = QTextFormat::UserProperty + 2;

Composer::Composer(QWidget *parent) : QTextEdit(parent), popup(new QListWidget(this))
{
    setAcceptRichText(false);
    setAcceptDrops(true);
    setObjectName("composer");
    setPlaceholderText("输入消息，@ 提及成员，或拖入 / 粘贴文件");
    setFrameShape(QFrame::NoFrame);
    popup->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    popup->setFocusPolicy(Qt::NoFocus);
    popup->setObjectName("mentionPopup");
    popup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    popup->setFixedWidth(166);
    connect(this, &QTextEdit::textChanged, this, &Composer::updatePopup);
    connect(popup, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { choose(popup->row(item)); });
}
void Composer::setMembers(const QJsonArray &value)
{
    members = value;
    updatePopup();
}
bool Composer::popupVisible() const
{
    return popup->isVisible();
}
bool Composer::canInsertFromMimeData(const QMimeData *source) const
{
    return !isReadOnly() && (source->hasUrls() || source->hasImage() || source->hasFormat("image/gif") ||
                             QTextEdit::canInsertFromMimeData(source));
}
void Composer::insertFromMimeData(const QMimeData *source)
{
    if (isReadOnly())
        return;
    if (source->hasUrls())
    {
        QStringList paths;
        for (const auto &url : source->urls())
            if (url.isLocalFile())
                paths << url.toLocalFile();
        if (!paths.isEmpty())
        {
            emit filesAttached(paths);
            return;
        }
    }
    if (source->hasImage() || source->hasFormat("image/gif"))
    {
        emit imageAttached(source);
        return;
    }
    insertPlainText(source->text());
}
void Composer::dragEnterEvent(QDragEnterEvent *event)
{
    if (canInsertFromMimeData(event->mimeData()))
        event->acceptProposedAction();
}
void Composer::dragMoveEvent(QDragMoveEvent *event)
{
    if (canInsertFromMimeData(event->mimeData()))
        event->acceptProposedAction();
}
void Composer::dropEvent(QDropEvent *event)
{
    if (canInsertFromMimeData(event->mimeData()))
    {
        insertFromMimeData(event->mimeData());
        event->acceptProposedAction();
    }
}
void Composer::inputMethodEvent(QInputMethodEvent *event)
{
    QTextEdit::inputMethodEvent(event);
    updatePopup();
}
bool Composer::isComposing() const
{
    const auto *layout = textCursor().block().layout();
    return layout && !layout->preeditAreaText().isEmpty();
}
void Composer::focusOutEvent(QFocusEvent *event)
{
    if (!popup->underMouse())
        popup->hide();
    QTextEdit::focusOutEvent(event);
}
void Composer::keyPressEvent(QKeyEvent *event)
{
    if (popup->isVisible())
    {
        if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up)
        {
            int delta = event->key() == Qt::Key_Down ? 1 : -1;
            popup->setCurrentRow((popup->currentRow() + delta + popup->count()) % popup->count());
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Tab)
        {
            choose(popup->currentRow());
            return;
        }
        if (event->key() == Qt::Key_Escape)
        {
            popup->hide();
            return;
        }
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        !event->modifiers().testFlag(Qt::ShiftModifier) && !isComposing())
    {
        emit sendRequested();
        return;
    }
    if (event->key() == Qt::Key_Backspace && !textCursor().hasSelection())
    {
        auto cursor = textCursor();
        if (cursor.position() > 0)
        {
            cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
            auto format = cursor.charFormat();
            if (!format.property(MEMBER_ID).toString().isEmpty())
            {
                auto id = format.property(MEMBER_ID).toString();
                int end = cursor.selectionEnd(), start = cursor.selectionStart();
                while (start > 0)
                {
                    QTextCursor check(document());
                    check.setPosition(start - 1);
                    check.setPosition(start, QTextCursor::KeepAnchor);
                    if (check.charFormat().property(MEMBER_ID).toString() != id)
                        break;
                    --start;
                }
                cursor.setPosition(start);
                cursor.setPosition(end, QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
                cursor.setCharFormat(QTextCharFormat());
                setTextCursor(cursor);
                return;
            }
        }
    }
    QTextEdit::keyPressEvent(event);
}
void Composer::updatePopup()
{
    if (choosing)
        return;
    if (isComposing())
    {
        popup->hide();
        return;
    }
    auto cursor = textCursor();
    QString prefix = toPlainText().left(cursor.position());
    auto match = QRegularExpression("(?:^|[\\s])@([^@\\s]{0,64})$").match(prefix);
    if (!match.hasMatch() || !hasFocus())
    {
        popup->hide();
        return;
    }
    if (!cursor.charFormat().property(MEMBER_ID).toString().isEmpty())
    {
        popup->hide();
        return;
    }
    mentionStart = prefix.lastIndexOf('@');
    auto query = match.captured(1);
    popup->clear();
    for (const auto &entry : members)
    {
        auto member = entry.toObject();
        auto name = member["name"].toString();
        auto remark = member["remark"].toString();
        if (query.isEmpty() || name.contains(query, Qt::CaseInsensitive) ||
            remark.contains(query, Qt::CaseInsensitive))
        {
            auto *item = new QListWidgetItem(
                QIcon(chatAvatar(member, 24)),
                name + (remark.isEmpty() || remark == name ? "" : "  ·  " + remark), popup);
            item->setData(Qt::UserRole, member);
            item->setSizeHint(QSize(158, 30));
            if (popup->count() >= 50)
                break;
        }
    }
    if (popup->count() == 0)
    {
        popup->hide();
        return;
    }
    popup->setIconSize(QSize(24, 24));
    popup->setCurrentRow(0);
    popup->setFixedHeight(qMin(6, popup->count()) * 30 + 8);
    auto position = mapToGlobal(cursorRect().topLeft());
    popup->move(position.x(), position.y() - popup->height() - 8);
    popup->show();
}
void Composer::choose(int row)
{
    if (row < 0 || row >= popup->count() || mentionStart < 0)
        return;
    auto member = popup->item(row)->data(Qt::UserRole).toJsonObject();
    auto cursor = textCursor();
    int end = cursor.position();
    cursor.setPosition(mentionStart);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
    insertMention(member);
}
void Composer::insertMention(const QJsonObject &member)
{
    if (isReadOnly() || member["id"].toString().isEmpty() || member["name"].toString().isEmpty())
        return;
    choosing = true;
    auto cursor = textCursor();
    cursor.beginEditBlock();
    const auto before = toPlainText().left(cursor.selectionStart());
    if (!before.isEmpty() && !before.back().isSpace())
        cursor.insertText(QString(QChar(0x2005)), QTextCharFormat());
    QTextCharFormat format;
    format.setForeground(QColor("#16805a"));
    format.setProperty(MEMBER_ID, member["id"].toString());
    format.setProperty(MEMBER_NAME, member["name"].toString());
    cursor.insertText("@" + member["name"].toString(), format);
    cursor.insertText(QString(QChar(0x2005)), QTextCharFormat());
    cursor.endEditBlock();
    setTextCursor(cursor);
    popup->hide();
    choosing = false;
    setFocus();
}
QJsonArray Composer::mentions() const
{
    QJsonArray result;
    for (auto block = document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
        {
            auto fragment = it.fragment();
            auto format = fragment.charFormat();
            auto id = format.property(MEMBER_ID).toString(), name = format.property(MEMBER_NAME).toString();
            if (!id.isEmpty() && fragment.text() == "@" + name)
            {
                result.append(QJsonObject{{"id", id},
                                          {"name", name},
                                          {"_start", fragment.position()},
                                          {"_length", fragment.length()}});
            }
        }
    return result;
}
void Composer::restore(const QString &text, const QJsonArray &tokens)
{
    choosing = true;
    setPlainText(text);
    for (const auto &entry : tokens)
    {
        auto token = entry.toObject();
        auto name = token["name"].toString();
        int start = token["_start"].toInt(-1), length = token["_length"].toInt();
        if (start >= 0 && length == name.size() + 1 && text.mid(start, length) == "@" + name)
        {
            QTextCursor cursor(document());
            cursor.setPosition(start);
            cursor.setPosition(start + length, QTextCursor::KeepAnchor);
            QTextCharFormat format;
            format.setForeground(QColor("#16805a"));
            format.setProperty(MEMBER_ID, token["id"].toString());
            format.setProperty(MEMBER_NAME, name);
            cursor.setCharFormat(format);
        }
    }
    choosing = false;
}
