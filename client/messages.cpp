#include "messages.h"
#include "attachment.h"
#include "smooth_scroll.h"
#include "theme.h"
#include "wechat_emoji.h"
#include <QApplication>
#include <QBuffer>
#include <QCache>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHideEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QScreen>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextLayout>
#include <QTextOption>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

static QColor themeColor(const char *light, const char *dark)
{
    return QColor(Theme::isDark() ? dark : light);
}

bool canRecallMessage(const QJsonObject &message, qint64 now)
{
    const auto status = message["status"].toString();
    const auto recall = message["recall_status"].toString();
    const auto age = now - message["timestamp"].toInteger();
    return message["outgoing"].toBool() && (status == "sent" || status == "received") &&
           !message["source_id"].toString().isEmpty() && !message["id"].toString().isEmpty() &&
           recall.isEmpty() && age >= 0 && age < 180000;
}
QString messageQuoteLabel(const QJsonObject &quote)
{
    if (quote["unavailable"].toBool())
        return "原消息已撤回或不可用";
    return quote["sender"].toString() + ": " + wechatDisplayText(quote["text"].toString().replace('\n', ' '));
}
static QJsonObject redactedMessage(QJsonObject message)
{
    if (message["status"] == "recalled")
    {
        message["text"] = "";
        message["media"] = QJsonArray{};
        message.remove("quote");
        message.remove("mentions");
    }
    return message;
}
QVariant MessageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rows.size())
        return {};
    if (role == Qt::UserRole)
        return rows[index.row()];
    if (role == Qt::AccessibleTextRole)
    {
        if (rows[index.row()]["status"] == "recalled")
            return "消息已撤回";
        return rows[index.row()]["sender"].toString() + "：" + rows[index.row()]["text"].toString();
    }
    return {};
}
static bool messageBefore(const QJsonObject &a, const QJsonObject &b)
{
    const auto first = a["timestamp"].toInteger(), second = b["timestamp"].toInteger();
    return first != second ? first < second : a["seq"].toInteger() < b["seq"].toInteger();
}
bool sameMediaDescriptor(const QJsonObject &first, const QJsonObject &second)
{
    return !first["id"].toString().isEmpty() && first["id"] == second["id"] &&
           first["sha256"] == second["sha256"] && first["size"] == second["size"] && first["kind"] == second["kind"];
}
QJsonObject preserveLocalMediaState(QJsonObject message, const QJsonObject &previous)
{
    if (message["group"] != previous["group"] || message["id"] != previous["id"] ||
        message["status"] == "recalled")
        return message;
    auto media = message["media"].toArray();
    for (int i = 0; i < media.size(); ++i)
    {
        auto item = media[i].toObject();
        for (const auto &entry : previous["media"].toArray())
        {
            const auto old = entry.toObject();
            if (sameMediaDescriptor(item, old))
            {
                // Treat each transient path independently. An explicit empty field
                // invalidates it, while a receipt without the field retains it.
                for (const auto &field : {"_local", "_poster"})
                {
                    const auto path = old[field].toString();
                    if (!item.contains(field) && !path.isEmpty() && QFileInfo(path).isFile())
                        item[field] = path;
                }
                break;
            }
        }
        media[i] = item;
    }
    message["media"] = media;
    return message;
}
QJsonObject MessageModel::normalize(QJsonObject message)
{
    const auto key = message["group"].toString() + '\n' + message["id"].toString();
    if (message["status"] == "recalled" && !message["id"].toString().isEmpty() && !recalledIds.contains(key))
    {
        recalledIds.insert(key);
        recalledOrder.append(key);
        if (recalledOrder.size() > 512)
            recalledIds.remove(recalledOrder.takeFirst());
    }
    if (recalledIds.contains(key))
        message["status"] = "recalled";
    auto quote = message["quote"].toObject();
    if (recalledIds.contains(message["group"].toString() + '\n' + quote["id"].toString()))
    {
        quote["unavailable"] = true;
        quote["text"] = "";
        message["quote"] = quote;
    }
    return redactedMessage(message);
}
void MessageModel::replace(const QJsonArray &messages)
{
    QHash<QString, QJsonObject> previous;
    for (const auto &row : rows)
        previous.insert(row["group"].toString() + '\n' + row["id"].toString(), row);
    beginResetModel();
    rows.clear();
    for (const auto &message : messages)
        if (message.toObject()["status"] == "recalled")
            normalize(message.toObject());
    for (const auto &message : messages)
    {
        auto row = message.toObject();
        row = preserveLocalMediaState(row, previous.value(row["group"].toString() + '\n' + row["id"].toString()));
        rows.append(normalize(row));
    }
    std::stable_sort(rows.begin(), rows.end(), messageBefore);
    while (rows.size() > 300)
        rows.removeFirst();
    endResetModel();
}
void MessageModel::upsert(const QJsonObject &incoming)
{
    const auto message = normalize(incoming);
    const auto id = message["id"].toString();
    if (message["status"] == "recalled")
        markQuoteUnavailable(id);
    for (int row = 0; row < rows.size(); ++row)
        if (rows[row]["id"] == id)
        {
            auto updated = message;
            if (updated["status"] == "recalled")
                updated["_can_reedit"] = rows[row]["_can_reedit"].toBool();
            updated = preserveLocalMediaState(updated, rows[row]);
            const bool moves = (row > 0 && messageBefore(updated, rows[row - 1])) ||
                               (row + 1 < rows.size() && messageBefore(rows[row + 1], updated));
            if (moves)
                beginResetModel();
            rows[row] = updated;
            if (moves)
            {
                std::stable_sort(rows.begin(), rows.end(), messageBefore);
                endResetModel();
            }
            else
                emit dataChanged(index(row), index(row));
            return;
        }
    if (rows.size() >= 300 && messageBefore(message, rows.first()))
        return;
    int row = 0;
    while (row < rows.size() && !messageBefore(message, rows[row]))
        ++row;
    beginInsertRows({}, row, row);
    rows.insert(row, message);
    endInsertRows();
    if (rows.size() > 300)
    {
        beginRemoveRows({}, 0, 0);
        rows.removeFirst();
        endRemoveRows();
    }
}
void MessageModel::prepend(const QJsonArray &messages)
{
    beginResetModel();
    QSet<QString> seen;
    for (const auto &message : rows)
        seen.insert(message["id"].toString());
    for (const auto &entry : messages)
    {
        auto message = normalize(entry.toObject());
        if (!seen.contains(message["id"].toString()))
        {
            rows.append(message);
            seen.insert(message["id"].toString());
        }
    }
    std::stable_sort(rows.begin(), rows.end(), messageBefore);
    while (rows.size() > 300)
        rows.removeLast();
    endResetModel();
}
void MessageModel::markQuoteUnavailable(const QString &id)
{
    if (id.isEmpty())
        return;
    for (int row = 0; row < rows.size(); ++row)
    {
        auto quote = rows[row]["quote"].toObject();
        if (quote["id"] == id && !quote["unavailable"].toBool())
        {
            quote["unavailable"] = true;
            quote["text"] = "";
            rows[row]["quote"] = quote;
            emit dataChanged(index(row), index(row));
        }
    }
}
void MessageModel::setCanReedit(const QString &id, bool value)
{
    for (int row = 0; row < rows.size(); ++row)
        if (rows[row]["id"] == id && rows[row]["_can_reedit"].toBool() != value)
        {
            rows[row]["_can_reedit"] = value;
            emit dataChanged(index(row), index(row));
            return;
        }
}
void MessageModel::setLocalMedia(const QString &id, const QString &path, const QJsonObject &descriptor)
{
    setLocalMediaPath(id, path, descriptor, "_local");
}
void MessageModel::setLocalPoster(const QString &id, const QString &path, const QJsonObject &descriptor)
{
    setLocalMediaPath(id, path, descriptor, "_poster");
}
void MessageModel::setLocalMediaPath(const QString &id, const QString &path, const QJsonObject &descriptor,
                                     const QString &field)
{
    for (int row = 0; row < rows.size(); ++row)
    {
        auto media = rows[row]["media"].toArray();
        bool changed = false;
        for (int i = 0; i < media.size(); ++i)
            if (media[i].toObject()["id"] == id && media[i].toObject()[field].toString() != path &&
                (descriptor.isEmpty() || sameMediaDescriptor(media[i].toObject(), descriptor)))
            {
                auto object = media[i].toObject();
                object[field] = path;
                media[i] = object;
                changed = true;
            }
        if (changed)
        {
            rows[row]["media"] = media;
            emit dataChanged(index(row), index(row));
        }
    }
}

