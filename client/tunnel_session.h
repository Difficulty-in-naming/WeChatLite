#pragma once
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class QProcess;

class TunnelSession final : public QObject
{
    Q_OBJECT
  public:
    explicit TunnelSession(QObject *parent = nullptr, const QString &executable = {});
    ~TunnelSession() override;
    void start(const QJsonObject &settings);
    void stop();
  signals:
    void ready(const QString &url);
    void pathChanged(const QString &path);
    void failed(const QString &message);
  private:
    void readOutput(QProcess *source, quint64 expected);
    void fail(const QString &message);
    QString program;
    QProcess *process = nullptr;
    QTimer startup;
    QByteArray buffer;
    QJsonObject configuration;
    quint64 generation = 0;
    bool announced = false;
};
