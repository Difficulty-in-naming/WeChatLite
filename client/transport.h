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
    // Abandons any pending backoff and re-opens the event socket immediately, so the
    // "重试" affordance never waits out a 30 s retry timer.
    void reconnectNow();
    void disconnectService();
    void setOwnedServer(const QString &url, const QString &secret, const QString &certificatePin = {});
    void shutdownLocalServer(class QProcess *process);
    void get(const QString &path, JsonCallback callback, int timeoutMs = 180000);
    void post(const QString &path, const QJsonObject &object, JsonCallback callback, int timeoutMs = 180000);
    void upload(const QString &group, const QString &path, const QString &name, JsonCallback callback);
    void cancelSend();
    void download(const QJsonObject &media, const QString &directory,
                  std::function<void(const QString &, const QString &)> callback);
    void downloadPoster(const QJsonObject &media, const QString &directory,
                        std::function<void(const QString &, const QString &)> callback);
    QUrl baseUrl() const { return base; }
    bool hasActiveDownloads() const { return !partialDownloads.isEmpty(); }
    // True when retrying cannot help (for example a certificate pin mismatch). The
    // client must show the reason and ask for a corrected configuration instead of
    // spinning forever on a connection that will never be accepted.
    bool isBlocked() const { return blocked; }
  signals:
    void event(const QJsonObject &event);
    void connectionChanged(bool connected);
    void warning(const QString &message);
    void progress(qint64 done, qint64 total);

  private:
    QNetworkRequest request(const QString &path) const;
    void failConnection(const QString &message, bool fatal);
    void stopLinkTimers();
    void scheduleReconnect();
    void finishJson(class QNetworkReply *reply, JsonCallback callback);
    void setupTls(class QNetworkReply *reply, const QByteArray &expectedPin);
    void trackSend(class QNetworkReply *reply);
    void downloadFile(const QJsonObject &media, const QString &directory, bool poster,
                      std::function<void(const QString &, const QString &)> callback);
    QNetworkAccessManager network;
    QWebSocket socket;
    QUrl base;
    QByteArray token, pin, eventPin;
    QTimer reconnect;
    // Guards the two silent failure modes a TCP socket alone does not report: a
    // handshake that never completes, and a link that stays open while the peer or
    // an intermediate network has silently stopped forwarding data.
    QTimer connectGuard;
    QTimer lastSeen;
    qint64 lastActivity = 0;
    bool awaitingConnect = false;
    bool blocked = false;
    bool stopping = false;
    int delay = 1000;
    qint64 cursor = 0;
    QString identity;
    QUrl ownedServer;
    QByteArray ownedToken, ownedPin;
    QSet<class QNetworkReply *> sendReplies;
    QSet<QString> partialDownloads;
};
