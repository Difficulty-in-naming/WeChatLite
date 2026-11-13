#pragma once
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
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
// One server/account scope. Only verified downloads or hash-checked legacy files
// enter the bounded index. An empty scope keeps the index in memory only.
class LocalFiles final
{
  public:
    explicit LocalFiles(const QString &scope = {}, const QString &indexDirectory = {});
    QString find(const QString &group, const QJsonObject &media) const;
    QString pathForId(const QString &group, const QString &id) const;
    void remember(const QString &group, const QJsonObject &media, const QString &path);
    QString findPoster(const QString &group, const QJsonObject &media) const;
    void rememberPoster(const QString &group, const QJsonObject &media, const QString &path);
    QJsonObject hydrate(QJsonObject message, const QString &fallbackGroup = {}) const;
    // Worker-thread only: scans configured media roots once for a whole batch,
    // hashing only matching legacy names/sizes. Never trusts payload local paths.
    void recover(const QJsonArray &messages, const QString &downloads, const QString &fallbackGroup = {},
                 std::function<bool()> cancelled = {});
    QString recover(const QString &group, const QJsonObject &media, const QString &downloads,
                    std::function<bool()> cancelled = {});
    void discard(const QStringList &paths);
    void pin(const void *owner, const QSet<QString> &paths);
    void unpin(const void *owner);
    bool removeIfUnpinned(const QString &path, bool &pinned);
  private:
    struct Entry
    {
        QString path, group, id, hash, kind;
        qint64 mediaSize = 0, size = 0;
        QDateTime modified;
        bool poster = false;
    };
    QString existing(const QString &key) const;
    QList<Entry> readIndex() const;
    void persist(const QSet<QString> &discardedPaths = {});
    QString indexFile, scopeHash;
    QDateTime indexModified;
    qint64 indexSize = -1;
    mutable QMutex mutex;
    mutable QCache<QString, Entry> entries{1024};
    QHash<quintptr, QSet<QString>> pins;
};
// Resolves the platform alias of the selected download root only (macOS ~/Downloads is a
// link to another volume). Nested links below the root are still refused by every caller,
// because only this one hop is applied. The deepest existing ancestor is resolved and the
// not-yet-created remainder is appended, so a first-run folder still lands in canonical form.
QString resolveRoot(const QString &path);
QString cacheDirectory();
bool isCachePath(const QString &path);
qint64 cacheLimit();
Usage measure(const QString &downloads, std::function<bool()> cancelled = {});
Pruned prune(qint64 limit, const QSet<QString> &protectedPaths,
             const std::shared_ptr<LocalFiles> &liveFiles = {});
QString saveCached(const QString &source, const QString &downloads, const QString &name, QString &error);
}
