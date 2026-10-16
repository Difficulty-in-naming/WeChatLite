#pragma once
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

// Private client drafts only: recalled bodies never go back into server history or events.
class RecallDrafts
{
  public:
    void configure(const QString &identity)
    {
        scope = QString::fromLatin1(
            QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
        QSettings settings;
        prune(settings, QDateTime::currentMSecsSinceEpoch());
    }
    QJsonObject get(const QString &id, const QString &group,
                    qint64 now = QDateTime::currentMSecsSinceEpoch()) const
    {
        const auto record = QJsonDocument::fromJson(QSettings().value(key(id)).toByteArray()).object();
        const auto age = now - record["created"].toInteger();
        if (record["group"].toString() != group || record["id"].toString() != id || age < 0 ||
            age >= 86400000)
            return {};
        auto draft = record["draft"].toObject();
        QJsonArray valid;
        int missing = draft["_omittedAttachments"].toInt();
        for (const auto &entry : draft["attachments"].toArray())
        {
            const auto item = entry.toObject();
            const QFileInfo info(item["path"].toString());
            if (info.isFile() && info.isReadable() && info.size() == item["size"].toInteger() &&
                info.lastModified().toMSecsSinceEpoch() == item["modified"].toInteger())
                valid.append(item);
            else
                ++missing;
        }
        draft["attachments"] = valid;
        draft["_missingAttachments"] = missing;
        return draft["text"].toString().isEmpty() && valid.isEmpty() && !missing ? QJsonObject{} : draft;
    }
    void remember(const QString &id, const QString &group, QJsonObject draft,
                  qint64 now = QDateTime::currentMSecsSinceEpoch())
    {
        if (id.isEmpty() || group.isEmpty() || !get(id, group, now).isEmpty())
            return;
        QSettings settings;
        prune(settings, now, 63);
        qint64 available = qMax(0LL, 128LL * 1024 * 1024 - cacheBytes());
        QJsonArray saved;
        int attachmentIndex = 0, omitted = 0;
        for (const auto &entry : draft["attachments"].toArray())
        {
            auto item = entry.toObject();
            QFileInfo info(item["path"].toString());
            if (!info.isFile() || !info.isReadable() || info.size() != item["size"].toInteger() ||
                info.lastModified().toMSecsSinceEpoch() != item["modified"].toInteger())
            {
                ++omitted;
                continue;
            }
            if (item["temporary"].toBool())
            {
                if (info.size() > available)
                {
                    ++omitted;
                    continue;
                }
                const auto folder = directory(id);
                if (!QDir().mkpath(folder))
                {
                    ++omitted;
                    continue;
                }
                const QFileInfo folderInfo(folder), rootInfo(folderInfo.dir().absolutePath());
                if (folderInfo.isSymLink() || rootInfo.isSymLink() ||
                    rootInfo.canonicalFilePath().isEmpty() ||
                    !folderInfo.canonicalFilePath().startsWith(rootInfo.canonicalFilePath() + '/'))
                {
                    ++omitted;
                    continue;
                }
                auto suffix = info.suffix().left(8);
                bool validSuffix = true;
                for (const auto ch : suffix)
                    if (!ch.isLetterOrNumber())
                        validSuffix = false;
                if (!validSuffix)
                    suffix.clear();
                const auto path = folder + '/' + QString::number(attachmentIndex++) +
                                  (suffix.isEmpty() ? "" : '.' + suffix);
                if (info.absoluteFilePath() != path)
                {
                    if ((QFile::exists(path) && !QFile::remove(path)) ||
                        !QFile::copy(info.absoluteFilePath(), path))
                    {
                        ++omitted;
                        continue;
                    }
                }
                const auto expectedSize = info.size();
                info.setFile(path);
                if (info.size() != expectedSize || info.size() > available)
                {
                    QFile::remove(path);
                    ++omitted;
                    continue;
                }
                available -= info.size();
                item["path"] = path;
                item["modified"] = info.lastModified().toMSecsSinceEpoch();
            }
            item["uploaded"] = "";
            saved.append(item);
        }
        draft["attachments"] = saved;
        draft["_omittedAttachments"] = omitted;
        settings.setValue(
            key(id),
            QJsonDocument(QJsonObject{{"id", id}, {"group", group}, {"created", now}, {"draft", draft}})
                .toJson(QJsonDocument::Compact));
        auto entries = index(settings);
        for (int i = entries.size() - 1; i >= 0; --i)
            if (entries[i].toObject()["id"] == id && entries[i].toObject()["scope"] == scope)
                entries.removeAt(i);
        entries.append(QJsonObject{{"id", id}, {"scope", scope}, {"created", now}});
        saveIndex(settings, entries);
    }
    // Move backup files into the ordinary input draft directory before releasing this cache entry.
    bool release(const QString &id, QJsonObject &draft) const
    {
        const auto folder = directory(id);
        bool ownsFiles = false;
        for (const auto &entry : draft["attachments"].toArray())
            ownsFiles |= entry.toObject()["temporary"].toBool() &&
                         entry.toObject()["path"].toString().startsWith(folder + '/');
        if (!ownsFiles)
            return true;
        const auto destinationRoot =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/drafts";
        const auto destination =
            destinationRoot + "/recall-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!safeFolder(folder) || !QDir().mkpath(destinationRoot) ||
            QFileInfo(destinationRoot).isSymLink() || !QDir().rename(folder, destination))
            return false;
        auto media = draft["attachments"].toArray();
        for (int i = 0; i < media.size(); ++i)
        {
            auto item = media[i].toObject();
            const auto path = item["path"].toString();
            if (item["temporary"].toBool() && path.startsWith(folder + '/'))
                item["path"] = destination + path.mid(folder.size());
            media[i] = item;
        }
        draft["attachments"] = media;
        return true;
    }
    void consume(const QString &id)
    {
        QSettings settings;
        if (safeFolder(directory(id)))
            QDir(directory(id)).removeRecursively();
        settings.remove(key(id));
        auto entries = index(settings);
        for (int i = entries.size() - 1; i >= 0; --i)
            if (entries[i].toObject()["id"] == id && entries[i].toObject()["scope"] == scope)
                entries.removeAt(i);
        saveIndex(settings, entries);
    }

