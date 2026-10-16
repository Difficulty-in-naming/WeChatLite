#include "attachment.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QMimeData>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

AttachmentStore::AttachmentStore(QObject *parent) : QObject(parent)
{
}

QString AttachmentStore::kindFor(const QString &path)
{
    QFile file(path);
    QByteArray header;
    if (file.open(QIODevice::ReadOnly))
        header = file.read(32);
    if (header.startsWith("GIF87a") || header.startsWith("GIF89a"))
        return "gif";
    if (header.startsWith("\x89PNG\r\n\x1a\n") || header.startsWith("\xff\xd8\xff") ||
        (header.startsWith("RIFF") && header.mid(8, 4) == "WEBP"))
        return "image";
    auto ext = QFileInfo(path).suffix().toLower();
    if (QStringList{"mp4", "mov", "m4v", "webm", "mkv", "avi"}.contains(ext))
        return "video";
    if (QStringList{"mp3", "wav", "ogg", "opus", "m4a", "silk", "amr"}.contains(ext))
        return "voice";
    return "file";
}
QString AttachmentStore::safeName(const QString &name)
{
    QString result = name;
    result.replace('\\', '/');
    result = result.section('/', -1);
    for (auto &ch : result)
        if (ch.unicode() < 32 || QString("<>:\"/\\|?*").contains(ch))
            ch = '_';
    result = result.left(160).trimmed();
    while (result.endsWith('.') || result.endsWith(' '))
        result.chop(1);
    return result.isEmpty() || result == "." || result == ".." ? QStringLiteral("附件") : result;
}
QString AttachmentStore::sizeLabel(qint64 bytes)
{
    if (bytes < 1024)
        return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    if (bytes < 1024LL * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 1) + " MB";
    return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 1) + " GB";
}
void AttachmentStore::addFiles(const QStringList &paths)
{
    if (locked)
    {
        emit warning("正在发送附件，请等待本次发送完成");
        return;
    }
    for (const auto &path : paths)
    {
        QFileInfo info(path);
        if (!info.exists() || !info.isFile() || !info.isReadable())
        {
            emit warning("无法附加文件：" + info.fileName());
            continue;
        }
        if (info.size() <= 0 || info.size() > 512LL * 1024 * 1024)
        {
            emit warning("附件需要大于 0 且不超过 512 MB：" + info.fileName());
            continue;
        }
        auto canonical = info.canonicalFilePath();
        if (std::any_of(items.begin(), items.end(), [&](const auto &item) { return item.path == canonical; }))
            continue;
        if (items.size() >= 12)
        {
            emit warning("一次最多附加 12 个文件");
            break;
        }
        items.push_back(
            {canonical, info.fileName(), kindFor(canonical), {}, info.size(), info.lastModified(), false});
    }
    emit changed();
}
bool AttachmentStore::addImage(const QMimeData *mime)
{
    if (locked)
    {
        emit warning("正在发送附件，请等待本次发送完成");
        return false;
    }
    if (items.size() >= 12)
    {
        emit warning("一次最多附加 12 个文件");
        return false;
    }
    QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/drafts";
    if (!QDir().mkpath(directory))
    {
        emit warning("无法保存剪贴板图片");
        return false;
    }
    bool gif = mime->hasFormat("image/gif");
    QString path =
        directory + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + (gif ? ".gif" : ".png");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        emit warning(file.errorString());
        return false;
    }
    if (gif)
    {
        auto bytes = mime->data("image/gif");
        if (bytes.size() > 512LL * 1024 * 1024 ||
            (!bytes.startsWith("GIF87a") && !bytes.startsWith("GIF89a")) || file.write(bytes) != bytes.size())
        {
            emit warning("剪贴板 GIF 无效");
            return false;
        }
    }
    else
    {
        auto image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull() || qint64(image.width()) * image.height() > 40'000'000 ||
            !image.save(&file, "PNG"))
        {
            emit warning("剪贴板图片无效或尺寸过大");
            return false;
        }
    }
    if (!file.commit())
    {
        emit warning(file.errorString());
        return false;
    }
    addFiles({path});
    if (!items.isEmpty() && items.last().path == QFileInfo(path).canonicalFilePath())
    {
        items.last().temporary = true;
        items.last().name =
            "粘贴图片_" + QDateTime::currentDateTime().toString("HHmmss") + (gif ? ".gif" : ".png");
        emit changed();
        return true;
    }
    QFile::remove(path);
    return false;
}
void AttachmentStore::remove(int index)
{
    if (locked)
        return;
    if (index < 0 || index >= items.size())
        return;
    auto item = items.takeAt(index);
    if (item.temporary)
        QFile::remove(item.path);
    emit changed();
}
void AttachmentStore::clear(bool discard)
{
    if (discard)
        for (const auto &item : items)
            if (item.temporary)
                QFile::remove(item.path);
    items.clear();
    emit changed();
}
QString AttachmentStore::validate() const
{
    for (const auto &item : items)
    {
        QFileInfo info(item.path);
        if (!info.isFile() || !info.isReadable() || info.size() != item.size ||
            info.lastModified() != item.modified)
            return "附件已移动或改变，请重新附加：" + item.name;
    }
    return {};
}
QJsonArray AttachmentStore::serialize() const
{
    QJsonArray array;
    for (const auto &item : items)
        array.append(QJsonObject{{"path", item.path},
                                 {"name", item.name},
                                 {"kind", item.kind},
                                 {"size", item.size},
                                 {"modified", item.modified.toMSecsSinceEpoch()},
                                 {"temporary", item.temporary},
                                 {"uploaded", item.uploaded}});
    return array;
}
void AttachmentStore::restore(const QJsonArray &array)
{
    items.clear();
    for (const auto &entry : array)
    {
        auto obj = entry.toObject();
        items.append({obj["path"].toString(), obj["name"].toString(), obj["kind"].toString(),
                      obj["uploaded"].toString(), obj["size"].toInteger(),
                      QDateTime::fromMSecsSinceEpoch(obj["modified"].toInteger()),
                      obj["temporary"].toBool()});
    }
    emit changed();
}
