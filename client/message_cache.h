#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <limits>

// A bounded per-conversation snapshot of server messages, so a conversation can
// paint its last rows before the first history round trip finishes.
//
// Nothing here is authoritative: the server stays the source of truth, every
// stored row is a whitelisted subset of the payload, and the first history page
// always wins over the copy on disk. A corrupt, oversized or foreign file reads
// as empty instead of surfacing partial data.
struct MessageCacheLimits
{
    // The model keeps a 300-row window; one screen needs far less, and this is
    // what a 60-row page plus a scroll-up can refresh anyway.
    static constexpr int rows = 120;
    static constexpr qint64 fileBytes = 4 * 1024 * 1024;
    static constexpr int idChars = 512;
    static constexpr int nameChars = 4096;
    static constexpr int textBytes = 100 * 1024;
    static constexpr int quoteBytes = 32 * 1024;
    static constexpr int avatarBytes = 48 * 1024;
    static constexpr int errorChars = 2048;
    static constexpr int recordItems = 500;
    static constexpr int mediaItems = 200;
};

class MessageCache
{
  public:
    // An empty scope keeps everything in memory only, which is what an
    // unconfigured client and the test binaries get.
    explicit MessageCache(const QString &scope = {}, const QString &root = {});
    // Returns true when the scope or directory actually changed, so the caller
    // knows the in-memory rows were dropped.
    bool configure(const QString &scope, const QString &root = {});
    QString scope() const { return scopeHash; }
    bool persistent() const { return !scopeHash.isEmpty() && !rootDirectory.isEmpty(); }
    // Oldest first, already sanitized and ordered by (timestamp, seq).
    QList<QJsonObject> read(const QString &group) const;
    void write(const QString &group, const QList<QJsonObject> &rows);
    void remove(const QString &group);
    // The row subset worth keeping: drops transport-only fields, local paths and
    // anything that could not be rendered after a restart.
    static QJsonObject sanitize(const QJsonObject &message);
    static QList<QJsonObject> sanitize(const QList<QJsonObject> &rows);

  private:
    QByteArray encoded(const QString &group, const QList<QJsonObject> &rows) const;
    QString fileFor(const QString &group) const;
    QList<QJsonObject> loadFile(const QString &group) const;
    void persist(const QString &group, const QList<QJsonObject> &rows) const;
    QString scopeHash;
    QString rootDirectory;
    mutable QHash<QString, QList<QJsonObject>> rows;
};
