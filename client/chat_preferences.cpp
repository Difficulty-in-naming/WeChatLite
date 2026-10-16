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
    auto migrated = values;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        auto entry = it.value().toObject();
        if (!entry.contains("muted"))
            continue;
        const auto mode = notificationMode(it.key());
        entry.remove("muted");
        if (mode == NotificationMode::AllMessages)
            entry.remove("notification_mode");
        else
            entry["notification_mode"] = mode == NotificationMode::Ignore ? "ignore" : "mentions";
        if (!entry.value("pinned").toBool() && mode == NotificationMode::AllMessages)
            migrated.remove(it.key());
        else
            migrated[it.key()] = entry;
    }
    if (migrated != values)
    {
        const auto migratedBytes = QJsonDocument(migrated).toJson(QJsonDocument::Compact);
        if (migratedBytes.size() <= 512 * 1024)
        {
            values = migrated;
            QSettings().setValue(key, migratedBytes);
        }
    }
    return true;
}

bool ChatPreferences::pinned(const QString &group) const
{
    return values.value(group).toObject().value("pinned").toBool();
}

bool ChatPreferences::muted(const QString &group) const
{
    return notificationMode(group) == NotificationMode::Ignore;
}

ChatPreferences::NotificationMode ChatPreferences::notificationMode(const QString &group) const
{
    const auto entry = values.value(group).toObject();
    const auto mode = entry.value("notification_mode").toString();
    if (mode == "ignore")
        return NotificationMode::Ignore;
    if (mode == "mentions")
        return NotificationMode::MentionsOnly;
    if (mode == "all")
        return NotificationMode::AllMessages;
    return entry.value("muted").toBool() ? NotificationMode::Ignore : NotificationMode::AllMessages;
}

bool ChatPreferences::allowsNotification(const QString &group, const QJsonArray &mentions,
                                        const QString &selfId) const
{
    const auto mode = notificationMode(group);
    if (mode != NotificationMode::MentionsOnly)
        return mode == NotificationMode::AllMessages;
    for (const auto &mention : mentions)
    {
        const auto id = mention.toObject().value("id").toString();
        if (id == "notify@all" || (!selfId.isEmpty() && id == selfId))
            return true;
    }
    return false;
}

void ChatPreferences::setPinned(const QString &group, bool value)
{
    set(group, value, notificationMode(group));
}
void ChatPreferences::setMuted(const QString &group, bool value)
{
    setNotificationMode(group, value ? NotificationMode::Ignore : NotificationMode::AllMessages);
}
void ChatPreferences::setNotificationMode(const QString &group, NotificationMode value)
{
    set(group, pinned(group), value);
}

void ChatPreferences::set(const QString &group, bool pin, NotificationMode mode)
{
    if (key.isEmpty() || group.isEmpty() || group.size() > 512)
        return;
    auto next = values;
    auto entry = next.value(group).toObject();
    entry.remove("muted");
    entry["pinned"] = pin;
    if (mode == NotificationMode::AllMessages)
        entry.remove("notification_mode");
    else
        entry["notification_mode"] = mode == NotificationMode::Ignore ? "ignore" : "mentions";
    if (!pin && mode == NotificationMode::AllMessages)
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
