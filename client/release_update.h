#pragma once
#include <QObject>
#include <QNetworkProxy>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <atomic>
#include <functional>
#include <memory>

class QFile;
class QCryptographicHash;
class QNetworkAccessManager;
class QNetworkReply;
class UpdateTests;

// A user-initiated GitHub update check. This never replaces or starts an application.
class ReleaseUpdate final : public QObject
{
    Q_OBJECT
  public:
    enum class Role { Client, Server };
    Q_ENUM(Role)
    enum class State { Idle, Checking, Downloading, UpToDate, Ready, Failed, Cancelled };
    Q_ENUM(State)
    explicit ReleaseUpdate(Role role, const QString &currentVersion, QObject *parent = nullptr);
    ~ReleaseUpdate() override;
    State state() const { return currentState; }
    QString version() const { return latestVersion; }
    QString filePath() const { return readyPath; }
    QString errorString() const { return failure; }
  public slots:
    void check();
    void cancel();
  signals:
    void stateChanged(ReleaseUpdate::State state);
    void downloadProgress(qint64 received, qint64 total);
    void ready(const QString &version, const QString &path);
    void failed(const QString &message);
    void upToDate();
  private:
    friend class UpdateTests;
    static QNetworkProxyFactory *updateProxyFactory(
        std::function<QList<QNetworkProxy>(const QNetworkProxyQuery &)> resolver = {});
    static constexpr qint64 maximumAssetSize = 1024LL * 1024 * 1024;
    struct Asset { QString name; QUrl url; QByteArray digest; qint64 size = 0; };
    void setState(State state);
    void resetAttempt();
    void fail(const QString &message);
    void request(const QUrl &url, bool metadata);
    void consume(QNetworkReply *reply, bool metadata);
    void finish(QNetworkReply *reply, bool metadata);
    void selectRelease();
    void prepareDownload();
    void startDownload();
    void completeDownload();
    bool busy() const { return currentState == State::Checking || currentState == State::Downloading; }
    Role role;
    QString installedVersion, system, architecture, cacheRoot;
    QNetworkAccessManager *network;
    QPointer<QNetworkReply> reply;
    QTimer deadline;
    State currentState = State::Idle;
    QString latestVersion, readyPath, failure, partialPath, workDirectory;
    QString cachedVersion, cachedPath;
    QByteArray cachedDigest;
    qint64 cachedSize = 0;
    Asset asset;
    QByteArray metadataBytes;
    std::unique_ptr<QFile> output;
    std::unique_ptr<QCryptographicHash> hash;
    std::shared_ptr<std::atomic_bool> verificationCancelled;
    quint64 generation = 0;
    int redirects = 0, checkTimeoutMs = 30000, downloadTimeoutMs = 600000;
    qint64 received = 0, discarded = 0;
};
