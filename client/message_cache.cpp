#include "message_cache.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <algorithm>

namespace
{
bool isLink(const QFileInfo &info) { return info.isSymLink() || info.isJunction(); }
// The cache is a display accelerator, never a trusted input: every path component
// is checked so a symlinked cache directory cannot redirect a write elsewhere.
bool safeRoot(const QString &path)
{
    if (!QDir::isAbsolutePath(path)) return false;
    auto current = QDir::cleanPath(path);
    while (!current.isEmpty())
    {
        if (isLink(QFileInfo(current))) return false;
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
bool hasNull(const QString &value) { return value.contains(QChar::Null); }
bool bounded(const QString &value, int chars)
{
    return !hasNull(value) && value.size() <= chars;
}
bool body(const QString &value)
{
    return !hasNull(value) && value.toUtf8().size() <= MessageCacheLimits::textBytes;
}
bool hex(const QString &value)
{
    return std::all_of(value.cbegin(), value.cend(), [](QChar ch)
        { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); });
}
// Mirrors MediaStorage's descriptor rules, so a restored row only advertises
// attachments the local store can resolve after a restart. A legacy descriptor
// without a hash still renders; it simply cannot be opened until the first page
// replaces it, which happens right after the restore.
bool descriptor(const QJsonObject &media)
{
    const auto id = media["id"].toString(), hash = media["sha256"].toString();
    return !id.isEmpty() && id.size() <= MessageCacheLimits::idChars && !hasNull(id) &&
        bounded(media["name"].toString(), MessageCacheLimits::nameChars) &&
        QStringList{"image", "gif", "video", "voice", "file"}.contains(media["kind"].toString()) &&
        media["size"].isDouble() && media["size"].toInteger(-1) > 0 &&
        (hash.isEmpty() || (hash.size() == 64 && hex(hash)));
}
QJsonObject mediaDescriptor(const QJsonObject &media)
{
    if (!descriptor(media)) return {};
    QJsonObject result{{"id", media["id"].toString()}, {"name", media["name"].toString()},
        {"kind", media["kind"].toString()}, {"size", media["size"].toDouble()}};
    if (!media["sha256"].toString().isEmpty()) result["sha256"] = media["sha256"].toString();
    if (media["duration"].toInt() > 0) result["duration"] = media["duration"].toInt();
    return result;
}
QJsonObject quote(const QJsonObject &value)
{
    const auto id = value["id"].toString();
    if (id.isEmpty() || id.size() > MessageCacheLimits::idChars || hasNull(id)) return {};
    const auto text = value["text"].toString();
    if (text.toUtf8().size() > MessageCacheLimits::quoteBytes ||
        !bounded(value["sender"].toString(), MessageCacheLimits::idChars)) return {};
    return QJsonObject{{"id", id}, {"sender", value["sender"].toString()}, {"text", text},
        {"unavailable", value["unavailable"].toBool()}};
}
QJsonArray mentions(const QJsonArray &values)
{
    QJsonArray result;
    for (const auto &entry : values)
    {
        if (result.size() >= 100) break;
        const auto value = entry.toObject();
        const auto id = value["id"].toString();
        if (id.isEmpty() || id.size() > MessageCacheLimits::idChars || hasNull(id) ||
            !bounded(value["name"].toString(), MessageCacheLimits::idChars)) continue;
        result.append(QJsonObject{{"id", id}, {"name", value["name"].toString()}});
    }
    return result;
}
QJsonObject record(const QJsonObject &value)
{
    const auto title = value["title"].toString();
    if (!bounded(title, MessageCacheLimits::idChars)) return {};
    QJsonArray items;
    for (const auto &entry : value["items"].toArray())
    {
        if (items.size() >= MessageCacheLimits::recordItems) break;
        const auto item = entry.toObject();
        const auto id = item["id"].toString();
        const auto text = item["text"].toString();
        if (id.isEmpty() || id.size() > MessageCacheLimits::idChars || hasNull(id) ||
            !bounded(item["sender"].toString(), MessageCacheLimits::idChars) || !body(text)) continue;
        // Items address the record's own media list by index; keep only valid ones.
        QJsonArray references;
        for (const auto &index : item["media"].toArray())
            if (index.toInt(-1) >= 0) references.append(index.toInt());
        items.append(QJsonObject{{"id", id}, {"sender", item["sender"].toString()}, {"text", text},
            {"timestamp", item["timestamp"].toInteger()}, {"media", references}});
    }
    return QJsonObject{{"title", title}, {"items", items}, {"pending", value["pending"].toBool()}};
}
bool messageOrder(const QJsonObject &left, const QJsonObject &right)
{
    const auto first = left["timestamp"].toInteger(), second = right["timestamp"].toInteger();
    return first != second ? first < second : left["seq"].toInteger() < right["seq"].toInteger();
}
}
MessageCache::MessageCache(const QString &scope, const QString &root)
{
    configure(scope, root);
}
bool MessageCache::configure(const QString &scope, const QString &root)
{
    const auto nextHash = scope.isEmpty() ? QString() :
        QString::fromLatin1(QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256).toHex());
    // Keep the message index outside <CacheLocation>/media: MediaStorage::prune
    // deletes every unpinned file under that root and would silently eat it.
    auto nextRoot = root;
    if (nextRoot.isEmpty() && !nextHash.isEmpty())
        nextRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (!nextHash.isEmpty() && !nextRoot.isEmpty())
        nextRoot = QDir(nextRoot).filePath("message-index/" + nextHash.mid(0, 32));
    if (nextHash.isEmpty() || nextRoot.isEmpty() || !QDir::isAbsolutePath(nextRoot) || !safeRoot(nextRoot))
        nextRoot.clear();
    if (nextHash == scopeHash && nextRoot == rootDirectory) return false;
    scopeHash = nextHash;
    rootDirectory = nextRoot;
    rows.clear();
    return true;
}
QString MessageCache::fileFor(const QString &group) const
{
    auto name = group;
    for (auto &ch : name)
        if (!ch.isLetterOrNumber() && ch != '-' && ch != '_' && ch != '.') ch = '_';
    name = name.left(64);
    if (name.isEmpty()) name = "group";
    // Sanitizing maps different ids onto the same name ("a/b" and "a_b"); the digest
    // keeps each conversation in its own file instead of overwriting a neighbour.
    const auto digest = QString::fromLatin1(
        QCryptographicHash::hash(group.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    return QDir(QDir(rootDirectory).filePath("messages")).filePath(name + '-' + digest + ".json");
}
QList<QJsonObject> MessageCache::read(const QString &group) const
{
    if (group.isEmpty() || !bounded(group, MessageCacheLimits::idChars)) return {};
    if (rows.contains(group)) return rows.value(group);
    auto loaded = persistent() ? loadFile(group) : QList<QJsonObject>{};
    rows.insert(group, loaded);
    return loaded;
}
QList<QJsonObject> MessageCache::loadFile(const QString &group) const
{
    QList<QJsonObject> result;
    const auto path = fileFor(group);
    const QFileInfo info(path);
    if (!safeRoot(path) || !info.isFile() || info.size() <= 0 || info.size() > MessageCacheLimits::fileBytes)
        return result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return result;
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return result;
    const auto stored = document.object();
    // A moved or copied index must never leak one account's history into another.
    if (stored["version"].toInt() != 1 || stored["scope"].toString() != scopeHash ||
        stored["group"].toString() != group) return result;
    for (const auto &entry : stored["messages"].toArray())
    {
        if (result.size() >= MessageCacheLimits::rows) break;
        auto message = sanitize(entry.toObject());
        if (message.isEmpty() || message["group"].toString() != group) continue;
        result.append(message);
    }
    std::stable_sort(result.begin(), result.end(), messageOrder);
    return result;
}
void MessageCache::write(const QString &group, const QList<QJsonObject> &values)
{
    if (group.isEmpty() || !bounded(group, MessageCacheLimits::idChars)) return;
    QList<QJsonObject> own;
    own.reserve(values.size());
    // The file and the in-memory key are per group: a mixed list would put one
    // conversation's rows under another's name.
    for (const auto &value : values)
        if (value["group"].toString() == group)
            own.append(value);
    // One row per id: the model re-sends a row on every status change, and a
    // duplicated id would paint twice after a restore. The later copy wins.
    QList<QJsonObject> unique;
    QHash<QString, int> position;
    for (const auto &value : own)
    {
        const auto id = value["id"].toString();
        if (id.isEmpty()) continue;
        const auto found = position.constFind(id);
        if (found == position.cend())
        {
            position.insert(id, int(unique.size()));
            unique.append(value);
        }
        else
            unique[*found] = value;
    }
    auto stored = sanitize(unique);
    // Always keep the newest window; a long session can hold 300 rows.
    if (stored.size() > MessageCacheLimits::rows)
        stored = stored.mid(stored.size() - MessageCacheLimits::rows);
    if (!persistent())
    {
        rows.insert(group, stored);
        return;
    }
    if (stored.isEmpty())
    {
        rows.insert(group, stored);
        QFile::remove(fileFor(group));
        return;
    }
    // Avatars and long text can push a full window past the file cap. Drop the oldest
    // quarter and try again rather than writing nothing: a shallower snapshot still
    // paints the first screen, which is the whole point of the file.
    while (stored.size() > 1 && encoded(group, stored).size() > MessageCacheLimits::fileBytes)
        stored = stored.mid(qMax(1, stored.size() / 4));
    rows.insert(group, stored);
    persist(group, stored);
}
QByteArray MessageCache::encoded(const QString &group, const QList<QJsonObject> &values) const
{
    QJsonArray messages;
    for (const auto &row : values) messages.append(row);
    return QJsonDocument(QJsonObject{{"version", 1}, {"scope", scopeHash}, {"group", group},
        {"messages", messages}}).toJson(QJsonDocument::Compact);
}
void MessageCache::persist(const QString &group, const QList<QJsonObject> &values) const
{
    const auto data = encoded(group, values);
    // The row cap bounds normal files; this guard only matters for very long text.
    if (data.size() > MessageCacheLimits::fileBytes) return;
    const auto path = fileFor(group);
    if (!safeRoot(path) || !QDir().mkpath(QFileInfo(path).absolutePath())) return;
    // Skip a rewrite when the bytes are unchanged: reconnects must not churn the disk.
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == data) return;
    // QSaveFile commits through rename, so a crash cannot leave a truncated file
    // that would later read as a shorter history.
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) return;
}
void MessageCache::remove(const QString &group)
{
    if (group.isEmpty() || !bounded(group, MessageCacheLimits::idChars)) return;
    rows.remove(group);
    if (persistent()) QFile::remove(fileFor(group));
}
QJsonObject MessageCache::sanitize(const QJsonObject &message)
{
    const auto group = message["group"].toString();
    const auto id = message["id"].toString();
    const auto status = message["status"].toString();
    if (group.isEmpty() || !bounded(group, MessageCacheLimits::idChars) ||
        id.isEmpty() || !bounded(id, MessageCacheLimits::idChars) ||
        !bounded(message["sender"].toString(), MessageCacheLimits::idChars) ||
        // Only rows the server has itself acknowledged: a queued/sending/failed/uncertain
        // row is a local transient that a page will never confirm, so replaying it after a
        // restart would show a message that was possibly never delivered.
        !QStringList{"received", "sent", "recalled"}.contains(status) ||
        !body(message["text"].toString())) return {};
    QJsonObject result{{"id", id}, {"group", group}, {"sender", message["sender"].toString()},
        {"text", message["text"].toString()}, {"timestamp", message["timestamp"].toInteger()},
        {"status", status}, {"outgoing", message["outgoing"].toBool()}};
    const auto senderId = message["sender_id"].toString();
    if (!senderId.isEmpty() && bounded(senderId, MessageCacheLimits::idChars)) result["sender_id"] = senderId;
    const auto avatar = message["avatar"].toString();
    // Avatars are small base64 JPEGs; anything much bigger is not worth a file.
    if (!avatar.isEmpty() && avatar.size() <= MessageCacheLimits::avatarBytes) result["avatar"] = avatar;
    const auto seq = message["seq"].toInteger();
    if (seq > 0) result["seq"] = seq;
    const auto source = message["source_id"].toString();
    // Recall eligibility is derived from this field, so it has to survive a restart.
    if (!source.isEmpty() && bounded(source, MessageCacheLimits::idChars)) result["source_id"] = source;
    const auto recall = message["recall_status"].toString();
    if (QStringList{"pending", "failed", "uncertain"}.contains(recall)) result["recall_status"] = recall;
    const auto recallError = message["recall_error"].toString();
    if (!recallError.isEmpty() && bounded(recallError, MessageCacheLimits::errorChars))
        result["recall_error"] = recallError;
    const auto error = message["error"].toString();
    if (!error.isEmpty() && bounded(error, MessageCacheLimits::errorChars)) result["error"] = error;
    // A recalled row renders from its status alone: keeping the body would let a
    // stale snapshot flash content the server already removed.
    if (status == "recalled")
    {
        result["text"] = QString();
        return result;
    }
    const auto quoteValue = quote(message["quote"].toObject());
    if (!quoteValue.isEmpty()) result["quote"] = quoteValue;
    const auto mentionValues = mentions(message["mentions"].toArray());
    if (!mentionValues.isEmpty()) result["mentions"] = mentionValues;
    if (message["record"].isObject())
    {
        const auto recordValue = record(message["record"].toObject());
        if (!recordValue["title"].toString().isEmpty() || !recordValue["items"].toArray().isEmpty())
            result["record"] = recordValue;
    }
    QJsonArray media;
    for (const auto &entry : message["media"].toArray())
    {
        if (media.size() >= MessageCacheLimits::mediaItems) break;
        const auto value = mediaDescriptor(entry.toObject());
        // Rendering a row without an attachment it had would look like data loss.
        if (value.isEmpty()) return {};
        media.append(value);
    }
    if (!media.isEmpty()) result["media"] = media;
    return result;
}
QList<QJsonObject> MessageCache::sanitize(const QList<QJsonObject> &values)
{
    QList<QJsonObject> result;
    for (const auto &value : values)
    {
        auto message = sanitize(value);
        if (!message.isEmpty()) result.append(message);
    }
    std::stable_sort(result.begin(), result.end(), messageOrder);
    return result;
}