  private:
    QString prefix() const { return "recallDrafts/" + scope + '/'; }
    static QString hash(const QString &value)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
    }
    QString key(const QString &id) const { return prefix() + "messages/" + hash(id); }
    static QString cacheRoot()
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recall-drafts";
    }
    QString directory(const QString &id) const { return cacheRoot() + '/' + scope + '/' + hash(id); }
    static QJsonArray index(QSettings &settings)
    {
        return QJsonDocument::fromJson(settings.value("recallDrafts/index").toByteArray()).array();
    }
    static void saveIndex(QSettings &settings, const QJsonArray &entries)
    {
        settings.setValue("recallDrafts/index", QJsonDocument(entries).toJson(QJsonDocument::Compact));
    }
    static bool isHash(const QString &value)
    {
        return value.size() == 64 &&
               std::all_of(value.begin(), value.end(),
                           [](QChar ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
    }
    static bool safeFolder(const QString &path)
    {
        const QFileInfo folder(path), owner(folder.dir().absolutePath()), root(cacheRoot());
        return folder.isDir() && !folder.isSymLink() && !owner.isSymLink() && isHash(folder.fileName()) &&
               isHash(owner.fileName()) && !root.canonicalFilePath().isEmpty() &&
               folder.canonicalFilePath().startsWith(root.canonicalFilePath() + '/');
    }
    static qint64 cacheBytes()
    {
        qint64 bytes = 0;
        QDirIterator files(cacheRoot(), QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (files.hasNext())
        {
            files.next();
            bytes += files.fileInfo().size();
        }
        return bytes;
    }
    static void discard(const QJsonObject &entry, QSettings &settings)
    {
        const auto owner = entry["scope"].toString(), id = entry["id"].toString();
        if (!isHash(owner))
            return;
        const auto folder = cacheRoot() + '/' + owner + '/' + hash(id);
        if (safeFolder(folder))
            QDir(folder).removeRecursively();
        settings.remove("recallDrafts/" + owner + "/messages/" + hash(id));
    }
    static void prune(QSettings &settings, qint64 now, int maximum = 64)
    {
        QJsonArray retained;
        QSet<QString> folders;
        for (const auto &value : index(settings))
        {
            const auto entry = value.toObject();
            const auto age = now - entry["created"].toInteger();
            if (age < 0 || age >= 86400000 || !isHash(entry["scope"].toString()))
                discard(entry, settings);
            else
                retained.append(entry);
        }
        while (retained.size() > maximum)
        {
            discard(retained.first().toObject(), settings);
            retained.removeFirst();
        }
        for (const auto &value : retained)
        {
            const auto entry = value.toObject();
            folders.insert(cacheRoot() + '/' + entry["scope"].toString() + '/' +
                           hash(entry["id"].toString()));
        }
        // A crash between copying a temporary image and committing its record leaves no permanent orphan.
        const QDir root(cacheRoot());
        for (const auto &owner : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
            if (isHash(owner.fileName()))
            {
                const QDir directory(owner.absoluteFilePath());
                for (const auto &folder :
                     directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
                    if (!folders.contains(folder.absoluteFilePath()) && safeFolder(folder.absoluteFilePath()))
                        QDir(folder.absoluteFilePath()).removeRecursively();
                root.rmdir(owner.fileName()); // Succeeds only for an empty scope folder.
            }
        saveIndex(settings, retained);
    }
    QString scope;
};
