#pragma once
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <functional>
#include <memory>

namespace MediaStorage
{
struct Usage { qint64 downloads = 0, cache = 0, available = -1; QString error; };
struct Pruned { QStringList removed; qint64 retained = 0; QString error; };
// One server/account scope. Only verified downloads enter this bounded in-process index.
class LocalFiles final
{
  public:
    QString find(const QString &group, const QJsonObject &media) const;
    QString pathForId(const QString &group, const QString &id) const;
    void remember(const QString &group, const QJsonObject &media, const QString &path);
    QJsonObject hydrate(QJsonObject message, const QString &fallbackGroup = {}) const;
    void discard(const QStringList &paths);
    void pin(const void *owner, const QSet<QString> &paths);
    void unpin(const void *owner);
    bool removeIfUnpinned(const QString &path, bool &pinned);
  private:
    struct Entry { QString path, hash, kind; qint64 size; QDateTime modified; };
    QString existing(const QString &key) const;
    mutable QMutex mutex;
    mutable QCache<QString, Entry> entries{1024};
    QHash<quintptr, QSet<QString>> pins;
};
QString cacheDirectory();
bool isCachePath(const QString &path);
qint64 cacheLimit();
Usage measure(const QString &downloads, std::function<bool()> cancelled = {});
Pruned prune(qint64 limit, const QSet<QString> &protectedPaths,
             const std::shared_ptr<LocalFiles> &liveFiles = {});
QString saveCached(const QString &source, const QString &downloads, const QString &name, QString &error);
}
