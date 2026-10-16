#pragma once
#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QListView>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTimer>

class QMovie;
class QMenu;
class SmoothScroll;

QMenu *createChatContextMenu(QWidget *parent);
void execChatContextMenu(QMenu *menu, const QPoint &anchor);

QPixmap chatAvatar(const QJsonObject &entity, int size);
bool canRecallMessage(const QJsonObject &message, qint64 now);
QString messageQuoteLabel(const QJsonObject &quote);

class MessageModel : public QAbstractListModel
{
    Q_OBJECT
  public:
    explicit MessageModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    QList<QJsonObject> rows;
    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(rows.size());
    }
    QVariant data(const QModelIndex &index, int role) const override;
    void replace(const QJsonArray &messages);
    void upsert(const QJsonObject &message);
    void prepend(const QJsonArray &messages);
    void setLocalMedia(const QString &id, const QString &path);
    void markQuoteUnavailable(const QString &id);
    void setCanReedit(const QString &id, bool value);

  private:
    QJsonObject normalize(QJsonObject message);
    QSet<QString> recalledIds;
    QStringList recalledOrder;
};
class MessageDelegate : public QStyledItemDelegate
{
    Q_OBJECT
  public:
    explicit MessageDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    static int textHeight(const QJsonObject &message, int width, const QFont &font);
    static QRect bubbleRect(const QStyleOptionViewItem &option, const QModelIndex &index);
    static QRect quoteRect(const QStyleOptionViewItem &option, const QModelIndex &index);
    static QRect reeditRect(const QStyleOptionViewItem &option, const QModelIndex &index);
};
// Keep the visible document position when asynchronous model updates resize earlier rows.
class StableListView : public QListView
{
  public:
    explicit StableListView(QWidget *parent = nullptr);
    void setModel(QAbstractItemModel *model) override;
    void doItemsLayout() override;

  protected:
    void dataChanged(const QModelIndex &first, const QModelIndex &last,
                     const QList<int> &roles = {}) override;
    void resizeEvent(QResizeEvent *event) override;
    SmoothScroll *smoothScroll = nullptr;

  private:
    void rememberReadingPosition();
    struct ReadingPosition
    {
        QPersistentModelIndex index;
        QString id, group;
        int offset = 0, scrollValue = 0;
        bool pending = false, bottom = false;
    } reading;
    QList<QMetaObject::Connection> anchorConnections;
    bool layingOut = false;
};
class MessageView : public StableListView
{
    Q_OBJECT
  public:
    explicit MessageView(QWidget *parent = nullptr);
    ~MessageView() override;
    void setModel(QAbstractItemModel *model) override;
    QMenu *contextMenuFor(const QModelIndex &index, bool avatar);
    QMenu *quoteContextMenuFor(const QModelIndex &index);
    void stopScrollAnimation();
    bool scrollAnimationActive() const;
    bool locateMessage(const QString &id);
    void setVoiceState(const QString &id, const QString &state);
  signals:
    void mediaClicked(const QJsonObject &media, bool saveOnly);
    void mentionRequested(const QJsonObject &member);
    void quoteRequested(const QJsonObject &message);
    void recallRequested(const QJsonObject &message);
    void reeditRequested(const QString &id);
    void locateRequested(const QJsonObject &quote);
    void latestRequested();
    void olderRequested();
    void viewportChanged();

  protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    struct GifAnimation
    {
        QMovie *movie;
        QString cacheKey;
        QList<QPersistentModelIndex> indices;
    };
    void scheduleGifUpdate();
    void updateVisibleGifs();
    void clearGifs();
    void removeGif(const QString &path);
    void requestOlder();
    QTimer gifUpdateTimer;
    QHash<QString, GifAnimation> gifs;
    QList<QMetaObject::Connection> modelConnections;
    QPointer<QWidget> observedWindow;
    QString gifCachePrefix;
};
class GroupDelegate : public QStyledItemDelegate
{
  public:
    explicit GroupDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {260, 65}; }
};
