#pragma once
#include <QJsonObject>
#include <QString>

namespace ConnectionInfo
{
constexpr qsizetype maximumBytes = 64 * 1024;
bool relaySettings(const QJsonObject &settings, QString &error, bool requireServerId = false);
bool parse(const QByteArray &bytes, QJsonObject &connection, QString &error);
bool publicEntry(const QByteArray &bytes, QJsonObject &settings, QString &error);
} // namespace ConnectionInfo