static QFont smallFont(const QFont &font, int pixels = 12)
{
    auto small = font;
    small.setPixelSize(pixels);
    return small;
}

static qreal layoutText(QTextLayout &layout, int width)
{
    auto textOption = layout.textOption();
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(textOption);
    layout.beginLayout();
    qreal height = 0;
    while (true)
    {
        auto line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(qMax(1, width));
        line.setPosition({0, height});
        height += qCeil(line.height());
    }
    layout.endLayout();
    return height;
}
static QString layoutString(const QJsonObject &message)
{
    return wechatDisplayText(message["text"].toString()).replace('\n', QChar::LineSeparator);
}
static QList<int> displaySourceOffsets(const QString &source)
{
    // Display-only bracket aliases may have a different UTF-16 length. Keep
    // clipboard text in the original representation, including whole aliases.
    QList<int> offsets{0};
    offsets.reserve(source.size() + 1);
    auto literal = [&](int first, int last)
    {
        for (int i = first; i < last; ++i)
            offsets.append(i + 1);
    };
    int position = 0;
    while (position < source.size())
    {
        const int opening = source.indexOf('[', position);
        if (opening < 0)
        {
            literal(position, int(source.size()));
            break;
        }
        literal(position, opening);
        const int closing = source.indexOf(']', opening + 1);
        if (closing < 0)
        {
            literal(opening, int(source.size()));
            break;
        }
        const auto token = source.mid(opening, closing - opening + 1);
        const auto display = wechatDisplayText(token);
        if (display == token)
            literal(opening, closing + 1);
        else
            for (int i = 0; i < display.size(); ++i)
                offsets.append(i + 1 == display.size() ? closing + 1 : opening);
        position = closing + 1;
    }
    return offsets;
}
int MessageDelegate::textHeight(const QJsonObject &message, int width, const QFont &font)
{
    if (message["text"].toString().isEmpty())
        return 0;
    QTextLayout layout(layoutString(message), wechatDisplayFont(font));
    return qCeil(layoutText(layout, width - 20)) + 20;
}
static QSize previewSize(const QJsonObject &media)
{
    auto path = media["_local"].toString();
    auto kind = media["kind"].toString();
    if (kind == "image" || kind == "gif")
    {
        if (!path.isEmpty())
        {
            // Downloaded media paths are immutable; avoid reopening their headers on every GIF frame.
            static QCache<QString, QSize> sizes(256);
            const auto key = path + ':' + media["id"].toString() + ':' + media["sha256"].toString();
            auto *cached = sizes.object(key);
            QSize size;
            if (cached)
                size = *cached;
            else
            {
                QImageReader reader(path);
                size = reader.size();
                if (size.isValid())
                    sizes.insert(key, new QSize(size));
            }
            if (size.isValid())
            {
                const int limit = kind == "gif" ? 134 : 240;
                return size.scaled(qMin(limit, size.width()), qMin(limit, size.height()),
                                   Qt::KeepAspectRatio);
            }
        }
        return {134, 134};
    }
    if (kind == "video")
        return {240, 144};
    return kind == "voice" ? QSize(166, 40) : QSize(260, 82);
}
static int bubbleWidth(const QJsonObject &message, int viewWidth, const QFont &font)
{
    const int maximum = qMax(100, qMin(540, viewWidth - 150));
    int desired = 36;
    const QFontMetrics metrics(wechatDisplayFont(font));
    for (const auto &line : wechatDisplayText(message["text"].toString()).split('\n'))
        desired = qMax(desired, metrics.horizontalAdvance(line) + 20);
    for (const auto &entry : message["media"].toArray())
        desired = qMax(desired, previewSize(entry.toObject()).width());
    return qMin(maximum, desired);
}
static bool showTime(const QModelIndex &index)
{
    if (index.row() == 0)
        return true;
    auto message = index.data(Qt::UserRole).toJsonObject();
    auto previous = index.siblingAtRow(index.row() - 1).data(Qt::UserRole).toJsonObject();
    return message["timestamp"].toInteger() - previous["timestamp"].toInteger() >= 5 * 60 * 1000;
}
static QString timeLabel(qint64 timestamp)
{
    auto time = QDateTime::fromMSecsSinceEpoch(timestamp);
    return time.date() == QDate::currentDate() ? time.toString("HH:mm") : time.toString("M月d日 HH:mm");
}
static QString statusLabel(const QJsonObject &message)
{
    if (!message["outgoing"].toBool())
        return {};
    auto status = message["status"].toString();
    const auto recall = message["recall_status"].toString();
    if (recall == "pending")
        return "等待撤回确认";
    if (recall == "uncertain")
        return "撤回结果待核对";
    if (recall == "failed")
        return "撤回失败";
    return status == "queued"      ? "等待发送"
           : status == "sending"   ? "等待微信确认"
           : status == "uncertain" ? "结果待核对"
           : status == "failed"    ? "发送失败"
                                   : "";
}
struct MessageGeometry
{
    QRect bubble, quote;
    int timeHeight = 0, textHeight = 0, rowHeight = 0;
    QList<QRect> media;
};
static MessageGeometry geometryFor(const QJsonObject &message, const QModelIndex &index, const QRect &row,
                                   const QFont &font)
{
    MessageGeometry geometry;
    const bool mine = message["outgoing"].toBool();
    const int width = bubbleWidth(message, row.width(), font);
    geometry.timeHeight = showTime(index) ? 30 : 0;
    if (message["status"] == "recalled")
    {
        geometry.rowHeight = geometry.timeHeight + 38;
        return geometry;
    }
    geometry.textHeight = MessageDelegate::textHeight(message, width, font);
    int height = geometry.textHeight;
    const auto media = message["media"].toArray();
    for (int i = 0; i < media.size(); ++i)
    {
        if (height)
            height += 8;
        auto item = media[i].toObject();
        const auto size = previewSize(item);
        const int mediaWidth = qMin(width, size.width());
        const int mediaHeight = size.height() * mediaWidth / size.width();
        geometry.media.append(QRect(mine ? width - mediaWidth : 0, height, mediaWidth, mediaHeight));
        height += mediaHeight;
    }
    const int x = mine ? row.right() - 73 - width : row.left() + 70;
    const int y = row.top() + geometry.timeHeight + (mine ? 8 : 23);
    geometry.bubble = QRect(x, y, width, qMax(36, height));
    const auto quote = message["quote"].toObject();
    if (!quote["id"].toString().isEmpty())
    {
        const int quoteWidth =
            qMin(qMax(100, qMin(540, row.width() - 150)),
                 QFontMetrics(wechatDisplayFont(smallFont(font)))
                         .horizontalAdvance(messageQuoteLabel(quote)) + 20);
        geometry.quote = QRect(mine ? geometry.bubble.right() - quoteWidth + 1 : x,
                               geometry.bubble.bottom() + 8, quoteWidth, 18);
    }
    geometry.rowHeight = (geometry.quote.isEmpty() ? geometry.bubble.bottom() : geometry.quote.bottom()) -
                         row.top() + 19 + (statusLabel(message).isEmpty() ? 0 : 17);
    return geometry;
}
static QRect textContentRect(const MessageGeometry &geometry)
{
    return QRect(geometry.bubble.topLeft() + QPoint(10, 10),
                 QSize(geometry.bubble.width() - 20, geometry.textHeight - 20));
}
QRect MessageDelegate::bubbleRect(const QStyleOptionViewItem &option, const QModelIndex &index)
{
    return geometryFor(index.data(Qt::UserRole).toJsonObject(), index, option.rect, option.font).bubble;
}
QRect MessageDelegate::quoteRect(const QStyleOptionViewItem &option, const QModelIndex &index)
{
    return geometryFor(index.data(Qt::UserRole).toJsonObject(), index, option.rect, option.font).quote;
}
QRect MessageDelegate::reeditRect(const QStyleOptionViewItem &option, const QModelIndex &index)
{
    const auto message = index.data(Qt::UserRole).toJsonObject();
    if (message["status"] != "recalled" || !message["outgoing"].toBool() || !message["_can_reedit"].toBool())
        return {};
    const QFontMetrics metrics(smallFont(option.font));
    const int noticeWidth = metrics.horizontalAdvance("你撤回了一条消息"),
              linkWidth = metrics.horizontalAdvance("重新编辑");
    return QRect(option.rect.center().x() - (noticeWidth + 6 + linkWidth) / 2 + noticeWidth + 6,
                 option.rect.top() + (showTime(index) ? 30 : 0), linkWidth, 22);
}
QSize MessageDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    auto message = index.data(Qt::UserRole).toJsonObject();
    const int width = option.rect.width() > 0 ? option.rect.width() : 700;
    return {width, geometryFor(message, index, QRect(0, 0, width, 0), option.font).rowHeight};
}
void MessageDelegate::setTextSelection(const QModelIndex &index, int start, int length)
{
    selectedTextIndex = index;
    selectedTextStart = start;
    selectedTextLength = length;
}
static QPixmap mediaPixmap(const QJsonObject &media, const QString &gifCachePrefix)
{
    auto path = media["_local"].toString();
    if (path.isEmpty())
        return {};
    QPixmap pixmap;
    if (media["kind"].toString() == "gif" && !gifCachePrefix.isEmpty() &&
        QPixmapCache::find(gifCachePrefix + path + ':' + media["sha256"].toString(), &pixmap))
        return pixmap;
    const auto cacheKey = "chat:" + path + ':' + media["id"].toString() + ':' + media["sha256"].toString();
    if (!QPixmapCache::find(cacheKey, &pixmap))
    {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const auto size = reader.size();
        if (size.isValid())
            reader.setScaledSize(size.scaled(
                media["kind"].toString() == "gif" ? QSize(134, 134) : QSize(480, 480), Qt::KeepAspectRatio));
        auto image = reader.read();
        if (!image.isNull())
        {
            pixmap = QPixmap::fromImage(image);
            QPixmapCache::insert(cacheKey, pixmap);
        }
    }
    return pixmap;
}
static QPixmap videoPosterPixmap(const QJsonObject &media)
{
    constexpr qint64 maximumBytes = 48 * 1024;
    const auto path = media["_poster"].toString();
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile() || info.size() <= 0 || info.size() > maximumBytes)
        return {};
    const auto identity = info.absoluteFilePath() + '\n' + media["id"].toString() + '\n' +
        media["sha256"].toString() + '\n' + media["kind"].toString() + '\n' +
        QString::number(media["size"].toInteger(-1)) + '\n' + QString::number(info.size()) + '\n' +
        QString::number(info.lastModified().toMSecsSinceEpoch());
    const auto key = "video-poster:" + QString::fromLatin1(
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
    QPixmap result;
    if (QPixmapCache::find(key, &result))
        return result;
    static QCache<QString, bool> invalid(128);
    if (invalid.contains(key))
        return {};

    QFile file(path);
    if (file.open(QIODevice::ReadOnly))
    {
        // The file can change after QFileInfo. Bound the actual read as well as
        // the decoded dimensions before asking the JPEG plugin for pixels.
        auto bytes = file.read(maximumBytes + 1);
        if (bytes.size() <= maximumBytes && bytes.startsWith("\xff\xd8") && bytes.endsWith("\xff\xd9"))
        {
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "jpeg");
            reader.setAutoDetectImageFormat(false);
            const auto size = reader.size();
            if (size.isValid() && size.width() <= 480 && size.height() <= 288)
            {
                const auto image = reader.read();
                if (!image.isNull() && image.size() == size && reader.error() == QImageReader::UnknownError)
                {
                    // Crop by source coordinates rather than creating a large
                    // intermediate pixmap for an unusually narrow image.
                    QRectF source(image.rect());
                    if (source.width() * 144 > source.height() * 240)
                    {
                        const auto width = source.height() * 240 / 144;
                        source.setLeft((source.width() - width) / 2);
                        source.setWidth(width);
                    }
                    else
                    {
                        const auto height = source.width() * 144 / 240;
                        source.setTop((source.height() - height) / 2);
                        source.setHeight(height);
                    }
                    result = QPixmap(240, 144);
                    result.fill(Qt::transparent);
                    QPainter painter(&result);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform);
                    painter.drawImage(QRectF(result.rect()), image, source);
                    painter.end();
                    QPixmapCache::insert(key, result);
                }
            }
        }
    }
    if (result.isNull())
        invalid.insert(key, new bool(true));
    return result;
}
void paintVideoPreview(QPainter *painter, const QRect &rect, const QJsonObject &media, const QFont &font)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor("#525559"));
    painter->drawRoundedRect(rect, 4, 4);
    const auto poster = videoPosterPixmap(media);
    if (!poster.isNull())
    {
        QPainterPath clip;
        clip.addRoundedRect(rect, 4, 4);
        painter->setClipPath(clip, Qt::IntersectClip);
        painter->drawPixmap(rect, poster, poster.rect());
        painter->setBrush(QColor(0, 0, 0, 80));
        painter->drawEllipse(rect.center(), 16, 16);
        painter->drawRect(rect.adjusted(0, rect.height() - 25, 0, 0));
    }
    painter->setBrush(Qt::white);
    painter->drawPolygon(QPolygon{{rect.center().x() - 5, rect.center().y() - 9},
                                  {rect.center().x() - 5, rect.center().y() + 9},
                                  {rect.center().x() + 10, rect.center().y()}});
    painter->setPen(Qt::white);
    painter->setFont(smallFont(font, 11));
    painter->drawText(rect.adjusted(8, 0, -8, -5), Qt::AlignBottom | Qt::AlignLeft,
        QFontMetrics(painter->font()).elidedText(media["name"].toString(), Qt::ElideMiddle, rect.width() - 16));
    painter->restore();
}
QPixmap chatAvatar(const QJsonObject &entity, int size)
{
    const int side = qBound(1, size, 64);
    const auto name = entity["name"].toString(entity["sender"].toString()).left(256);
    const auto id = entity["sender_id"].toString(entity["id"].toString(name)).left(256);
    const auto encoded = entity["avatar"].toString();
    const bool candidate = !encoded.isEmpty() && encoded.size() <= 32 * 1024;
    const auto fingerprint = QString::fromLatin1(
        QCryptographicHash::hash(candidate ? encoded.toLatin1() : (id + '\n' + name).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
    QPixmap source;
    if (candidate)
    {
        const auto sourceKey = "avatar-source:" + fingerprint;
        static QCache<QString, bool> invalid(128);
        if (!QPixmapCache::find(sourceKey, &source) && !invalid.contains(fingerprint))
        {
            auto bytes = QByteArray::fromBase64(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "jpeg");
            const auto imageSize = bytes.startsWith("\xff\xd8") ? reader.size() : QSize();
            if (imageSize.isValid() && imageSize.width() <= 64 && imageSize.height() <= 64)
            {
                const auto image = reader.read();
                if (!image.isNull())
                {
                    source = QPixmap::fromImage(image);
                    QPixmapCache::insert(sourceKey, source);
                }
            }
            if (source.isNull())
                invalid.insert(fingerprint, new bool(true));
        }
    }
    const auto cacheKey =
        (source.isNull() ? "avatar-placeholder:" + id + ':' + name + (Theme::isDark() ? ":dark" : ":light")
                         : "avatar-rounded:" + fingerprint) + ':' +
        QString::number(side);
    QPixmap result;
    if (QPixmapCache::find(cacheKey, &result))
        return result;
    result = QPixmap(side, side);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRect(0, 0, side, side), qMin(4, side / 6), qMin(4, side / 6));
    painter.setClipPath(clip);
    if (!source.isNull())
    {
        const auto scaled =
            source.scaled(side, side, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        painter.drawPixmap((side - scaled.width()) / 2, (side - scaled.height()) / 2, scaled);
    }
    else
    {
        const QColor palette[]{themeColor("#cbd9cc", "#4a5b4e"), themeColor("#dad2c6", "#5d554a"),
                               themeColor("#d8d8df", "#535360"), themeColor("#b9c9d4", "#455866"),
                               themeColor("#d9c5bf", "#624e49"), themeColor("#cad5bb", "#505e46")};
        const auto seed = qHash(id);
        painter.fillRect(result.rect(), palette[seed % 6]);
        painter.setPen(themeColor("#52605b", "#dce5df"));
        auto font = QApplication::font();
        font.setPixelSize(qMax(8, qRound(side * .47)));
        painter.setFont(font);
        painter.drawText(result.rect(), Qt::AlignCenter, name.left(1));
    }
    painter.end();
    QPixmapCache::insert(cacheKey, result);
    return result;
}
void MessageDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const auto message = index.data(Qt::UserRole).toJsonObject();
    const bool mine = message["outgoing"].toBool();
    const auto geometry = geometryFor(message, index, option.rect, option.font);
    const auto bubble = geometry.bubble;
    const auto media = message["media"].toArray();
    p->save();
    p->setClipRect(option.rect);
    p->setRenderHint(QPainter::Antialiasing);
    p->setFont(option.font);
    if (!property("locatedId").toString().isEmpty() &&
        property("locatedId").toString() == message["id"].toString())
        p->fillRect(option.rect, themeColor("#eaeaea", "#353a3e"));
    if (geometry.timeHeight)
    {
        p->setFont(smallFont(option.font));
        p->setPen(themeColor("#9f9fa6", "#999ca3"));
        p->drawText(QRect(option.rect.left(), option.rect.top() + 3, option.rect.width(), 20),
                    Qt::AlignCenter, timeLabel(message["timestamp"].toInteger()));
    }
    if (message["status"] == "recalled")
    {
        p->setFont(smallFont(option.font));
        p->setPen(themeColor("#9f9fa6", "#999ca3"));
        const auto link = reeditRect(option, index);
        if (link.isEmpty())
            p->drawText(option.rect.adjusted(20, geometry.timeHeight, -20, 0),
                        Qt::AlignHCenter | Qt::AlignTop,
                        mine ? "你撤回了一条消息" : message["sender"].toString() + "撤回了一条消息");
        else
        {
            const int width = QFontMetrics(p->font()).horizontalAdvance("你撤回了一条消息");
            p->drawText(QRect(link.left() - width - 6, link.top(), width, link.height()), Qt::AlignVCenter,
                        "你撤回了一条消息");
            p->setPen(themeColor("#304b82", "#75bfff"));
            p->drawText(link, Qt::AlignVCenter, "重新编辑");
        }
        p->restore();
        return;
    }
    QRect avatar(mine ? option.rect.right() - 59 : option.rect.left() + 20, bubble.top(), 36, 36);
    p->drawPixmap(avatar, chatAvatar(message, avatar.width()));
    if (!mine)
    {
        p->setFont(smallFont(option.font));
        p->setPen(themeColor("#9f9fa6", "#999ca3"));
        p->drawText(QRect(bubble.left(), bubble.top() - 20, qMax(200, bubble.width()), 17),
                    Qt::AlignLeft | Qt::AlignVCenter, message["sender"].toString());
    }
    const bool textOnly = media.isEmpty();
    if (geometry.textHeight)
    {
        const auto background = mine ? themeColor("#9df29f", "#3c633c") : themeColor("#eeeef0", "#303234");
        p->setPen(Qt::NoPen);
        p->setBrush(background);
        p->drawRoundedRect(QRect(bubble.left(), bubble.top(), bubble.width(), geometry.textHeight), 5, 5);
        p->drawPolygon(mine ? QPolygon{{bubble.right() - 1, bubble.top() + 10},
                                       {bubble.right() + 5, bubble.top() + 15},
                                       {bubble.right() - 1, bubble.top() + 20}}
                            : QPolygon{{bubble.left() + 1, bubble.top() + 10},
                                       {bubble.left() - 5, bubble.top() + 15},
                                       {bubble.left() + 1, bubble.top() + 20}});
        p->setFont(wechatDisplayFont(option.font));
        p->setPen(themeColor("#19191a", "#ededed"));
        QTextLayout text(layoutString(message), wechatDisplayFont(option.font));
        layoutText(text, bubble.width() - 20);
        QList<QTextLayout::FormatRange> selections;
        if (selectedTextIndex == index && selectedTextLength > 0)
        {
            QTextLayout::FormatRange selection;
            selection.start = selectedTextStart;
            selection.length = selectedTextLength;
            selection.format.setBackground(option.palette.highlight());
            selection.format.setForeground(option.palette.highlightedText());
            selections.append(selection);
        }
        text.draw(p, {qreal(bubble.left() + 10), qreal(bubble.top() + 10)}, selections);
    }
    else if (textOnly)
    {
        p->setPen(Qt::NoPen);
        p->setBrush(mine ? themeColor("#9df29f", "#3c633c") : themeColor("#eeeef0", "#303234"));
        p->drawRoundedRect(bubble, 5, 5);
    }
    for (int i = 0; i < media.size(); ++i)
    {
        const auto item = media[i].toObject();
        const auto kind = item["kind"].toString();
        auto rect = geometry.media[i].translated(bubble.topLeft());
        auto local = item["_local"].toString();
        if (kind == "image" || kind == "gif")
        {
            const auto pixmap = mediaPixmap(item, property("gifCachePrefix").toString());
            if (!pixmap.isNull())
                p->drawPixmap(rect, pixmap, pixmap.rect());
            else
            {
                p->setPen(Qt::NoPen);
                p->setBrush(themeColor("#eeeef0", "#303234"));
                p->drawRoundedRect(rect, 4, 4);
                p->setPen(themeColor("#9f9fa6", "#999ca3"));
                p->setFont(smallFont(option.font));
                p->drawText(rect, Qt::AlignCenter, kind == "gif" ? "GIF · 点击下载" : "图片 · 点击下载");
            }
        }
        else if (kind == "video")
            paintVideoPreview(p, rect, item, option.font);
        else if (kind == "voice")
        {
            const auto state = property("voiceId").toString() == item["id"].toString()
                                   ? property("voiceState").toString() : QString{};
            p->setPen(Qt::NoPen);
            p->setBrush(mine ? themeColor("#9df29f", "#3c633c") : themeColor("#eeeef0", "#303234"));
            p->drawRoundedRect(rect, 5, 5);
            p->setPen(QPen(state == "playing" ? themeColor("#078850", "#6ee8aa")
                                            : themeColor("#444444", "#d6d9db"), 1.6));
            const QPoint center(rect.left() + 16, rect.center().y());
            for (int radius : {4, 8, 12})
                p->drawArc(QRect(center.x() - radius, center.y() - radius, radius * 2, radius * 2), -50 * 16,
                           100 * 16);
            p->setFont(option.font);
            p->drawText(rect.adjusted(39, 0, -9, 0), Qt::AlignVCenter,
                        state == "loading" ? QString("下载中…")
                        : state == "error" ? QString("无法播放")
                        : state == "playing" ? QString("播放中")
                        : item["duration"].toInt() > 0 ? QString::number(item["duration"].toInt()) + "″"
                                                       : QString("语音消息"));
        }
        else
        {
            p->setPen(QPen(themeColor("#e6e6e6", "#44474b"), 1));
            p->setBrush(themeColor("#ffffff", "#292929"));
            p->drawRoundedRect(rect, 4, 4);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor("#e99538"));
            QRect file(rect.right() - 58, rect.top() + 17, 40, 45);
            p->drawRoundedRect(file, 2, 2);
            p->setPen(Qt::white);
            p->setFont(smallFont(option.font, 11));
            p->drawText(file, Qt::AlignCenter, QFileInfo(item["name"].toString()).suffix().toUpper().left(4));
            p->setFont(option.font);
            p->setPen(themeColor("#19191a", "#ededed"));
            p->drawText(rect.adjusted(12, 12, -69, -40), Qt::AlignVCenter,
                        QFontMetrics(option.font)
                            .elidedText(item["name"].toString(), Qt::ElideMiddle, rect.width() - 81));
            p->setFont(smallFont(option.font, 11));
            p->setPen(themeColor("#9f9fa6", "#999ca3"));
            p->drawText(rect.adjusted(12, 42, -69, -14), Qt::AlignVCenter,
                        AttachmentStore::sizeLabel(item["size"].toInteger()));
        }
    }
    if (!geometry.quote.isEmpty())
    {
        const auto rect = geometry.quote;
        p->fillRect(mine ? rect.right() - 1 : rect.left(), rect.top(), 2, rect.height(),
                    themeColor("#e2e2e2", "#51565a"));
        p->setFont(wechatDisplayFont(smallFont(option.font)));
        p->setPen(themeColor("#9f9fa6", "#999ca3"));
        p->drawText(rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter,
                    QFontMetrics(p->font()).elidedText(messageQuoteLabel(message["quote"].toObject()),
                                                       Qt::ElideRight, rect.width() - 16));
    }
    const auto status = statusLabel(message);
    if (!status.isEmpty())
    {
        p->setFont(smallFont(option.font, 11));
        p->setPen(message["status"].toString() == "failed" || message["status"].toString() == "uncertain"
                      ? themeColor("#d8534b", "#ff8c85")
                      : themeColor("#9f9fa6", "#999ca3"));
        p->drawText(QRect(bubble.left(),
                          (geometry.quote.isEmpty() ? bubble.bottom() : geometry.quote.bottom()) + 4,
                          bubble.width(), 17),
                    Qt::AlignRight, status);
    }
    p->restore();
}
void MessageView::setVoiceState(const QString &id, const QString &state)
{
    itemDelegate()->setProperty("voiceId", id);
    itemDelegate()->setProperty("voiceState", state);
    viewport()->update();
}

