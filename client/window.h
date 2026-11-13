#pragma once
#include "attachment.h"
#include "chat_preferences.h"
#include "message_cache.h"
#include "recall_drafts.h"
#include "transport.h"
#include "tunnel_session.h"
#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>
#include <QPointer>
#include <QQueue>
#include <QSystemTrayIcon>
#include <QTimer>
#include <atomic>
#include <memory>

namespace MediaStorage { class LocalFiles; }

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
class HistoryBrowser;
class AnnouncementDialog;
class PreferencesDialog;
class QScreen;
class QDialog;
class QFrame;

QRect chatWindowBounds(const QRect &available, const QRect &requested = {});

class Window : public QMainWindow
{
    Q_OBJECT
  public:
    explicit Window(bool demo = false, const QString &tunnelExecutable = {});
    // Landing page for the preferences window; connection failures open it on
    // Connection so a wrong address is one click from being fixed.
    enum class PreferencesPage { Connection, Storage, Appearance, Notification, Update };
    void showSettings();
    void showPreferences(PreferencesPage page = PreferencesPage::Storage);
    void startDemo();
    void activateConversation(const QString &id);
    QMenu *groupContextMenuFor(const QString &id);
  signals:
    void systemNotificationRequested(const QString &groupId, const QString &title, const QString &body);
    // Client-level trouble (unreachable server, rejected certificate). Not tied to a
    // conversation and deliberately not activatable.
    void statusNotificationRequested(const QString &title, const QString &body);
    void notificationSoundPreviewRequested();
    // Aggregate unread across conversations, mirrored to the Dock tile by the native notifier.
    void unreadTotalChanged(int count);

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
    void showHistoryBrowser();
    void showAnnouncement();
    void invalidateAuxiliaryWindows();
    void showForwardRecord(const QJsonObject &message);
    void refreshForwardRecord();
    void activateHistoryRecord(const QJsonObject &message, const QJsonObject &media);
    void playLocatedRecord(const QString &messageId);
    void clearPendingRecord(const QString &messageId);
    void loadGroups();
    void sortGroups();
    bool rememberMessage(const QJsonObject &message);
    void maybeNotifyMessage(const QJsonObject &event, bool firstSeen);
    void updateUnreadTotal();
    void markConversationRead();
    void selectGroup(const QString &id, const QString &name);
    // Paints the stored rows of the current group before the first history page.
    void restoreCachedMessages();
    void storeCachedMessages();
    void scheduleCacheWrite();
    void initializeMessageCache();
    void loadHistory(bool older = false, int pendingAttempt = 0);
    void loadMembers();
    void activateCurrentGroup();
    void handleEvent(const QJsonObject &event);
    void refreshAttachments();
    void updateSendButton();
    void quoteMessage(const QJsonObject &message);
    void refreshQuote();
    void recallMessage(const QJsonObject &message);
    void reeditMessage(const QString &id);
    void cacheMessageDraft(const QJsonObject &message);
    void refreshReeditLinks();
    void locateQuote(const QJsonObject &quote);
    void refreshQuoteSource(const QJsonObject &quote);
    void updateNativeFrame();
    void refreshThemeIcons();
    void pruneMediaCache(qint64 limit, std::function<void(const QString &)> callback = {});
    void updateCachePins();
    QJsonArray hydrateMessages(const QJsonArray &snapshot) const;
    void recoverCachedMessages(const QJsonArray &snapshot, std::function<void()> callback);
    bool hasCurrentMedia(const QJsonObject &media) const;
    void updateComposeHeight();
    void fitToScreen(bool initial = false);
    void watchScreen(QScreen *screen);
    void send();
    void uploadNext(int index, quint64 generation, const QString &targetGroup);
    void clearConversation();
    void cancelSend();
    void saveDraft();
    void restoreDraft();
    void notify(const QString &text, bool error = false);
    void showBanner(const QString &text, bool error, const QString &action = QStringLiteral("settings"),
                    const QString &actionText = QStringLiteral("打开设置"));
    void hideBanner();
    void positionBanner();
    void setConnectionState(const QString &text, bool reconnecting);
    void fetchMedia(const QJsonObject &media, bool saveOnly = false, bool thumbnail = false, bool resumed = false);
    void copyMedia(const QJsonObject &media);
    void copyMediaToClipboard(const QJsonObject &media, const QString &path);
    void drainMediaQueue();
    void openMedia(const QJsonObject &media, const QString &path);
    void saveMediaAndMaybeOpen(const QJsonObject &media, const QString &path, bool open);
    void stopVoice();
    void visibleThumbnails();
    void fetchVideoPoster(const QJsonObject &media);
    QString draftKey() const;
    Transport transport;
    TunnelSession tunnel;
    AttachmentStore attachments;
    // Uninitialised pointers are read by event filters installed during makeUi(), before
    // the corresponding widgets exist; keep them nullable from construction.
    MessageModel *model = nullptr;
    MessageView *messages = nullptr;
    Composer *composer = nullptr;
    QListWidget *groups = nullptr;
    QLineEdit *groupSearch;
    QLabel *title, *subtitle, *connection, *transferLabel, *attachmentCount, *demoLabel;
    // Floating status strip. It is a child overlay rather than a layout row so that a
    // connection message can never resize the compose box under the user's cursor.
    QFrame *banner = nullptr;
    QLabel *bannerLabel = nullptr;
    QToolButton *bannerAction = nullptr;
    QToolButton *bannerRetrySettings = nullptr;
    bool bannerError = false;
    bool positioningBanner = false;
    QPushButton *sendButton = nullptr, *historyButton = nullptr, *announcementButton = nullptr,
                *preferencesButton = nullptr;
    QPointer<PreferencesDialog> preferencesDialog;
    QPointer<HistoryBrowser> historyBrowser;
    QPointer<AnnouncementDialog> announcementDialog;
    QPointer<QDialog> forwardDialog;
    MessageModel *forwardModel = nullptr;
    MessageView *forwardView = nullptr;
    QJsonObject forwardMessage;
    QString pendingRecordId;
    QJsonObject pendingRecordMedia;
    QScrollArea *attachmentArea;
    QHBoxLayout *attachmentLayout;
    QWidget *quoteArea = nullptr;
    QWidget *composeResizeHandle = nullptr;
    QLabel *quoteLabel = nullptr;
    QToolButton *cancelQuoteButton = nullptr;
    VoicePlayer *voicePlayer = nullptr;
    QString requestedVoiceId, playingVoiceId, playingVoicePath, voiceGroup;
    quint64 voiceConversation = 0;
    QJsonObject quotedMessage;
    QJsonObject pendingMediaCopy;
    QJsonObject p2pSettings;
    RecallDrafts recallDrafts;
    ChatPreferences chatPreferences;
    // Last known rows per conversation, so opening one does not start blank.
    MessageCache messageCache;
    bool cacheReady = false;
    QSet<QByteArray> seenMessages;
    QQueue<QByteArray> seenMessageOrder;
    bool notificationsSynced = false;
    qint64 notificationSyncSeq = 0;
    QString selfMemberId;
    QString locatingId;
    QString pendingActivatedGroup;
    quint64 locateGeneration = 0;
    QProcess demoServer;
    QSystemTrayIcon tray;
    QTimer draftTimer, thumbnailTimer, cachePruneTimer, cacheWriteTimer;
    // Startup watchdog: if nothing has connected shortly after launch, say so once even
    // when the transport has not produced an error yet (silent SYN retries, helper hang).
    QTimer startupWatchdog;
    bool startupAlerted = false;
    bool demoMode = false;
    bool cacheWorking = false;
    bool composeDragging = false, screenWatching = false;
    // True while the highlighted conversation is being changed from the keyboard, so the
    // focus can stay in the list instead of jumping back to the editor after every row.
    bool groupSelectionFromKeyboard = false;
    int composeHeightPreference = 0, composeDragHeight = 0;
    QPoint composeDragStart;
    QList<QWidget *> resizeEdges;
    QString groupId, serverUrl, secret, downloadRoot, nickname, clientId, pin, pendingId, demoFailure;
    QString lastError;
    QJsonArray memberCache;
    std::shared_ptr<MediaStorage::LocalFiles> localFiles;
    std::shared_ptr<std::atomic<quint64>> cacheRecoveryGeneration = std::make_shared<std::atomic<quint64>>(0);
    QHash<QString, int> unread;
    // Of those unread messages, the ones that mentioned this account (or everyone). The Dock badge
    // derives its total from both counters plus the current notification level, so a conversation
    // switched back to "all messages" regains the messages the quieter levels had dropped, while
    // unread above keeps drawing the plain new-message marker in the conversation list.
    QHash<QString, int> unreadMentions;
    QHash<QString, QJsonObject> historyUpdates;
    QSet<QString> downloading, openRequests, saveRequests;
    QSet<QString> queuedMediaIds;
    QQueue<std::function<void()>> mediaQueue;
    QSet<QString> unavailableOriginals;
    QSet<QString> pendingPosters, failedPosters;
    quint64 sendGeneration = 0, groupsGeneration = 0, conversationGeneration = 0, historyGeneration = 0;
    quint64 permissionsGeneration = 0;
    quint64 recallCapabilityGeneration = 0;
    bool busy = false, ready = false, connected = false, loading = false, restoring = false, quitting = false,
         hasOlder = true, canSend = true, canDownload = true;
};
