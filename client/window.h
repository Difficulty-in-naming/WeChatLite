#pragma once
#include "attachment.h"
#include "chat_preferences.h"
#include "recall_drafts.h"
#include "transport.h"
#include "tunnel_session.h"
#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>
#include <QQueue>
#include <QSystemTrayIcon>
#include <QTimer>

class Composer;
class MessageModel;
class MessageView;
class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QHBoxLayout;
class QScrollArea;
class QToolButton;
class VoicePlayer;
class QMenu;

class Window : public QMainWindow
{
    Q_OBJECT
  public:
    explicit Window(bool demo = false, const QString &tunnelExecutable = {});
    void showSettings();
    void startDemo();
    void activateConversation(const QString &id);
    QMenu *groupContextMenuFor(const QString &id);
  signals:
    void systemNotificationRequested(const QString &groupId, const QString &title, const QString &body);

  protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;

  private:
    void makeUi();
    void connectService();
    void loadPermissions();
    void loadGroups();
    void sortGroups();
    bool rememberMessage(const QJsonObject &message);
    void maybeNotifyMessage(const QJsonObject &event, bool firstSeen);
    void selectGroup(const QString &id, const QString &name);
    void loadHistory(bool older = false, int pendingAttempt = 0);
    void loadMembers();
    void activateCurrentGroup();
    void handleEvent(const QJsonObject &event);
    void refreshAttachments();
    void quoteMessage(const QJsonObject &message);
    void refreshQuote();
    void recallMessage(const QJsonObject &message);
    void reeditMessage(const QString &id);
    void cacheMessageDraft(const QJsonObject &message);
    void refreshReeditLinks();
    void locateQuote(const QJsonObject &quote);
    void updateNativeFrame();
    void updateComposeHeight();
    void send();
    void uploadNext(int index, quint64 generation, const QString &targetGroup);
    void clearConversation();
    void cancelSend();
    void saveDraft();
    void restoreDraft();
    void notify(const QString &text, bool error = false);
    void fetchMedia(const QJsonObject &media, bool saveOnly = false, bool thumbnail = false);
    void openMedia(const QJsonObject &media, const QString &path);
    void stopVoice();
    void visibleThumbnails();
    QString draftKey() const;
    Transport transport;
    TunnelSession tunnel;
    AttachmentStore attachments;
    MessageModel *model;
    MessageView *messages;
    Composer *composer;
    QListWidget *groups;
    QLineEdit *groupSearch;
    QLabel *title, *subtitle, *connection, *transferLabel, *attachmentCount, *demoLabel;
    QPushButton *sendButton;
    QScrollArea *attachmentArea;
    QHBoxLayout *attachmentLayout;
    QWidget *quoteArea = nullptr;
    QLabel *quoteLabel = nullptr;
    QToolButton *cancelQuoteButton = nullptr;
    VoicePlayer *voicePlayer = nullptr;
    QString requestedVoiceId, playingVoiceId, voiceGroup;
    quint64 voiceConversation = 0;
    QJsonObject quotedMessage;
    QJsonObject p2pSettings;
    RecallDrafts recallDrafts;
    ChatPreferences chatPreferences;
    QSet<QByteArray> seenMessages;
    QQueue<QByteArray> seenMessageOrder;
    bool notificationsSynced = false;
    qint64 notificationSyncSeq = 0;
    QString locatingId;
    QString pendingActivatedGroup;
    quint64 locateGeneration = 0;
    QProcess demoServer;
    QSystemTrayIcon tray;
    QTimer draftTimer, thumbnailTimer;
    QList<QWidget *> resizeEdges;
    QString groupId, serverUrl, secret, downloadRoot, nickname, clientId, pin, pendingId, demoFailure;
    QString lastError;
    QJsonArray memberCache;
    QHash<QString, QString> downloaded;
    QHash<QString, int> unread;
    QHash<QString, QJsonObject> historyUpdates;
    QSet<QString> downloading, openRequests;
    QSet<QString> unavailableOriginals;
    quint64 sendGeneration = 0, groupsGeneration = 0, conversationGeneration = 0, historyGeneration = 0;
    quint64 permissionsGeneration = 0;
    bool busy = false, ready = false, connected = false, loading = false, restoring = false, quitting = false,
         hasOlder = true, canSend = true, canDownload = true;
};
