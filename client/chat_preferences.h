#pragma once
#include <QJsonObject>
#include <QString>

class ChatPreferences
{
  public:
    bool configure(const QString &logicalServerUrl, const QString &token);
    QString scope() const { return key; }
    bool pinned(const QString &group) const;
    bool muted(const QString &group) const;
    void setPinned(const QString &group, bool value);
    void setMuted(const QString &group, bool value);
  private:
    void set(const QString &group, const QString &field, bool value);
    QString key;
    QJsonObject values;
};
