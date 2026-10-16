#include "media_storage.h"
#include "attachment.h"
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <algorithm>

namespace
{
bool link(const QFileInfo &info) { return info.isSymLink() || info.isJunction(); }
bool safeRoot(const QString &path)
{
    if (!QDir::isAbsolutePath(path)) return false;
    auto current = QDir::cleanPath(path);
    while (!current.isEmpty())
    {
        if (link(QFileInfo(current))) return false;
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
void visitFiles(const QString &root, const std::function<void(const QFileInfo &)> &visit,
                const std::function<bool()> &cancelled = {})
{
    if (!safeRoot(root) || !QFileInfo(root).isDir()) return;
    const auto prefix = QDir(root).canonicalPath() + '/';
    QStringList pending{root};
    while (!pending.isEmpty() && (!cancelled || !cancelled()))
    {
        QDirIterator iterator(pending.takeLast(), QDir::Files | QDir::Dirs | QDir::Hidden |
            QDir::NoDotAndDotDot | QDir::NoSymLinks);
        while (iterator.hasNext() && (!cancelled || !cancelled()))
        {
            iterator.next();
            const auto entry = iterator.fileInfo();
            if (link(entry) || !entry.canonicalFilePath().startsWith(prefix, Qt::CaseInsensitive)) continue;
            if (entry.isDir()) pending.append(entry.absoluteFilePath());
            else if (entry.isFile()) visit(entry);
        }
    }
}
QFileInfoList files(const QString &root)
{
    QFileInfoList result;
    visitFiles(root, [&result](const QFileInfo &entry) { result.append(entry); });
    return result;
}
}
namespace MediaStorage
{
namespace
{
QString mediaKey(const QString &group, const QString &id) { return group + QChar::Null + id; }
QString mediaKey(const QString &group, const QJsonObject &media)
{
    return mediaKey(group, media["id"].toString()) + QChar::Null + media["sha256"].toString() + QChar::Null +
           media["kind"].toString() + QChar::Null + QString::number(media["size"].toInteger(-1));
}
}
QString LocalFiles::existing(const QString &key) const
{
    const auto *entry = entries.object(key);
    if (!entry) return {};
    const QFileInfo info(entry->path);
    if (!info.isFile() || info.size() != entry->size || info.lastModified() != entry->modified)
    {
        entries.remove(key);
        return {};
    }
    return entry->path;
}
QString LocalFiles::find(const QString &group, const QJsonObject &media) const
{
    QMutexLocker lock(&mutex);
    const auto key = mediaKey(group, media);
    const auto *entry = entries.object(key);
    if (!entry || entry->hash != media["sha256"].toString() || entry->size != media["size"].toInteger(-1) ||
        entry->kind != media["kind"].toString()) return {};
    return existing(key);
}
QString LocalFiles::pathForId(const QString &group, const QString &id) const
{
    QMutexLocker lock(&mutex);
    if (id.isEmpty()) return {};
    const auto prefix = mediaKey(group, id) + QChar::Null;
    for (const auto &key : entries.keys())
        if (key.startsWith(prefix))
        {
            const auto path = existing(key);
            if (!path.isEmpty()) return path;
        }
    return {};
}
void LocalFiles::remember(const QString &group, const QJsonObject &media, const QString &path)
{
    const QFileInfo info(path);
    if (group.isEmpty() || media["id"].toString().isEmpty() || media["sha256"].toString().size() != 64 ||
        !info.isFile() || info.size() != media["size"].toInteger(-1)) return;
    QMutexLocker lock(&mutex);
    entries.insert(mediaKey(group, media), new Entry{
        info.absoluteFilePath(), media["sha256"].toString(), media["kind"].toString(), info.size(), info.lastModified()});
}
QString LocalFiles::findPoster(const QString &group, const QJsonObject &media) const
{
    if (media["kind"] != "video") return {};
    QMutexLocker lock(&mutex);
    return existing("poster:" + mediaKey(group, media));
}
void LocalFiles::rememberPoster(const QString &group, const QJsonObject &media, const QString &path)
{
    const QFileInfo info(path);
    if (group.isEmpty() || media["kind"] != "video" || media["id"].toString().isEmpty() ||
        media["sha256"].toString().size() != 64 || !info.isFile() || info.size() <= 0 ||
        info.size() > 48 * 1024 || !isCachePath(info.absoluteFilePath())) return;
    QMutexLocker lock(&mutex);
    entries.insert("poster:" + mediaKey(group, media), new Entry{info.absoluteFilePath(),
        media["sha256"].toString(), "video-poster", info.size(), info.lastModified()});
}
QJsonObject LocalFiles::hydrate(QJsonObject message, const QString &fallbackGroup) const
{
    if (message["status"] == "recalled") { message["media"] = QJsonArray{}; return message; }
    const auto group = message["group"].toString(fallbackGroup);
    auto media = message["media"].toArray();
    for (int i = 0; i < media.size(); ++i)
    {
        auto item = media[i].toObject();
        item["_local"] = find(group, item); // Never trust a path supplied in an HTTP/WS payload.
        item["_poster"] = findPoster(group, item);
        media[i] = item;
    }
    message["media"] = media;
    return message;
}
void LocalFiles::discard(const QStringList &paths)
{
    const QSet<QString> removed(paths.cbegin(), paths.cend());
    QMutexLocker lock(&mutex);
    for (const auto &key : entries.keys())
        if (removed.contains(entries.object(key)->path)) entries.remove(key);
}
void LocalFiles::pin(const void *owner, const QSet<QString> &paths)
{
    QMutexLocker lock(&mutex);
    pins.insert(reinterpret_cast<quintptr>(owner), paths);
}
void LocalFiles::unpin(const void *owner)
{
    QMutexLocker lock(&mutex);
    pins.remove(reinterpret_cast<quintptr>(owner));
}
bool LocalFiles::removeIfUnpinned(const QString &path, bool &pinned)
{
    QMutexLocker lock(&mutex);
    pinned = false;
    for (const auto &paths : pins)
        if (paths.contains(path)) { pinned = true; return false; }
    // The deletion and live pin updates share this lock, closing the check/delete race.
    return QFile::remove(path);
}
QString cacheDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("media");
}
bool isCachePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).startsWith(
        QDir::cleanPath(cacheDirectory()) + '/', Qt::CaseInsensitive);
}
qint64 cacheLimit()
{
    const auto value = QSettings().value("mediaCacheLimit", 512LL * 1024 * 1024).toLongLong();
    return value == 256LL * 1024 * 1024 || value == 1024LL * 1024 * 1024 ||
        value == 2048LL * 1024 * 1024 ? value : 512LL * 1024 * 1024;
}
Usage measure(const QString &downloads, std::function<bool()> cancelled)
{
    Usage result;
    const auto resolved = QFileInfo(downloads).canonicalFilePath();
    visitFiles(resolved.isEmpty() ? downloads : resolved,
        [&result](const auto &file) { result.downloads += file.size(); }, cancelled);
    visitFiles(cacheDirectory(), [&result](const auto &file) { result.cache += file.size(); }, cancelled);
    auto diskPath = QFileInfo(downloads).absoluteFilePath();
    while (!QFileInfo::exists(diskPath))
    {
        const auto parent = QFileInfo(diskPath).absolutePath();
        if (diskPath == parent) break;
        diskPath = parent;
    }
    const QStorageInfo disk(diskPath);
    if (disk.isValid() && disk.isReady()) result.available = disk.bytesAvailable();
    if (!safeRoot(cacheDirectory())) result.error = "缓存目录包含链接，已跳过以保护其他文件。";
    return result;
}
Pruned prune(qint64 limit, const QSet<QString> &protectedPaths, const std::shared_ptr<LocalFiles> &liveFiles)
{
    Pruned result;
    const auto root = cacheDirectory();
    if (!safeRoot(root)) { result.error = "缓存目录包含链接，无法清理。"; return result; }
    auto entries = files(root);
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b)
        { return a.lastModified() < b.lastModified(); });
    for (const auto &file : entries) result.retained += file.size();
    for (const auto &file : entries)
    {
        if (result.retained <= qMax(qint64(0), limit)) break;
        const auto path = file.absoluteFilePath();
        if (protectedPaths.contains(path) || !safeRoot(file.absolutePath())) continue;
        const QFileInfo current(path);
        if (link(current) || !current.canonicalFilePath().startsWith(QDir(root).canonicalPath() + '/', Qt::CaseInsensitive)) continue;
        bool pinned = false;
        const bool removed = liveFiles ? liveFiles->removeIfUnpinned(path, pinned) : QFile::remove(path);
        if (removed) { result.retained -= file.size(); result.removed.append(path); }
        else if (!pinned) result.error = "部分缓存正在使用，已保留。";
    }
    return result;
}
QString saveCached(const QString &source, const QString &downloads, const QString &name, QString &error)
{
    if (!QDir::isAbsolutePath(downloads) || !QFileInfo(source).isFile())
    { error = "无法读取缓存或下载目录无效。"; return {}; }
    const auto folder = QDir(downloads).filePath(QDate::currentDate().toString("yyyy-MM-dd"));
    if (!QDir().mkpath(folder)) { error = "无法创建下载目录。"; return {}; }
    const auto safe = AttachmentStore::safeName(name);
    auto target = QDir(folder).filePath(safe);
    int number = 0;
    while (QFileInfo::exists(target))
    {
        const QFileInfo original(safe);
        target = QDir(folder).filePath(original.completeBaseName() + " (" + QString::number(++number) + ")" +
            (original.suffix().isEmpty() ? "" : "." + original.suffix()));
    }
    if (!QFile::copy(source, target)) { error = "无法保存文件，请检查磁盘空间和目录权限。"; return {}; }
    return target;
}
}
