#include "media_storage.h"
#include "attachment.h"
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
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
Pruned prune(qint64 limit, const QSet<QString> &protectedPaths)
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
        if (QFile::remove(path)) { result.retained -= file.size(); result.removed.append(path); }
        else result.error = "部分缓存正在使用，已保留。";
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
