#pragma once
#include <QString>
#include <QStringList>
#include <QWidget>

class ServerAutostart
{
  public:
    enum class Platform { Windows, MacOS, Unsupported };
    ServerAutostart(Platform platform, QString location, QString executable, QString configPath);
    static ServerAutostart currentUser(const QString &configPath);
    bool read(bool &enabled, QString &error) const;
    bool setEnabled(bool enabled, QString &error) const;
    bool refreshIfEnabled(QString &error) const;

  private:
    QStringList arguments() const;
    bool readArguments(QStringList &arguments, QString &error) const;
    Platform platform;
    QString location, executable, configPath;
};

class AutostartSettings final : public QWidget
{
  public:
    explicit AutostartSettings(const ServerAutostart &registration, QWidget *parent = nullptr);
};
