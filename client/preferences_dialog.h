#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QListWidget;

class PreferencesDialog : public QDialog
{
    Q_OBJECT
  public:
    struct Values
    {
        QString downloads;
        QString theme = "system";
        bool notificationSound = true;
        qint64 cacheLimitBytes = 512LL * 1024 * 1024;
        // Wheel damping: 1.0 is raw speed, lower is slower. See SmoothScroll.
        qreal wheelDamping = 0.35;
    };
    // Connection state lives outside Values because it is applied live — connect,
    // disconnect and retry act on the spot instead of waiting for dialog save — so
    // the storage and appearance pages stay usable while the client is offline.
    struct Connection
    {
        QString serverUrl;
        QString token;
        QString nickname;
        QString certificatePin;
        QString mode = "direct";
        QJsonObject p2p;
        bool connected = false;
        bool reconnecting = false;
        QString status;
    };
    explicit PreferencesDialog(const Values &initial, const Connection &connection,
                               QWidget *parent = nullptr);
    Values values() const;
    Connection connection() const;

    // Driven by the window as the transport changes, so an open dialog shows the
    // same connection state as the chat banner instead of a stale snapshot.
    void setConnectionState(bool connected, bool reconnecting, const QString &status);
    // Selects a page by its row, so entry points can land on the relevant one.
    void selectPage(int row);

  public slots:
    void refreshUsage();
    void setUsage(quint64 requestId, qint64 downloadsBytes, qint64 cacheBytes,
                  const QString &error = {});
    void cacheCleared(quint64 requestId, const QString &error = {});
    void accept() override;

  signals:
    void usageRequested(const QString &downloads, quint64 requestId);
    // The path identifies the displayed statistics. Only the application's fixed
    // preview/voice cache is eligible for deletion; this is never a downloads purge.
    void clearCacheRequested(const QString &downloads, quint64 requestId);
    void previewSoundRequested();
    void applied();
    // Emitted when the user asks to apply the connection fields right away.
    void connectRequested();
    // Emitted when the user asks to stop using the current connection. The dialog
    // keeps the saved address and key so reconnecting does not need retyping.
    void disconnectRequested();

  private:
    void chooseDirectory();
    void clearCache();
    void updateUsageState();
    void updateConnectionButtons();
    void pasteConnectionInfo();
    QListWidget *navigation;
    QLineEdit *directory;
    QComboBox *theme, *cacheLimit, *wheelSpeed;
    QComboBox *connectionMode;
    QLineEdit *serverUrl, *accessKey, *nickname, *certificatePin;
    QLabel *connectionStatus, *connectionInfo, *pasteResult;
    QLabel *downloadsUsage, *cacheUsage, *usageStatus, *validation;
    QPushButton *choose, *refresh, *clear, *preview;
    QPushButton *connectButton, *disconnectButton, *pasteButton;
    QToolButton *showKey;
    QCheckBox *sound;
    quint64 sequence = 0, usageRequest = 0, clearRequest = 0;
    bool scanning = false, clearing = false, connected = false, reconnecting = false;
    QJsonObject p2p;
    QString actionFeedback;
};
