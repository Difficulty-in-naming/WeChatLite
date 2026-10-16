#pragma once
#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

class Transport;
class QAbstractListModel;
class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPushButton;

class HistoryBrowser : public QDialog
{
    Q_OBJECT
  public:
    HistoryBrowser(Transport *transport, const QString &group, const QString &name,
                   const QString &downloadRoot, bool canDownload, QWidget *parent = nullptr);
    QString group() const { return groupId; }
    void invalidate();
    void updateMessage(const QJsonObject &message);
    QSet<QString> protectedCachePaths() const;
    void discardCached(const QStringList &paths);
  signals:
    void recordActivated(const QJsonObject &message, const QJsonObject &media);
    void cacheUpdated();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    void resetSearch();
    void loadPage();
    void loadMembers();
    void loadVisibleThumbnails();
    void activate(const QModelIndex &index, const QPoint &point);
    void updateThemeStyle();
    QPointer<Transport> transport;
    QString groupId, downloadRoot, nextBefore, type = "all";
    QAbstractListModel *records;
    QListView *list;
    QLineEdit *search;
    QComboBox *member;
    QCheckBox *dateEnabled;
    QDateEdit *date;
    QLabel *state;
    QPushButton *more;
    QTimer searchTimer, thumbnailTimer;
    QSet<QString> downloading, failedThumbnails;
    QHash<QString, QJsonObject> updates;
    QStringList updateOrder;
    quint64 generation = 0;
    int requestsInFlight = 0;
    bool loading = false, hasMore = true, valid = true, allowDownload = false, searchQueued = false;
    bool updatingThemeStyle = false;
};