StableListView::StableListView(QWidget *parent) : QListView(parent)
{
    smoothScroll = new SmoothScroll(this);
}
void StableListView::rememberReadingPosition()
{
    if (reading.pending || layingOut || !isVisible() || !model() || model()->rowCount() == 0)
        return;
    reading.index = indexAt(QPoint(1, 0));
    const auto message = reading.index.data(Qt::UserRole).toJsonObject();
    reading.id = message["id"].toString();
    reading.group = message["group"].toString();
    reading.offset = reading.index.isValid() ? visualRect(reading.index).top() : 0;
    auto *bar = verticalScrollBar();
    reading.scrollValue = bar->value();
    reading.bottom = property("followBottom").toBool() && !property("locatedHistory").toBool() &&
                     !smoothScroll->isRunning() && bar->maximum() - bar->value() <= 2;
    reading.pending = reading.index.isValid() || reading.bottom;
}
void StableListView::setModel(QAbstractItemModel *next)
{
    for (const auto &connection : anchorConnections)
        disconnect(connection);
    anchorConnections.clear();
    reading = {};
    if (next)
    {
        // Register before QListView so cached old row rectangles are captured before Qt invalidates them.
        anchorConnections.append(connect(next, &QAbstractItemModel::rowsAboutToBeInserted,
                                         this, [this] { rememberReadingPosition(); }));
        anchorConnections.append(connect(next, &QAbstractItemModel::rowsAboutToBeRemoved,
                                         this, [this] { rememberReadingPosition(); }));
        anchorConnections.append(connect(next, &QAbstractItemModel::modelAboutToBeReset,
                                         this, [this] { rememberReadingPosition(); }));
        anchorConnections.append(connect(next, &QAbstractItemModel::layoutAboutToBeChanged,
                                         this, [this] { rememberReadingPosition(); }));
    }
    QListView::setModel(next);
}
void StableListView::dataChanged(const QModelIndex &first, const QModelIndex &last,
                                 const QList<int> &roles)
{
    rememberReadingPosition();
    QListView::dataChanged(first, last, roles);
}
void StableListView::resizeEvent(QResizeEvent *event)
{
    rememberReadingPosition();
    QListView::resizeEvent(event);
}
void StableListView::doItemsLayout()
{
    if (layingOut || !reading.pending)
    {
        QListView::doItemsLayout();
        return;
    }
    layingOut = true;
    auto *bar = verticalScrollBar();
    const auto previous = bar->value();
    auto saved = reading;
    reading = {};
    // Qt's scrollbar handler can execute the pending layout before later valueChanged slots.
    // Measure the user's intervening movement here, before our own range clamping begins.
    if (previous != saved.scrollValue)
    {
        saved.offset -= previous - saved.scrollValue;
        saved.bottom = false;
    }
    {
        // Qt still needs scrollbar signals to update its pixel offset and scrollbar visibility.
        // Only postpone wheel cancellation while the same layout restores the document anchor.
        smoothScroll->setLayoutChanging(true);
        QListView::doItemsLayout();
        QModelIndex anchor = saved.index;
        if (!anchor.isValid() && !saved.id.isEmpty() && model())
            for (int row = 0; row < model()->rowCount(); ++row)
            {
                const auto index = model()->index(row, 0);
                const auto message = index.data(Qt::UserRole).toJsonObject();
                if (message["id"].toString() == saved.id && message["group"].toString() == saved.group)
                {
                    anchor = index;
                    break;
                }
            }
        if (saved.bottom && !property("locatedHistory").toBool() && !smoothScroll->isRunning())
            bar->setValue(bar->maximum());
        else if (anchor.isValid())
            bar->setValue(bar->value() + visualRect(anchor).top() - saved.offset);
        smoothScroll->offsetBy(bar->value() - previous);
        smoothScroll->setLayoutChanging(false);
    }
    layingOut = false;
    viewport()->update();
}
MessageView::MessageView(QWidget *parent) : StableListView(parent)
{
    setFont(QApplication::font());
    gifCachePrefix = "chat-gif:" + QString::number(quintptr(this), 16) + ":";
    auto *delegate = new MessageDelegate(this);
    delegate->setProperty("gifCachePrefix", gifCachePrefix);
    setItemDelegate(delegate);
    setObjectName("messages");
    setFrameShape(QFrame::NoFrame);
    setSelectionMode(QAbstractItemView::NoSelection);
    setMouseTracking(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSpacing(0);
    setUniformItemSizes(false);
    setResizeMode(QListView::Adjust);
    setProperty("followBottom", true);
    smoothScroll->reachedTop = [this] { requestOlder(); };
    smoothScroll->reachedBottom = [this]
    {
        if (property("locatedHistory").toBool())
            emit latestRequested();
    };
    gifUpdateTimer.setSingleShot(true);
    gifUpdateTimer.setInterval(0);
    connect(&gifUpdateTimer, &QTimer::timeout, this, &MessageView::updateVisibleGifs);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &MessageView::scheduleGifUpdate);
    connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, &MessageView::requestOlder);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { emit viewportChanged(); });
}
MessageView::~MessageView()
{
    clearGifs();
    if (observedWindow)
        observedWindow->removeEventFilter(this);
}
void MessageView::requestOlder()
{
    if (model() && model()->rowCount() > 0 && verticalScrollBar()->value() <= 40)
        emit olderRequested();
}
void MessageView::stopScrollAnimation()
{
    if (smoothScroll)
        smoothScroll->stop();
}
bool MessageView::scrollAnimationActive() const
{
    return smoothScroll && smoothScroll->isRunning();
}
bool MessageView::locateMessage(const QString &id)
{
    if (!model())
        return false;
    for (int row = 0; row < model()->rowCount(); ++row)
    {
        const auto index = model()->index(row, 0);
        if (index.data(Qt::UserRole).toJsonObject()["id"].toString() != id)
            continue;
        stopScrollAnimation();
        scrollTo(index, QAbstractItemView::PositionAtCenter);
        itemDelegate()->setProperty("locatedId", id);
        viewport()->update();
        QTimer::singleShot(1000, this,
                           [this, id]
                           {
                               if (itemDelegate()->property("locatedId").toString() == id)
                               {
                                   itemDelegate()->setProperty("locatedId", QString());
                                   viewport()->update();
                               }
                           });
        return true;
    }
    return false;
}
QString MessageView::selectedText() const
{
    if (!selectedTextIndex.isValid() || textAnchor == textCursor)
        return {};
    const auto message = selectedTextIndex.data(Qt::UserRole).toJsonObject();
    if (message["status"] == "recalled" || message["text"].toString() != selectedTextSource)
        return {};
    const auto offsets = displaySourceOffsets(selectedTextSource);
    const int first = qBound(0, qMin(textAnchor, textCursor), int(offsets.size()) - 1);
    const int last = qBound(first, qMax(textAnchor, textCursor), int(offsets.size()) - 1);
    return selectedTextSource.mid(offsets[first], offsets[last] - offsets[first]);
}
void MessageView::clearTextSelection()
{
    const auto previous = selectedTextIndex;
    selectedTextIndex = QPersistentModelIndex();
    selectedTextSource.clear();
    textAnchor = textCursor = 0;
    if (auto *delegate = qobject_cast<MessageDelegate *>(itemDelegate()))
        delegate->setTextSelection({}, 0, 0);
    if (previous.isValid())
        viewport()->update(visualRect(previous));
}
void MessageView::validateTextSelection()
{
    if (!selectedTextIndex.isValid())
    {
        clearTextSelection();
        return;
    }
    const auto message = selectedTextIndex.data(Qt::UserRole).toJsonObject();
    if (message["status"] == "recalled" || message["text"].toString() != selectedTextSource)
        clearTextSelection();
}
void MessageView::updateTextSelection(int position)
{
    validateTextSelection();
    if (!selectedTextIndex.isValid())
        return;
    textCursor = qBound(0, position, int(wechatDisplayText(selectedTextSource).size()));
    if (auto *delegate = qobject_cast<MessageDelegate *>(itemDelegate()))
        delegate->setTextSelection(selectedTextIndex, qMin(textAnchor, textCursor), qAbs(textCursor - textAnchor));
    viewport()->update(visualRect(selectedTextIndex));
}
int MessageView::textPositionAt(const QModelIndex &index, const QPoint &point, bool clamp) const
{
    if (!index.isValid())
        return -1;
    const auto message = index.data(Qt::UserRole).toJsonObject();
    if (message["status"] == "recalled" || message["text"].toString().isEmpty())
        return -1;
    const auto geometry = geometryFor(message, index, visualRect(index), font());
    const auto content = textContentRect(geometry);
    if (!clamp && !content.contains(point))
        return -1;
    QTextLayout layout(layoutString(message), wechatDisplayFont(font()));
    layoutText(layout, content.width());
    const auto relative = point - content.topLeft();
    if (relative.y() < 0)
        return 0;
    for (int i = 0; i < layout.lineCount(); ++i)
    {
        const auto line = layout.lineAt(i);
        if (relative.y() < line.y() + line.height())
        {
            const auto cursor = line.xToCursor(relative.x());
            return layout.isValidCursorPosition(cursor) ? cursor : layout.previousCursorPosition(cursor);
        }
    }
    return int(layout.text().size());
}
void MessageView::wheelEvent(QWheelEvent *event)
{
    QListView::wheelEvent(event);
    if (event->angleDelta().y() > 0 || event->pixelDelta().y() > 0)
        requestOlder();
    else if ((event->angleDelta().y() < 0 || event->pixelDelta().y() < 0) &&
             property("locatedHistory").toBool() &&
             verticalScrollBar()->value() == verticalScrollBar()->maximum())
        emit latestRequested();
}
void MessageView::keyPressEvent(QKeyEvent *event)
{
    stopScrollAnimation();
    if (event->matches(QKeySequence::Copy))
    {
        const auto text = selectedText();
        if (!text.isEmpty())
        {
            QApplication::clipboard()->setText(text);
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && selectedTextIndex.isValid())
    {
        clearTextSelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_End && property("locatedHistory").toBool())
    {
        emit latestRequested();
        event->accept();
        return;
    }
    QListView::keyPressEvent(event);
    if (event->key() == Qt::Key_Up || event->key() == Qt::Key_PageUp || event->key() == Qt::Key_Home)
        requestOlder();
}
void MessageView::setModel(QAbstractItemModel *next)
{
    for (const auto &connection : modelConnections)
        disconnect(connection);
    modelConnections.clear();
    clearGifs();
    clearTextSelection();
    stopScrollAnimation();
    StableListView::setModel(next);
    if (!next)
        return;
    modelConnections.append(connect(next, &QAbstractItemModel::modelAboutToBeReset, this, [this]
    {
        stopScrollAnimation();
        clearTextSelection();
    }));
    modelConnections.append(connect(next, &QAbstractItemModel::layoutAboutToBeChanged,
                                    this, &MessageView::clearTextSelection));
    modelConnections.append(connect(next, &QAbstractItemModel::rowsAboutToBeRemoved, this,
        [this](const QModelIndex &parent, int first, int last)
        {
            if (selectedTextIndex.isValid() && selectedTextIndex.parent() == parent &&
                selectedTextIndex.row() >= first && selectedTextIndex.row() <= last)
                clearTextSelection();
        }));
    modelConnections.append(connect(next, &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex &first, const QModelIndex &last)
        {
            if (selectedTextIndex.isValid() && selectedTextIndex.parent() == first.parent() &&
                selectedTextIndex.row() >= first.row() && selectedTextIndex.row() <= last.row())
                validateTextSelection();
        }));
    modelConnections.append(connect(next, &QObject::destroyed, this, &MessageView::clearTextSelection));
    modelConnections.append(
        connect(next, &QAbstractItemModel::layoutAboutToBeChanged, this, &MessageView::clearGifs));
    modelConnections.append(
        connect(next, &QAbstractItemModel::rowsAboutToBeRemoved, this, &MessageView::clearGifs));
    modelConnections.append(connect(next, &QObject::destroyed, this, &MessageView::clearGifs));
    modelConnections.append(
        connect(next, &QAbstractItemModel::modelReset, this, &MessageView::scheduleGifUpdate));
    modelConnections.append(
        connect(next, &QAbstractItemModel::layoutChanged, this, &MessageView::scheduleGifUpdate));
    modelConnections.append(
        connect(next, &QAbstractItemModel::rowsInserted, this, &MessageView::scheduleGifUpdate));
    modelConnections.append(
        connect(next, &QAbstractItemModel::rowsRemoved, this, &MessageView::scheduleGifUpdate));
    modelConnections.append(
        connect(next, &QAbstractItemModel::dataChanged, this, &MessageView::scheduleGifUpdate));
    scheduleGifUpdate();
}
void MessageView::scheduleGifUpdate()
{
    if (!gifUpdateTimer.isActive())
        gifUpdateTimer.start();
}
void MessageView::removeGif(const QString &path)
{
    auto animation = gifs.take(path);
    animation.movie->stop();
    animation.movie->deleteLater();
    QPixmapCache::remove(animation.cacheKey);
}
void MessageView::clearGifs()
{
    gifUpdateTimer.stop();
    const auto paths = gifs.keys();
    for (const auto &path : paths)
        removeGif(path);
}
void MessageView::updateVisibleGifs()
{
    if (!model() || !isVisible() || window()->isMinimized())
    {
        clearGifs();
        return;
    }
    struct VisibleGif
    {
        QJsonObject media;
        QList<QPersistentModelIndex> indices;
    };
    QHash<QString, VisibleGif> visible;
    const auto bounds = viewport()->rect();
    const auto first = indexAt(QPoint(1, 0));
    for (int row = qMax(0, first.row()); row < model()->rowCount(); ++row)
    {
        const auto index = model()->index(row, 0);
        const auto rect = visualRect(index);
        if (rect.top() > bounds.bottom())
            break;
        if (!rect.intersects(bounds))
            continue;
        const auto message = index.data(Qt::UserRole).toJsonObject();
        if (message["status"] == "recalled")
            continue;
        const auto media = message["media"].toArray();
        const auto geometry = geometryFor(message, index, rect, font());
        for (int i = 0; i < media.size(); ++i)
        {
            const auto item = media[i].toObject();
            const auto path = item["_local"].toString();
            if (item["kind"].toString() != "gif" || path.isEmpty() ||
                !geometry.media[i].translated(geometry.bubble.topLeft()).intersects(bounds))
                continue;
            const auto identity = path + ':' + item["sha256"].toString();
            if (!visible.contains(identity) && visible.size() >= 4)
                continue;
            auto &entry = visible[identity];
            entry.media = item;
            if (!entry.indices.contains(index))
                entry.indices.append(index);
        }
    }
    const auto previous = gifs.keys();
    for (const auto &path : previous)
        if (!visible.contains(path))
            removeGif(path);
    for (auto entry = visible.cbegin(); entry != visible.cend(); ++entry)
    {
        const auto identity = entry.key();
        if (gifs.contains(identity))
        {
            gifs[identity].indices = entry->indices;
            continue;
        }
        auto *movie = new QMovie(entry->media["_local"].toString(), "gif", this);
        movie->setObjectName("visibleChatGif");
        movie->setCacheMode(QMovie::CacheNone);
        movie->setScaledSize(previewSize(entry->media));
        if (!movie->isValid())
        {
            delete movie;
            continue;
        }
        gifs.insert(identity, {movie, gifCachePrefix + identity, entry->indices});
        connect(movie, &QMovie::frameChanged, this,
                [this, movie, identity](int)
                {
                    const auto animation = gifs.constFind(identity);
                    if (animation == gifs.cend() || animation->movie != movie)
                        return;
                    QPixmapCache::insert(animation->cacheKey, movie->currentPixmap());
                    for (const auto &index : animation->indices)
                        if (index.isValid())
                        {
                            const auto dirty = visualRect(index).intersected(viewport()->rect());
                            if (!dirty.isEmpty())
                                viewport()->update(dirty);
                        }
                });
        movie->start();
    }
}
void MessageView::showEvent(QShowEvent *event)
{
    QListView::showEvent(event);
    if (observedWindow != window())
    {
        if (observedWindow)
            observedWindow->removeEventFilter(this);
        observedWindow = window();
        observedWindow->installEventFilter(this);
    }
    scheduleGifUpdate();
}
void MessageView::hideEvent(QHideEvent *event)
{
    stopScrollAnimation();
    selectingText = false;
    clearGifs();
    QListView::hideEvent(event);
}
bool MessageView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == observedWindow)
    {
        if (event->type() == QEvent::Hide ||
            (event->type() == QEvent::WindowStateChange && window()->isMinimized()))
            clearGifs();
        else if (event->type() == QEvent::Show || event->type() == QEvent::WindowStateChange)
            scheduleGifUpdate();
    }
    return QListView::eventFilter(watched, event);
}
void MessageView::resizeEvent(QResizeEvent *event)
{
    StableListView::resizeEvent(event);
    doItemsLayout();
    scheduleGifUpdate();
    emit viewportChanged();
}
void MessageView::mousePressEvent(QMouseEvent *event)
{
    const auto index = indexAt(event->pos());
    if (event->button() == Qt::LeftButton)
    {
        selectingText = false;
        clearTextSelection();
        const auto position = textPositionAt(index, event->pos(), false);
        if (position >= 0)
        {
            stopScrollAnimation();
            // Qt normally scrolls a newly current item into view. A tall bubble
            // must remain at the exact position where the drag began.
            const bool autoScroll = hasAutoScroll();
            setAutoScroll(false);
            setCurrentIndex(index);
            setFocus(Qt::MouseFocusReason);
            setAutoScroll(autoScroll);
            selectedTextIndex = index;
            selectedTextSource = index.data(Qt::UserRole).toJsonObject()["text"].toString();
            textAnchor = position;
            selectingText = true;
            updateTextSelection(position);
            viewport()->setCursor(Qt::IBeamCursor);
            event->accept();
            return;
        }
    }
    else if (event->button() == Qt::RightButton && selectedTextIndex != index)
        clearTextSelection();
    QListView::mousePressEvent(event);
}
void MessageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && selectingText)
    {
        selectingText = false;
        const auto position = textPositionAt(selectedTextIndex, event->pos(), true);
        if (position >= 0)
            updateTextSelection(position);
        event->accept();
        return;
    }
    auto index = indexAt(event->pos());
    if (event->button() == Qt::LeftButton && index.isValid())
    {
        auto message = index.data(Qt::UserRole).toJsonObject();
        QStyleOptionViewItem option;
        option.rect = visualRect(index);
        option.font = font();
        if (MessageDelegate::reeditRect(option, index).contains(event->pos()))
        {
            emit reeditRequested(message["id"].toString());
            return;
        }
        const auto geometry = geometryFor(message, index, visualRect(index), font());
        auto media = message["media"].toArray();
        for (int i = 0; i < geometry.media.size(); ++i)
            if (geometry.media[i].translated(geometry.bubble.topLeft()).contains(event->pos()))
            {
                emit mediaClicked(media[i].toObject(), false);
                return;
            }
    }
    QListView::mouseReleaseEvent(event);
}
void MessageView::mouseMoveEvent(QMouseEvent *event)
{
    if (selectingText)
    {
        const auto position = textPositionAt(selectedTextIndex, event->pos(), true);
        if (position >= 0)
            updateTextSelection(position);
        viewport()->setCursor(Qt::IBeamCursor);
        event->accept();
        return;
    }
    const auto index = indexAt(event->pos());
    QStyleOptionViewItem option;
    option.rect = visualRect(index);
    option.font = font();
    bool onText = false;
    if (index.isValid())
    {
        const auto message = index.data(Qt::UserRole).toJsonObject();
        if (message["status"] != "recalled" && !message["text"].toString().isEmpty())
            onText = textContentRect(geometryFor(message, index, option.rect, option.font)).contains(event->pos());
    }
    viewport()->setCursor(index.isValid() && MessageDelegate::reeditRect(option, index).contains(event->pos())
                              ? Qt::PointingHandCursor
                              : onText ? Qt::IBeamCursor : Qt::ArrowCursor);
    QListView::mouseMoveEvent(event);
}
class ChatContextMenu : public QMenu
{
  public:
    explicit ChatContextMenu(QWidget *parent) : QMenu(parent)
    {
        setObjectName("messageContextMenu");
        setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        setFixedWidth(162 + shadowMargin * 2);
        auto uiFont = QApplication::font();
        uiFont.setPixelSize(14);
        setFont(uiFont);
        setStyleSheet("QMenu { padding:30px; border:0; background:transparent; }"
                      "QMenu::item { padding:7px 12px; } QMenu::separator { height:1px; margin:5px 8px; }");
    }
    void execAtPanel(const QPoint &anchor)
    {
        ensurePolished();
        adjustSize();
        auto position = anchor - QPoint(shadowMargin, shadowMargin);
        const auto *targetScreen = QGuiApplication::screenAt(anchor);
        if (!targetScreen)
            targetScreen = screen();
        if (targetScreen)
        {
            const auto available = targetScreen->availableGeometry().adjusted(2, 2, -2, -2);
            position.setX(qBound(available.left(), position.x(),
                                 qMax(available.left(), available.right() - width() + 1)));
            position.setY(qBound(available.top(), position.y(),
                                 qMax(available.top(), available.bottom() - height() + 1)));
        }
        exec(position);
    }

  protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (!panelRect().contains(event->position()))
        {
            close();
            event->accept();
            return;
        }
        QMenu::mousePressEvent(event);
    }
    void paintEvent(QPaintEvent *) override
    {
        const auto ratio = devicePixelRatioF();
        if (cachedSize != size() || !qFuzzyCompare(cachedRatio, ratio) || cachedDark != Theme::isDark())
            updateBackground(ratio);
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.drawPixmap(QPoint(), background);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setFont(font());
        for (auto *action : actions())
        {
            const auto area = actionGeometry(action);
            if (action->isSeparator())
            {
                painter.setPen(themeColor("#e5e5e5", "#4b4f53"));
                painter.drawLine(area.left() + 7, area.center().y(), area.right() - 7, area.center().y());
                continue;
            }
            const bool selected = activeAction() == action && action->isEnabled();
            if (selected)
            {
                painter.setPen(Qt::NoPen);
                painter.setBrush(themeColor("#00c777", "#087f52"));
                painter.drawRoundedRect(area, 5, 5);
            }
            const auto color = !action->isEnabled()                      ? themeColor("#9f9fa6", "#777c81")
                               : selected                                ? QColor(Qt::white)
                               : action->objectName() == "recallMessage" ? themeColor("#e55757", "#ff8c85")
                                                                         : themeColor("#19191a", "#ededed");
            painter.setPen(color);
            const bool checkable = action->isCheckable();
            if (checkable && action->isChecked())
            {
                painter.save();
                painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                const auto centerY = area.center().y();
                painter.drawPolyline(QPolygonF{{qreal(area.left() + 10), qreal(centerY)},
                                               {qreal(area.left() + 14), qreal(centerY + 4)},
                                               {qreal(area.left() + 21), qreal(centerY - 4)}});
                painter.restore();
            }
            const auto icon = action->property("chatIcon").toString();
            if (!checkable && !icon.isEmpty())
            {
                painter.save();
                painter.translate(area.left() + 10, area.center().y() - 7);
                painter.setPen(QPen(color, 1.2));
                painter.setBrush(Qt::NoBrush);
                if (icon == "copy")
                {
                    painter.drawRoundedRect(QRectF(0, 0, 9, 10), 1, 1);
                    painter.fillRect(QRectF(4, 4, 9, 10), selected ? themeColor("#00c777", "#087f52")
                                                                : themeColor("#f7f7f7", "#303234"));
                    painter.drawRoundedRect(QRectF(4, 4, 9, 10), 1, 1);
                }
                else if (icon == "quote")
                {
                    painter.drawRoundedRect(QRectF(0, 1, 14, 10), 1, 1);
                    painter.drawLine(QPointF(3, 11), QPointF(3, 14));
                    painter.drawLine(QPointF(3, 14), QPointF(6, 11));
                    painter.drawLine(QPointF(4, 4), QPointF(4, 7));
                    painter.drawLine(QPointF(9, 4), QPointF(9, 7));
                }
                else
                {
                    QPainterPath arrow;
                    arrow.moveTo(2, 4);
                    arrow.lineTo(8, 4);
                    arrow.cubicTo(16, 4, 16, 13, 6, 13);
                    painter.drawPath(arrow);
                    painter.drawPolyline(QPolygonF{{5, 1}, {2, 4}, {5, 7}});
                }
                painter.restore();
            }
            const auto textArea = area.adjusted(checkable || !icon.isEmpty() ? 30 : 12, 0, -8, 0);
            painter.drawText(textArea, Qt::AlignVCenter | Qt::AlignLeft,
                             fontMetrics().elidedText(action->text(), Qt::ElideRight, textArea.width()));
        }
    }

  private:
    static constexpr int shadowMargin = 24;
    QRectF panelRect() const
    {
        return QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
    }
    void updateBackground(qreal ratio)
    {
        QImage image(QSize(qCeil(width() * ratio), qCeil(height() * ratio)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        const auto panel = panelRect();
        const auto center = panel.center();
        const qreal radius = 6, halfWidth = panel.width() / 2, halfHeight = panel.height() / 2;
        const auto denominator = 8 * std::sqrt(2.0);
        // Signed distance to a rounded panel gives a smooth alpha falloff, including the corners.
        // Rasterize once per popup size/DPI; hover only blits the cached background.
        for (int y = 0; y < image.height(); ++y)
        {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const qreal dy = qAbs((y + .5) / ratio - center.y()) - halfHeight + radius;
            for (int x = 0; x < image.width(); ++x)
            {
                const qreal dx = qAbs((x + .5) / ratio - center.x()) - halfWidth + radius;
                const qreal outsideX = qMax(dx, 0.0), outsideY = qMax(dy, 0.0);
                const qreal distance =
                    std::sqrt(outsideX * outsideX + outsideY * outsideY) + qMin(qMax(dx, dy), 0.0) - radius;
                const int alpha = qBound(0, qRound(32 * std::erfc(distance / denominator)), 64);
                line[x] = qRgba(0, 0, 0, alpha);
            }
        }
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(themeColor("#d6d6d6", "#4b4f53"), 1));
        painter.setBrush(themeColor("#f7f7f7", "#303234"));
        painter.drawRoundedRect(panel.adjusted(.5, .5, -.5, -.5), radius, radius);
        painter.end();
        background = QPixmap::fromImage(image);
        cachedSize = size();
        cachedRatio = ratio;
        cachedDark = Theme::isDark();
    }
    QPixmap background;
    QSize cachedSize;
    qreal cachedRatio = 0;
    bool cachedDark = false;
};
QMenu *createChatContextMenu(QWidget *parent) { return new ChatContextMenu(parent); }

