#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <functional>

using JsonCallback = std::function<void(const QJsonValue &, const QString &)>;
class Transport : public QObject
{
    Q_OBJECT
  public:
    explicit Transport(QObject *parent = nullptr);
    void configure(const QString &url, const QString &token, const QString &certificatePin = {});
    void connectEvents();
    void disconnectService();
    void setOwnedServer(const QString &url, const QString &secret, const QString &certificatePin = {});
    void shutdownLocalServer(class QProcess *process);
    void get(const QString &path, JsonCallback callback, int timeoutMs = 180000);
    void post(const QString &path, const QJsonObject &object, JsonCallback callback, int timeoutMs = 180000);
    void upload(const QString &group, const QString &path, const QString &name, JsonCallback callback);
    void cancelSend();
    void download(const QJsonObject &media, const QString &directory,
                  std::function<void(const QString &, const QString &)> callback);
    QUrl baseUrl() const { return base; }
  signals:
    void event(const QJsonObject &event);
    void connectionChanged(bool connected);
    void warning(const QString &message);
    void progress(qint64 done, qint64 total);

  private:
    QNetworkRequest request(const QString &path) const;
    void finishJson(class QNetworkReply *reply, JsonCallback callback);
    void setupTls(class QNetworkReply *reply, const QByteArray &expectedPin);
    void trackSend(class QNetworkReply *reply);
    QNetworkAccessManager network;
    QWebSocket socket;
    QUrl base;
    QByteArray token, pin, eventPin;
    QTimer reconnect;
    bool stopping = false;
    int delay = 1000;
    qint64 cursor = 0;
    QString identity;
    QUrl ownedServer;
    QByteArray ownedToken, ownedPin;
    QSet<class QNetworkReply *> sendReplies;
};
