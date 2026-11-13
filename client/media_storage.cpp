#include "media_storage.h"
#include "attachment.h"
#include <QDate>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMutexLocker>
#include <QSettings>
#include <QSaveFile>
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
constexpr int indexLimit = 1024;
constexpr qint64 indexByteLimit = 4 * 1024 * 1024;
QString mediaKey(const QString &group, const QString &id) { return group + QChar::Null + id; }
QString mediaKey(const QString &group, const QJsonObject &media, bool poster = false)
{
    return QString(poster ? "poster" : "original") + QChar::Null + mediaKey(group, media["id"].toString()) +
           QChar::Null + media["sha256"].toString() + QChar::Null +
           media["kind"].toString() + QChar::Null + QString::number(media["size"].toInteger(-1));
}
bool validDescriptor(const QString &group, const QJsonObject &media)
{
    const auto id = media["id"].toString(), hash = media["sha256"].toString();
    return !group.isEmpty() && group.size() <= 512 && !group.contains(QChar::Null) &&
        !id.isEmpty() && id.size() <= 512 && !id.contains(QChar::Null) && hash.size() == 64 &&
        std::all_of(hash.cbegin(), hash.cend(), [](QChar ch)
        { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }) &&
        QStringList{"image", "gif", "video", "voice", "file"}.contains(media["kind"].toString()) &&
        media["size"].isDouble() && media["size"].toInteger(-1) > 0;
}
bool sameFile(const QFileInfo &info, qint64 size, const QDateTime &modified)
{
    return info.isFile() && info.size() == size && info.lastModified() == modified &&
        safeRoot(info.absoluteFilePath());
}
bool legacyName(const QString &expected, const QString &actual)
{
    if (actual == expected) return true;
    const QFileInfo original(expected), candidate(actual);
    const auto prefix = original.completeBaseName() + " (";
    const auto base = candidate.completeBaseName();
    if (candidate.suffix() != original.suffix() || !base.startsWith(prefix) || !base.endsWith(')')) return false;
    const auto number = base.mid(prefix.size(), base.size() - prefix.size() - 1);
    return !number.isEmpty() && number[0] != '0' && std::all_of(number.cbegin(), number.cend(),
        [](QChar ch) { return ch >= '0' && ch <= '9'; });
}
void visitLegacyFiles(const QString &root, const std::function<void(const QFileInfo &)> &visit,
                      const std::function<bool()> &cancelled)
{
    if (!safeRoot(root) || !QFileInfo(root).isDir()) return;
    const auto prefix = QDir(root).canonicalPath() + '/';
    QStringList directories{root};
    QDirIterator dates(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    while (dates.hasNext() && (!cancelled || !cancelled()))
    {
        dates.next();
        const auto info = dates.fileInfo();
        const auto date = QDate::fromString(info.fileName(), "yyyy-MM-dd");
        if (!link(info) && date.isValid() && date.toString("yyyy-MM-dd") == info.fileName() &&
            info.canonicalFilePath().startsWith(prefix, Qt::CaseInsensitive)) directories.append(info.absoluteFilePath());
    }
    for (const auto &directory : directories)
    {
        if (cancelled && cancelled()) return;
        QDirIterator files(directory, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        while (files.hasNext() && (!cancelled || !cancelled()))
        {
            files.next();
            const auto info = files.fileInfo();
            if (!link(info) && info.canonicalFilePath().startsWith(prefix, Qt::CaseInsensitive)) visit(info);
        }
    }
}
QByteArray checkedHash(const QFileInfo &before, const std::function<bool()> &cancelled)
{
    if (!sameFile(before, before.size(), before.lastModified()) || (cancelled && cancelled())) return {};
    QFile file(before.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly) || file.size() != before.size()) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (!file.atEnd())
    {
        if (cancelled && cancelled()) return {};
        const auto chunk = file.read(64 * 1024);
        if (chunk.isEmpty()) return {};
        total += chunk.size();
        if (total > before.size()) return {};
        hash.addData(chunk);
    }
    if (total != before.size() || !sameFile(QFileInfo(before.absoluteFilePath()), before.size(), before.lastModified()) ||
        (cancelled && cancelled())) return {};
    return hash.result().toHex();
}
}
LocalFiles::LocalFiles(const QString &scope, const QString &indexDirectory)
{
    if (scope.isEmpty()) return;
    scopeHash = QString::fromLatin1(QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256).toHex());
    const auto directory = indexDirectory.isEmpty() ?
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("media-index") :
        indexDirectory;
    if (!QDir::isAbsolutePath(directory) || !safeRoot(directory)) { scopeHash.clear(); return; }
    indexFile = QDir(directory).filePath(scopeHash + ".json");
    for (const auto &entry : readIndex())
    {
        const QJsonObject media{{"id", entry.id}, {"sha256", entry.hash}, {"kind", entry.kind}, {"size", entry.mediaSize}};
        entries.insert(mediaKey(entry.group, media, entry.poster), new Entry(entry));
    }
    const QFileInfo info(indexFile);
    indexModified = info.lastModified();
    indexSize = info.exists() ? info.size() : -1;
}
QList<LocalFiles::Entry> LocalFiles::readIndex() const
{
    QList<Entry> result;
    const QFileInfo info(indexFile);
    if (indexFile.isEmpty() || !safeRoot(indexFile) || !info.isFile() || info.size() <= 0 ||
        info.size() > indexByteLimit) return result;
    QFile file(indexFile);
    if (!file.open(QIODevice::ReadOnly)) return result;
    const auto bytes = file.read(indexByteLimit + 1);
    if (bytes.size() != info.size()) return result;
    const auto root = QJsonDocument::fromJson(bytes).object();
    const auto stored = root["entries"].toArray();
    if (root["version"] != 1 || root["scope"] != scopeHash || !root["entries"].isArray() ||
        stored.size() > indexLimit) return result;
    for (const auto &value : stored)
    {
        const auto object = value.toObject();
        const auto group = object["group"].toString();
        const auto path = object["path"].toString();
        const auto size = object["file_size"].toInteger(-1);
        const auto modified = QDateTime::fromMSecsSinceEpoch(object["modified"].toInteger(0));
        const auto poster = object["poster"].toBool();
        if (!validDescriptor(group, object) || !QDir::isAbsolutePath(path) || path.size() > 4096 ||
            size <= 0 || !modified.isValid() || object["modified"].toInteger(0) <= 0 ||
            (poster ? (object["kind"] != "video" || size > 48 * 1024 || !isCachePath(path)) :
                      size != object["size"].toInteger(-1))) continue;
        result.append({QDir::cleanPath(path), group, object["id"].toString(), object["sha256"].toString(),
            object["kind"].toString(), object["size"].toInteger(), size, modified, poster});
    }
    return result;
}
void LocalFiles::persist(const QSet<QString> &discardedPaths)
{
    if (indexFile.isEmpty() || !safeRoot(indexFile) || !safeRoot(indexFile + ".lock")) return;
    const auto directory = QFileInfo(indexFile).absolutePath();
    if (!QDir().mkpath(directory) || !safeRoot(directory)) return;
    QLockFile lock(indexFile + ".lock");
    if (!lock.tryLock(0)) return;
    // Another process/window may have downloaded files since this instance was
    // created. Merge only on an actual on-disk change, within the same scope.
    const QFileInfo disk(indexFile);
    if ((disk.exists() ? disk.size() : -1) != indexSize || disk.lastModified() != indexModified)
        for (const auto &entry : readIndex())
        {
            if (entries.size() >= indexLimit) break;
            const QJsonObject media{{"id", entry.id}, {"sha256", entry.hash}, {"kind", entry.kind}, {"size", entry.mediaSize}};
            const auto key = mediaKey(entry.group, media, entry.poster);
            if (!entries.contains(key) && !discardedPaths.contains(entry.path) &&
                sameFile(QFileInfo(entry.path), entry.size, entry.modified)) entries.insert(key, new Entry(entry));
        }
    QJsonArray stored;
    qint64 bytes = 256;
    for (const auto &key : entries.keys())
    {
        const auto &entry = *entries.object(key);
        if (discardedPaths.contains(entry.path)) { entries.remove(key); continue; }
        const QJsonObject object{{"path", entry.path}, {"group", entry.group}, {"id", entry.id},
            {"sha256", entry.hash}, {"kind", entry.kind}, {"size", entry.mediaSize}, {"file_size", entry.size},
            {"modified", entry.modified.toMSecsSinceEpoch()}, {"poster", entry.poster}};
        const auto cost = QJsonDocument(object).toJson(QJsonDocument::Compact).size() + 1;
        if (bytes + cost > indexByteLimit) continue;
        bytes += cost;
        stored.append(object);
    }
    const auto data = QJsonDocument(QJsonObject{{"version", 1}, {"scope", scopeHash}, {"entries", stored}})
        .toJson(QJsonDocument::Compact);
    QSaveFile file(indexFile);
    file.setDirectWriteFallback(false);
    if (!safeRoot(indexFile) || !file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) return;
    const QFileInfo updated(indexFile);
    indexModified = updated.lastModified();
    indexSize = updated.size();
}
QString LocalFiles::existing(const QString &key) const
{
    const auto *entry = entries.object(key);
    if (!entry) return {};
    const QFileInfo info(entry->path);
    if (!sameFile(info, entry->size, entry->modified))
    {
        entries.remove(key);
        return {};
    }
    return entry->path;
}
QString LocalFiles::find(const QString &group, const QJsonObject &media) const
{
    if (!validDescriptor(group, media)) return {};
    QMutexLocker lock(&mutex);
    const auto key = mediaKey(group, media);
    const auto *entry = entries.object(key);
    if (!entry || entry->hash != media["sha256"].toString() || entry->mediaSize != media["size"].toInteger(-1) ||
        entry->kind != media["kind"].toString() || entry->poster) return {};
    return existing(key);
}
QString LocalFiles::pathForId(const QString &group, const QString &id) const
{
    QMutexLocker lock(&mutex);
    if (group.isEmpty() || group.contains(QChar::Null) || id.isEmpty() || id.contains(QChar::Null)) return {};
    const auto prefix = QStringLiteral("original") + QChar::Null + mediaKey(group, id) + QChar::Null;
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
    if (!validDescriptor(group, media) || !info.isFile() || info.size() != media["size"].toInteger(-1) ||
        !safeRoot(info.absoluteFilePath()) || info.absoluteFilePath().size() > 4096) return;
    QMutexLocker lock(&mutex);
    entries.insert(mediaKey(group, media), new Entry{info.absoluteFilePath(), group, media["id"].toString(),
        media["sha256"].toString(), media["kind"].toString(), info.size(), info.size(), info.lastModified(), false});
    persist();
}
QString LocalFiles::findPoster(const QString &group, const QJsonObject &media) const
{
    if (media["kind"] != "video" || !validDescriptor(group, media)) return {};
    QMutexLocker lock(&mutex);
    return existing(mediaKey(group, media, true));
}
void LocalFiles::rememberPoster(const QString &group, const QJsonObject &media, const QString &path)
{
    const QFileInfo info(path);
    if (!validDescriptor(group, media) || media["kind"] != "video" || !info.isFile() || info.size() <= 0 ||
        info.size() > 48 * 1024 || info.absoluteFilePath().size() > 4096 ||
        !isCachePath(info.absoluteFilePath()) || !safeRoot(info.absoluteFilePath())) return;
    QMutexLocker lock(&mutex);
    entries.insert(mediaKey(group, media, true), new Entry{info.absoluteFilePath(), group, media["id"].toString(),
        media["sha256"].toString(), media["kind"].toString(), media["size"].toInteger(), info.size(), info.lastModified(), true});
    persist();
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
void LocalFiles::recover(const QJsonArray &messages, const QString &downloads, const QString &fallbackGroup,
                         std::function<bool()> cancelled)
{
    struct Missing { QString group, name; QJsonObject media; };
    QList<Missing> missing;
    QHash<qint64, QList<int>> sizes;
    QSet<QString> seen;
    for (const auto &value : messages)
    {
        if (cancelled && cancelled()) return;
        const auto message = value.toObject();
        if (message["status"] == "recalled") continue;
        const auto group = message["group"].toString(fallbackGroup);
        for (const auto &mediaValue : message["media"].toArray())
        {
            const auto media = mediaValue.toObject();
            if (!validDescriptor(group, media) || media["name"].toString().isEmpty() ||
                !find(group, media).isEmpty()) continue;
            const auto key = mediaKey(group, media);
            if (seen.contains(key) || missing.size() >= indexLimit) continue;
            seen.insert(key);
            sizes[media["size"].toInteger()].append(missing.size());
            missing.append({group, AttachmentStore::safeName(media["name"].toString()), media});
        }
    }
    if (missing.isEmpty()) return;
    QSet<int> recovered;
    const auto stopped = [&] { return recovered.size() == missing.size() || (cancelled && cancelled()); };
    const auto candidate = [&](const QFileInfo &file)
    {
        QList<int> matches;
        for (const auto i : sizes.value(file.size()))
            if (!recovered.contains(i) && legacyName(missing[i].name, file.fileName())) matches.append(i);
        if (matches.isEmpty() || stopped()) return;
        const auto hash = checkedHash(file, stopped);
        if (hash.isEmpty()) return;
        for (const auto i : matches)
        {
            if (missing[i].media["sha256"].toString().compare(QString::fromLatin1(hash), Qt::CaseInsensitive) != 0) continue;
            if (!sameFile(QFileInfo(file.absoluteFilePath()), file.size(), file.lastModified()) ||
                (cancelled && cancelled())) return;
            remember(missing[i].group, missing[i].media, file.absoluteFilePath());
            if (!find(missing[i].group, missing[i].media).isEmpty()) recovered.insert(i);
        }
    };
    // A selected download root can resolve a platform alias such as macOS /var;
    // nested symlinks are still excluded and only the resolved path is indexed.
    const auto resolved = QFileInfo(downloads).canonicalFilePath();
    const QStringList roots{cacheDirectory(), resolved.isEmpty() ? downloads : resolved};
    QSet<QString> visited;
    for (const auto &root : roots)
    {
        const auto clean = QDir::cleanPath(root);
        if (stopped()) break;
        if (root.isEmpty() || visited.contains(clean)) continue;
        visited.insert(clean);
        visitLegacyFiles(root, candidate, stopped);
    }
}
QString LocalFiles::recover(const QString &group, const QJsonObject &media, const QString &downloads,
                            std::function<bool()> cancelled)
{
    if (const auto path = find(group, media); !path.isEmpty()) return path;
    recover(QJsonArray{QJsonObject{{"group", group}, {"media", QJsonArray{media}}}}, downloads, {}, std::move(cancelled));
    return find(group, media);
}
void LocalFiles::discard(const QStringList &paths)
{
    const QSet<QString> removed(paths.cbegin(), paths.cend());
    QMutexLocker lock(&mutex);
    for (const auto &key : entries.keys())
        if (removed.contains(entries.object(key)->path)) entries.remove(key);
    persist(removed);
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
QString resolveRoot(const QString &path)
{
    if (!QDir::isAbsolutePath(path)) return path;
    const auto clean = QDir::cleanPath(path);
    QString remainder;
    auto current = clean;
    while (!current.isEmpty())
    {
        const auto resolved = QFileInfo(current).canonicalFilePath();
        if (!resolved.isEmpty()) return QDir::cleanPath(resolved + remainder);
        const auto name = QFileInfo(current).fileName();
        if (name.isEmpty()) break;
        remainder.prepend('/' + name);
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return clean;
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