void execChatContextMenu(QMenu *menu, const QPoint &anchor)
{
    if (auto *styled = dynamic_cast<ChatContextMenu *>(menu))
        styled->execAtPanel(anchor);
}

QMenu *MessageView::quoteContextMenuFor(const QModelIndex &index)
{
    if (!index.isValid())
        return nullptr;
    const auto message = index.data(Qt::UserRole).toJsonObject();
    auto quote = message["quote"].toObject();
    if (quote["id"].toString().isEmpty() || message["status"] == "recalled")
        return nullptr;
    quote["group"] = message["group"];
    auto *menu = new ChatContextMenu(this);
    auto *locate = menu->addAction("定位到原文位置", this, [this, quote] { emit locateRequested(quote); });
    locate->setObjectName("locateQuote");
    locate->setEnabled(!quote["unavailable"].toBool());
    return menu;
}
QMenu *MessageView::contextMenuFor(const QModelIndex &index, bool avatar)
{
    if (!index.isValid())
        return nullptr;
    const auto message = index.data(Qt::UserRole).toJsonObject();
    if (message["status"] == "recalled")
        return nullptr;
    auto *menu = new ChatContextMenu(this);
    if (avatar)
    {
        if (!message["sender_id"].toString().isEmpty())
        {
            const QJsonObject member{{"id", message["sender_id"]}, {"name", message["sender"]}};
            auto *action = menu->addAction("@ " + member["name"].toString(), this,
                                           [this, member] { emit mentionRequested(member); });
            action->setObjectName("mentionMember");
        }
        if (!menu->actions().isEmpty())
            return menu;
        delete menu;
        return nullptr;
    }
    if (!message["text"].toString().isEmpty())
    {
        const auto selected = selectedTextIndex == index ? selectedText() : QString{};
        const auto copyText = selected.isEmpty() ? message["text"].toString() : selected;
        auto *copy = menu->addAction("复制", this, [copyText]
                                     { QApplication::clipboard()->setText(copyText); });
        copy->setObjectName("copyMessage");
        copy->setProperty("chatIcon", "copy");
        auto *quote = menu->addAction("引用", this, [this, message] { emit quoteRequested(message); });
        quote->setObjectName("quoteMessage");
        quote->setProperty("chatIcon", "quote");
    }
    for (const auto &entry : message["media"].toArray())
    {
        auto media = entry.toObject();
        auto *save = menu->addAction("保存 " + media["name"].toString(), this,
                                     [this, media] { emit mediaClicked(media, true); });
        const auto allowed = property("canDownload");
        save->setEnabled(!allowed.isValid() || allowed.toBool());
    }
    if (canRecallMessage(message, QDateTime::currentMSecsSinceEpoch()))
    {
        if (!menu->actions().isEmpty())
            menu->addSeparator();
        auto *recall = menu->addAction("撤回", this, [this, message] { emit recallRequested(message); });
        recall->setObjectName("recallMessage");
        recall->setProperty("chatIcon", "recall");
    }
    const auto error = message["recall_error"].toString(message["error"].toString());
    if (!error.isEmpty())
    {
        auto *action = menu->addAction(error);
        action->setEnabled(false);
    }
    if (!menu->actions().isEmpty())
        return menu;
    delete menu;
    return nullptr;
}
void MessageView::contextMenuEvent(QContextMenuEvent *event)
{
    const auto index = event->reason() == QContextMenuEvent::Keyboard && !indexAt(event->pos()).isValid()
                           ? currentIndex()
                           : indexAt(event->pos());
    if (!index.isValid())
        return;
    const auto message = index.data(Qt::UserRole).toJsonObject();
    const auto row = visualRect(index);
    const auto geometry = geometryFor(message, index, row, font());
    const auto showMenu = [this, event](QMenu *menu, const QPoint &anchor)
    {
        if (!menu)
            return;
        static_cast<ChatContextMenu *>(menu)->execAtPanel(viewport()->mapToGlobal(anchor));
        delete menu;
        event->accept();
    };
    if (geometry.quote.contains(event->pos()))
    {
        showMenu(quoteContextMenuFor(index), event->pos());
        return;
    }
    const QRect avatar(message["outgoing"].toBool() ? row.right() - 59 : row.left() + 20,
                       geometry.bubble.top(), 36, 36);
    const bool onAvatar = avatar.contains(event->pos());
    if (!onAvatar && event->reason() != QContextMenuEvent::Keyboard &&
        !geometry.bubble.contains(event->pos()))
        return;
    const auto anchor = event->reason() == QContextMenuEvent::Keyboard
                            ? geometry.bubble.bottomRight() + QPoint(2, 2)
                            : event->pos();
    showMenu(contextMenuFor(index, onAvatar), anchor);
}
void GroupDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const auto group = index.data(Qt::UserRole).toJsonObject();
    const auto rect = option.rect;
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    p->save();
    p->setClipRect(rect);
    p->setRenderHint(QPainter::Antialiasing);
    p->fillRect(rect, selected                                         ? themeColor("#15ac70", "#245c44")
                      : option.state.testFlag(QStyle::State_MouseOver) ? themeColor("#e0e0e2", "#333638")
                      : group["pinned"].toBool()                       ? themeColor("#e6e6e8", "#2d3032")
                                                                       : themeColor("#eeeef0", "#262626"));
    if (!selected)
    {
        p->setPen(themeColor("#dddddf", "#3a3d40"));
        p->drawLine(rect.left() + 56, rect.bottom(), rect.right() - 12, rect.bottom());
    }
    const QRect avatar(rect.left() + 12, rect.top() + 16, 36, 36);
    p->drawPixmap(avatar, chatAvatar(group, avatar.width()));
    auto font = option.font;
    font.setPixelSize(option.font.pixelSize() > 0 ? qMin(14, option.font.pixelSize()) : 14);
    font.setWeight(QFont::Normal);
    p->setFont(font);
    p->setPen(selected ? Qt::white : themeColor("#19191a", "#ededed"));
    const auto timestamp = group["timestamp"].toInteger();
    const int textWidth = rect.width() - 69 - (timestamp ? 45 : 0);
    p->drawText(QRect(rect.left() + 57, rect.top() + 11, textWidth, 23), Qt::AlignVCenter,
                QFontMetrics(font).elidedText(group["name"].toString(), Qt::ElideRight, textWidth));
    font.setPixelSize(12);
    p->setFont(wechatDisplayFont(font));
    p->setPen(selected ? QColor("#def5ed") : themeColor("#9f9fa6", "#999ca3"));
    p->drawText(
        QRect(rect.left() + 57, rect.top() + 34, rect.width() - 70, 20), Qt::AlignVCenter,
        QFontMetrics(p->font()).elidedText(wechatDisplayText(group["preview"].toString()),
                                         Qt::ElideRight, rect.width() - 70));
    if (timestamp)
    {
        font.setPixelSize(10);
        p->setFont(font);
        p->drawText(QRect(rect.right() - 51, rect.top() + 14, 39, 17), Qt::AlignRight | Qt::AlignVCenter,
                    QDateTime::fromMSecsSinceEpoch(timestamp).toString("HH:mm"));
    }
    const auto unread = group["unread"].toInt();
    if (unread > 0)
    {
        const QRect badge(avatar.right() - 6, avatar.top() - 5, unread > 9 ? 20 : 15, 15);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor("#ff5059"));
        p->drawRoundedRect(badge, 7.5, 7.5);
        p->setPen(Qt::white);
        p->setFont(smallFont(option.font, 10));
        p->drawText(badge, Qt::AlignCenter, unread > 99 ? "99+" : QString::number(unread));
    }
    p->restore();
}
