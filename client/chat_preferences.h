#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class ChatPreferences
{
  public:
    enum class NotificationMode { Ignore, MentionsOnly, AllMessages };
    bool configure(const QString &logicalServerUrl, const QString &token);
    QString scope() const { return key; }
    bool pinned(const QString &group) const;
    bool muted(const QString &group) const;
    NotificationMode notificationMode(const QString &group) const;
    bool allowsNotification(const QString &group, const QJsonArray &mentions,
                            const QString &selfId) const;
    static bool mentionsSelf(const QJsonArray &mentions, const QString &selfId);
    void setPinned(const QString &group, bool value);
    void setMuted(const QString &group, bool value);
    void setNotificationMode(const QString &group, NotificationMode value);
  private:
    void set(const QString &group, bool pinned, NotificationMode mode);
    QString key;
    QJsonObject values;
};
