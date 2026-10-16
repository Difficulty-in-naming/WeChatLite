#pragma once
#include <QSet>
#include <QStringList>
#include <functional>

namespace MediaStorage
{
struct Usage { qint64 downloads = 0, cache = 0, available = -1; QString error; };
struct Pruned { QStringList removed; qint64 retained = 0; QString error; };
QString cacheDirectory();
bool isCachePath(const QString &path);
qint64 cacheLimit();
Usage measure(const QString &downloads, std::function<bool()> cancelled = {});
Pruned prune(qint64 limit, const QSet<QString> &protectedPaths);
QString saveCached(const QString &source, const QString &downloads, const QString &name, QString &error);
}
