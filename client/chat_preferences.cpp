#include "chat_preferences.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSettings>

bool ChatPreferences::configure(const QString &logicalServerUrl, const QString &token)
{
    auto identity = logicalServerUrl.toUtf8();
    identity.append('\0');
    identity.append(token.toUtf8());
    const auto next = "chatPreferences/" +
        QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    if (next == key)
        return false;
    key = next;
    const auto bytes = QSettings().value(key).toByteArray();
    values = bytes.size() <= 512 * 1024 ? QJsonDocument::fromJson(bytes).object() : QJsonObject{};
    if (values.size() > 4096)
        values = {};
    return true;
}

bool ChatPreferences::pinned(const QString &group) const
{
    return values.value(group).toObject().value("pinned").toBool();
}

bool ChatPreferences::muted(const QString &group) const
{
    return values.value(group).toObject().value("muted").toBool();
}

void ChatPreferences::setPinned(const QString &group, bool value) { set(group, "pinned", value); }
void ChatPreferences::setMuted(const QString &group, bool value) { set(group, "muted", value); }

void ChatPreferences::set(const QString &group, const QString &field, bool value)
{
    if (key.isEmpty() || group.isEmpty() || group.size() > 512)
        return;
    auto next = values;
    auto entry = next.value(group).toObject();
    entry[field] = value;
    if (!entry.value("pinned").toBool() && !entry.value("muted").toBool())
        next.remove(group);
    else
        next[group] = entry;
    if (next.size() > 4096)
        return;
    const auto bytes = QJsonDocument(next).toJson(QJsonDocument::Compact);
    if (bytes.size() > 512 * 1024)
        return;
    values = next;
    QSettings().setValue(key, bytes);
}
