#pragma once
#include "transport.h"
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QTimer>

class QLabel;
class QPushButton;
class QPlainTextEdit;
class ForwardingSettings;
class ClientPermissions;

class ServerWindow : public QMainWindow
{
    Q_OBJECT
  public:
    explicit ServerWindow(const QString &configPath, bool demo = false, QWidget *parent = nullptr);
    ~ServerWindow() override;
    void startService();
    void stopService();

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    bool ensureConfiguration(QString &error);
    void configureService(bool showAutomatic = false);
    void showSoftwareUpdate();
    void showAutostart();
    void pollStatus();
    void appendLog(const QString &text);
    QString configPath;
    bool demo, quitting = false, polling = false, p2pPolling = false, observedHttp = false;
    quint64 serviceGeneration = 0;
    QJsonObject runningConfig;
    Transport admin;
    QProcess service;
    QSystemTrayIcon tray;
    QTimer statusTimer;
    QLabel *state, *bridge, *address, *online, *feedback, *p2pState, *p2pId;
    QPushButton *start, *stop, *settings;
    QPlainTextEdit *log;
    ForwardingSettings *forwarding;
    ClientPermissions *clients;
};
