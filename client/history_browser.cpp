#include "history_browser.h"
#include "attachment.h"
#include "messages.h"
#include "media_storage.h"
#include "smooth_scroll.h"
#include "theme.h"
#include "transport.h"
#include "wechat_emoji.h"
#include <QAbstractListModel>
#include <QButtonGroup>
#include <QCache>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmapCache>
#include <QPushButton>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace
{
constexpr int pageSize = 50;
constexpr int retainedRecords = 200;

QColor historyColor(const char *light, const char *dark)
{
    return QColor(Theme::isDark() ? dark : light);
}

QString recordText(const QJsonObject &message)
{
    if (message["status"] == "recalled")
        return "此消息已撤回";
    auto text = message["text"].toString();
    const auto quote = message["quote"].toObject();
    if (!quote.isEmpty())
        text += (text.isEmpty() ? "" : "\n") + messageQuoteLabel(quote);
    if (text.isEmpty() && message["media"].toArray().isEmpty())
        text = "[消息]";
    // The original stays in the model and can be located; an exceptionally long result
    // must not turn one list row into an unbounded text layout.
    return wechatDisplayText(text.size() > 1200 ? text.left(1200) + "…" : text);
}

QString mediaLabel(const QJsonObject &media)
{
    const auto kind = media["kind"].toString();
    const auto label = kind == "image" ? "图片" : kind == "gif" ? "GIF" : kind == "video" ? "视频" :
                       kind == "voice" ? "语音" : "文件";
    const auto name = media["name"].toString();
    return QString("%1%2").arg(label, name.isEmpty() ? "" : "  " + name.left(160));
}

QSize thumbnailSize(const QJsonObject &media)
{
    const auto kind = media["kind"].toString();
    const auto local = media["_local"].toString();
    if ((kind != "image" && kind != "gif") || local.isEmpty())
        return {};
    static QCache<QString, QSize> sizes(256);
    const auto key = local + ':' + media["id"].toString() + ':' + media["sha256"].toString();
    if (const auto *size = sizes.object(key))
        return *size;
    QImageReader reader(local);
    const auto size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 12 * 1024 * 1024)
        return {};
    const auto scaled = size.scaled(kind == "gif" ? QSize(134, 134) : QSize(240, 180), Qt::KeepAspectRatio);
    sizes.insert(key, new QSize(scaled));
    return scaled;
}

struct RowLayout
{
    QRect text;
    QList<QRect> media;
    int height = 0;
};

RowLayout layoutRow(const QJsonObject &message, int width, const QFont &font)
{
    RowLayout result;
    const auto available = qMax(80, width - 94);
    const auto text = recordText(message);
    const QFontMetrics metrics(wechatDisplayFont(font));
    const auto textHeight = text.isEmpty() ? 0 :
        qMin(metrics.lineSpacing() * 8,
             metrics.boundingRect(QRect(0, 0, available, 2000), Qt::TextWordWrap, text).height() + 2);
    result.text = QRect(70, 37, available, textHeight);
    int top = 37 + textHeight;
    const auto media = message["status"] == "recalled" ? QJsonArray{} : message["media"].toArray();
    for (int i = 0; i < qMin(8, media.size()); ++i)
    {
        top += 6;
        const auto item = media[i].toObject();
        const auto size = item["kind"] == "video" ? QSize(240, 144) : thumbnailSize(item);
        const auto row = size.isEmpty() ? QSize(qMin(270, available), 55) : size.boundedTo({available, 180});
        result.media.append(QRect(70, top, row.width(), row.height()));
        top += row.height();
    }
    result.height = qMax(66, top + 19);
    return result;
}

class RecordsModel final : public QAbstractListModel
{
  public:
    using QAbstractListModel::QAbstractListModel;
    QList<QJsonObject> rows;
    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(rows.size());
    }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rows.size())
            return {};
        const auto &row = rows[index.row()];
        if (role == Qt::UserRole)
            return row;
        if (role == Qt::AccessibleTextRole)
            return row["sender"].toString() + " " + recordText(row);
        return {};
    }
    void clear()
    {
        beginResetModel();
        rows.clear();
        endResetModel();
    }
    void append(const QJsonArray &page, const QString &group)
    {
        QList<QJsonObject> incoming;
        for (const auto &value : page)
        {
            auto row = value.toObject();
            const auto id = row["id"].toString();
            if (row["group"] != group || id.isEmpty() || id.size() > 512)
                continue;
            bool duplicate = false;
            for (const auto &existing : rows)
                duplicate |= existing["id"] == id;
            for (const auto &existing : incoming)
                duplicate |= existing["id"] == id;
            if (!duplicate)
                incoming.append(row);
        }
        if (incoming.isEmpty())
            return;
        beginInsertRows({}, int(rows.size()), int(rows.size() + incoming.size() - 1));
        rows.append(incoming);
        endInsertRows();
        const int excess = int(rows.size()) - retainedRecords;
        if (excess > 0)
        {
            beginRemoveRows({}, 0, excess - 1);
            rows.remove(0, excess);
            endRemoveRows();
        }
    }
    void localMedia(const QString &id, const QString &path, const QJsonObject &descriptor = {})
    {
        localMediaPath(id, path, descriptor, "_local");
    }
    void localPoster(const QString &id, const QString &path, const QJsonObject &descriptor = {})
    {
        localMediaPath(id, path, descriptor, "_poster");
    }
    void localMediaPath(const QString &id, const QString &path, const QJsonObject &descriptor, const QString &field)
    {
        for (int row = 0; row < rows.size(); ++row)
        {
            auto media = rows[row]["media"].toArray();
            bool changed = false;
            for (int i = 0; i < media.size(); ++i)
                if (media[i].toObject()["id"] == id && media[i].toObject()[field].toString() != path &&
                    (descriptor.isEmpty() || sameMediaDescriptor(media[i].toObject(), descriptor)))
                {
                    auto item = media[i].toObject();
                    item[field] = path;
                    media[i] = item;
                    changed = true;
                }
            if (changed)
            {
                rows[row]["media"] = media;
                emit dataChanged(index(row), index(row));
            }
        }
    }
    void update(const QJsonObject &message)
    {
        for (int row = 0; row < rows.size(); ++row)
            if (rows[row]["id"] == message["id"])
            {
                rows[row] = preserveLocalMediaState(message, rows[row]);
                emit dataChanged(index(row), index(row));
                return;
            }
    }
};

class RecordDelegate final : public QStyledItemDelegate
{
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        return {option.rect.width(), layoutRow(index.data(Qt::UserRole).toJsonObject(),
                                               option.rect.width(), option.font).height};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto message = index.data(Qt::UserRole).toJsonObject();
        const auto layout = layoutRow(message, option.rect.width(), option.font);
        p->save();
        p->setClipRect(option.rect);
        p->translate(option.rect.topLeft());
        p->setRenderHint(QPainter::Antialiasing);
        p->fillRect(QRect(QPoint(), option.rect.size()),
                    option.state & QStyle::State_Selected ? historyColor("#eeeef0", "#333638")
                                                         : historyColor("#fafafa", "#202020"));
        p->drawPixmap(24, 13, chatAvatar(message, 36));
        auto caption = option.font;
        caption.setPixelSize(12);
        p->setFont(caption);
        p->setPen(historyColor("#9f9fa6", "#999ca3"));
        const auto stamp = message["timestamp"].toInteger();
        const auto time = stamp > 0 ? QDateTime::fromMSecsSinceEpoch(stamp).toString("yyyy年M月d日 HH:mm") : "";
        const int timeWidth = QFontMetrics(caption).horizontalAdvance(time);
        const int nameWidth = qMax(30, option.rect.width() - 70 - timeWidth - 36);
        p->drawText(QRect(70, 10, nameWidth, 21), Qt::AlignVCenter,
            QFontMetrics(caption).elidedText(message["sender"].toString(), Qt::ElideRight, nameWidth));
        p->drawText(QRect(option.rect.width() - timeWidth - 24, 10, timeWidth, 21), Qt::AlignVCenter, time);
        p->setFont(wechatDisplayFont(option.font));
        p->setPen(message["status"] == "recalled" ? historyColor("#9f9fa6", "#999ca3")
                                                : historyColor("#19191a", "#ededed"));
        p->drawText(layout.text, Qt::TextWordWrap, recordText(message));
        const auto media = message["media"].toArray();
        for (int i = 0; i < layout.media.size(); ++i)
        {
            const auto item = media[i].toObject();
            const auto rect = layout.media[i];
            if (item["kind"] == "video")
            {
                paintVideoPreview(p, rect, item, option.font);
                continue;
            }
            if (!thumbnailSize(item).isEmpty())
            {
                const auto path = item["_local"].toString();
                    const auto key = "records-image:" + path + ':' + item["id"].toString() + ':' +
                        item["sha256"].toString() + ':' + QString::number(rect.width()) + ':' +
                    QString::number(rect.height());
                QPixmap pixmap;
                if (!QPixmapCache::find(key, &pixmap))
                {
                    QImageReader reader(path);
                    reader.setAutoTransform(true);
                    reader.setScaledSize(rect.size());
                    pixmap = QPixmap::fromImage(reader.read());
                    if (!pixmap.isNull())
                        QPixmapCache::insert(key, pixmap);
                }
                if (!pixmap.isNull())
                {
                    p->drawPixmap(rect.topLeft(), pixmap);
                    continue;
                }
            }
            p->setPen(Qt::NoPen);
            p->setBrush(historyColor("#eeeef0", "#303234"));
            p->drawRoundedRect(rect, 4, 4);
            p->setPen(historyColor("#19191a", "#ededed"));
            p->drawText(rect.adjusted(10, 5, -10, -22), Qt::AlignVCenter,
                QFontMetrics(option.font).elidedText(mediaLabel(item), Qt::ElideRight, rect.width() - 20));
            p->setFont(caption);
            p->setPen(historyColor("#9f9fa6", "#999ca3"));
            p->drawText(rect.adjusted(10, 29, -10, -4), Qt::AlignVCenter,
                item["kind"] == "voice" ? "定位到聊天播放" : AttachmentStore::sizeLabel(item["size"].toInteger()));
            p->setFont(option.font);
        }
        p->restore();
    }
};
}

HistoryBrowser::HistoryBrowser(Transport *service, const QString &group, const QString &name,
                               const QString &directory, bool canDownload, QWidget *parent,
                               std::shared_ptr<MediaStorage::LocalFiles> files)
    : QDialog(parent, Qt::Window), localFiles(files ? std::move(files) : std::make_shared<MediaStorage::LocalFiles>()),
      transport(service), groupId(group), downloadRoot(directory),
      allowDownload(canDownload)
{
    setObjectName("historyBrowser");
    setWindowTitle("“" + name + "”的聊天记录");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(560, 720);
    setMinimumSize(480, 420);
    updateThemeStyle();
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 18, 0, 10);
    outer->setSpacing(10);
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(24, 0, 24, 0);
    search = new QLineEdit;
    search->setObjectName("historySearch");
    search->setPlaceholderText("搜索聊天记录");
    search->setClearButtonEnabled(true);
    search->setMaxLength(256);
    searchRow->addWidget(search);
    outer->addLayout(searchRow);
    auto *filters = new QHBoxLayout;
    filters->setContentsMargins(24, 0, 24, 0);
    filters->setSpacing(22);
    auto *filterGroup = new QButtonGroup(this);
    const QList<QPair<QString, QString>> categories{{"全部", "all"}, {"文件", "file"},
        {"图片与视频", "media"}, {"链接", "link"}, {"语音", "voice"}};
    for (const auto &[label, key] : categories)
    {
        auto *filter = new QPushButton(label);
        filter->setObjectName("historyFilter");
        filter->setProperty("recordType", key);
        filter->setCheckable(true);
        filter->setChecked(key == "all");
        filter->setCursor(Qt::PointingHandCursor);
        filterGroup->addButton(filter);
        filters->addWidget(filter);
        connect(filter, &QPushButton::clicked, this, [this, key]
        {
            if (type != key)
            {
                type = key;
                resetSearch();
            }
        });
    }
    filters->addStretch();
    outer->addLayout(filters);
    auto *details = new QHBoxLayout;
    details->setContentsMargins(24, 0, 24, 0);
    dateEnabled = new QCheckBox("日期");
    dateEnabled->setObjectName("historyDateEnabled");
    date = new QDateEdit(QDate::currentDate());
    date->setObjectName("historyDate");
    date->setDisplayFormat("yyyy-MM-dd");
    date->setCalendarPopup(true);
    date->setEnabled(false);
    member = new QComboBox;
    member->setObjectName("historyMember");
    member->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    member->setMinimumContentsLength(8);
    member->setMaximumWidth(200);
    member->addItem("全部群成员", "");
    details->addWidget(dateEnabled);
    details->addWidget(date);
    details->addStretch();
    details->addWidget(member);
    outer->addLayout(details);
    list = new StableListView;
    list->setObjectName("historyRecords");
    records = new RecordsModel(this);
    list->setModel(records);
    list->setItemDelegate(new RecordDelegate(list));
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->setSpacing(0);
    list->installEventFilter(this);
    list->viewport()->installEventFilter(this);
    outer->addWidget(list, 1);
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(24, 0, 24, 0);
    state = new QLabel;
    state->setObjectName("historyStatus");
    state->setTextFormat(Qt::PlainText);
    state->setWordWrap(true);
    more = new QPushButton("加载更多");
    more->setObjectName("historyMore");
    footer->addWidget(state, 1);
    footer->addWidget(more);
    outer->addLayout(footer);
    searchTimer.setSingleShot(true);
    searchTimer.setInterval(280);
    thumbnailTimer.setSingleShot(true);
    thumbnailTimer.setInterval(120);
    connect(&searchTimer, &QTimer::timeout, this, &HistoryBrowser::loadPage);
    connect(search, &QLineEdit::textChanged, this, &HistoryBrowser::resetSearch);
    connect(dateEnabled, &QCheckBox::toggled, this, [this](bool checked)
    {
        date->setEnabled(checked);
        resetSearch();
    });
    connect(date, &QDateEdit::dateChanged, this, [this] { if (dateEnabled->isChecked()) resetSearch(); });
    connect(member, &QComboBox::currentIndexChanged, this, &HistoryBrowser::resetSearch);
    connect(more, &QPushButton::clicked, this, &HistoryBrowser::loadPage);
    connect(&thumbnailTimer, &QTimer::timeout, this, &HistoryBrowser::loadVisibleThumbnails);
    connect(list->verticalScrollBar(), &QScrollBar::valueChanged, this,
        [this] { updateCachePins(); thumbnailTimer.start(); });
    connect(records, &QAbstractItemModel::dataChanged, this, &HistoryBrowser::updateCachePins);
    connect(records, &QAbstractItemModel::rowsInserted, this, &HistoryBrowser::updateCachePins);
    connect(records, &QAbstractItemModel::modelReset, this, &HistoryBrowser::updateCachePins);
    loadMembers();
    QTimer::singleShot(0, this, &HistoryBrowser::loadPage);
}
HistoryBrowser::~HistoryBrowser() { ++*cacheRecoveryGeneration; localFiles->unpin(this); }

void HistoryBrowser::resetSearch()
{
    ++generation;
    ++*cacheRecoveryGeneration;
    loading = false;
    hasMore = true;
    nextBefore.clear();
    static_cast<RecordsModel *>(records)->clear();
    failedThumbnails.clear();
    state->setText("正在查找…");
    more->setEnabled(false);
    searchTimer.start();
}

void HistoryBrowser::loadPage()
{
    if (!valid || !transport || loading || !hasMore)
        return;
    searchTimer.stop();
    if (requestsInFlight >= 2)
    {
        searchQueued = true;
        state->setText("正在等待上一查询完成…");
        return;
    }
    searchQueued = false;
    ++requestsInFlight;
    loading = true;
    more->setEnabled(false);
    state->setText("正在读取聊天记录…");
    QUrlQuery query;
    query.addQueryItem("group", groupId);
    query.addQueryItem("limit", QString::number(pageSize));
    query.addQueryItem("type", type);
    if (!nextBefore.isEmpty())
        query.addQueryItem("before", nextBefore);
    if (!search->text().trimmed().isEmpty())
        query.addQueryItem("q", search->text().trimmed());
    if (!member->currentData().toString().isEmpty())
        query.addQueryItem("member", member->currentData().toString());
    if (dateEnabled->isChecked())
    {
        // Local calendar boundaries are sent as absolute times; server and client
        // clocks/time zones need not agree and DST days need not be 24 hours.
        query.addQueryItem("start", QString::number(date->date().startOfDay().toMSecsSinceEpoch()));
        query.addQueryItem("end", QString::number(date->date().addDays(1).startOfDay().toMSecsSinceEpoch()));
    }
    const auto requestedGeneration = generation;
    const auto cursor = nextBefore;
    QPointer<HistoryBrowser> self(this);
    auto applyPage = [self, requestedGeneration, cursor](const QJsonValue &value, const QString &error)
        {
            if (!self)
                return;
            --self->requestsInFlight;
            if (!self->valid || requestedGeneration != self->generation)
            {
                if (self->valid && self->searchQueued && !self->loading && !self->searchTimer.isActive())
                    self->searchTimer.start(0);
                return;
            }
            self->loading = false;
            const auto page = value.toObject();
            const auto entries = page["messages"].toArray();
            const auto next = page["next_before"].toString();
            const bool pending = page["pending"].toBool();
            if (!error.isEmpty() || !page["messages"].isArray() || entries.size() > pageSize || next.size() > 4096)
            {
                self->state->setText(error.isEmpty() ? "聊天记录响应无效，请重试" : "暂时无法读取聊天记录：" + error.left(160));
                self->more->setText("重试");
                self->more->setEnabled(true);
                return;
            }
            // A pending backend read may legitimately ask us to retry this cursor.
            // A normal non-progressing page must not offer an endless request loop.
            self->hasMore = pending || (page["has_more"].toBool() && !next.isEmpty() && next != cursor);
            if (!next.isEmpty())
                self->nextBefore = next;
            QJsonArray hydrated;
            for (const auto &entry : entries)
                hydrated.append(self->localFiles->hydrate(entry.toObject(), self->groupId));
            static_cast<RecordsModel *>(self->records)->append(hydrated, self->groupId);
            for (const auto &update : self->updates)
                static_cast<RecordsModel *>(self->records)->update(self->localFiles->hydrate(update, self->groupId));
            const auto count = self->records->rowCount();
            self->state->setText(pending ? "本机历史正在读取，可继续加载" :
                count == 0 ? (self->hasMore ? "本页没有匹配记录，可继续查找更早记录" : "没有匹配的聊天记录") :
                count == retainedRecords ? "当前保留最近加载的 200 条记录" : QString("已显示 %1 条记录").arg(count));
            self->more->setText(pending ? "继续读取" : entries.isEmpty() && self->hasMore ? "继续查找" : "加载更多");
            self->more->setEnabled(self->hasMore);
            self->thumbnailTimer.start();
        };
    transport->get("/api/records?" + query.toString(QUrl::FullyEncoded),
        [self, requestedGeneration, applyPage](const QJsonValue &value, const QString &error)
        {
            if (!self) return;
            if (!error.isEmpty() || !self->valid || requestedGeneration != self->generation)
            { applyPage(value, error); return; }
            const auto files = self->localFiles;
            const auto entries = value.toObject()["messages"].toArray();
            const auto root = self->downloadRoot;
            const auto group = self->groupId;
            const auto cancellation = self->cacheRecoveryGeneration;
            const auto expected = cancellation->load();
            QThreadPool::globalInstance()->start([files, entries, root, group, cancellation, expected, applyPage, value, error]
            {
                files->recover(entries, root, group, [cancellation, expected] { return cancellation->load() != expected; });
                QMetaObject::invokeMethod(qApp, [applyPage, value, error] { applyPage(value, error); });
            });
        }, 20000);
}

void HistoryBrowser::loadMembers()
{
    if (!transport)
        return;
    QPointer<HistoryBrowser> self(this);
    transport->get("/api/groups/" + QString::fromLatin1(QUrl::toPercentEncoding(groupId)) + "/members",
        [self](const QJsonValue &value, const QString &error)
        {
            if (!self || !self->valid || !error.isEmpty())
                return;
            const auto members = value.toArray();
            QSet<QString> seen;
            for (const auto &entry : members)
            {
                const auto object = entry.toObject();
                const auto id = object["id"].toString();
                if (id.isEmpty() || id.size() > 512 || seen.contains(id))
                    continue;
                seen.insert(id);
                self->member->addItem(QIcon(chatAvatar(object, 24)), object["name"].toString().left(160), id);
                if (seen.size() >= 4096)
                    break;
            }
        }, 15000);
}

void HistoryBrowser::loadVisibleThumbnails()
{
    if (!valid || !transport || !allowDownload || !isVisible() || isMinimized() || property("cacheMaintenance").toBool())
        return;
    const auto rows = static_cast<RecordsModel *>(records);
    for (int row = 0; row < rows->rows.size() && downloading.size() < 2; ++row)
    {
        if (!list->visualRect(records->index(row, 0)).intersects(list->viewport()->rect()))
            continue;
        const auto &message = rows->rows[row];
        if (message["status"] == "recalled")
            continue;
        for (const auto &value : message["media"].toArray())
        {
            const auto media = value.toObject();
            const auto id = media["id"].toString();
            const auto kind = media["kind"].toString();
            if (downloading.size() >= 2)
                break;
            if (kind == "video")
            {
                const auto known = localFiles->findPoster(groupId, media);
                if (!known.isEmpty()) { rows->localPoster(id, known, media); continue; }
                if (id.isEmpty() || downloading.contains(id) || failedThumbnails.contains(id)) continue;
                downloading.insert(id);
                QPointer<HistoryBrowser> self(this);
                const auto requestedGeneration = generation;
                const auto files = localFiles;
                const auto group = groupId;
                transport->downloadPoster(media, MediaStorage::cacheDirectory(),
                    [self, files, group, media, id, requestedGeneration](const QString &path, const QString &error)
                    {
                        if (!self) return;
                        self->downloading.remove(id);
                        if (!self->valid || self->generation != requestedGeneration) return;
                        if (error.isEmpty())
                        {
                            files->rememberPoster(group, media, path);
                            static_cast<RecordsModel *>(self->records)->localPoster(id, path, media);
                            self->updateCachePins();
                            self->list->viewport()->update();
                            emit self->cacheUpdated();
                        }
                        else
                        {
                            if (self->failedThumbnails.size() >= 256) self->failedThumbnails.erase(self->failedThumbnails.begin());
                            self->failedThumbnails.insert(id);
                        }
                        self->thumbnailTimer.start();
                    });
                continue;
            }
            const auto known = localFiles->find(groupId, media);
            if (!known.isEmpty())
            {
                rows->localMedia(id, known, media);
                continue;
            }
            if ((kind != "image" && kind != "gif") || id.isEmpty() || downloading.contains(id) ||
                failedThumbnails.contains(id) ||
                media["size"].toInteger() > 8 * 1024 * 1024)
                continue;
            downloading.insert(id);
            QPointer<HistoryBrowser> self(this);
            const auto requestedGeneration = generation;
            const auto files = localFiles;
            const auto group = groupId;
            transport->download(media, MediaStorage::cacheDirectory(),
                [self, files, group, media, id, requestedGeneration](const QString &path, const QString &error)
                {
                    if (error.isEmpty()) files->remember(group, media, path);
                    if (!self)
                        return;
                    self->downloading.remove(id);
                    if (!self->valid || self->generation != requestedGeneration)
                        return;
                    if (!error.isEmpty())
                    {
                        if (self->failedThumbnails.size() >= 256)
                            self->failedThumbnails.erase(self->failedThumbnails.begin());
                        self->failedThumbnails.insert(id);
                    }
                    else
                    {
                        static_cast<RecordsModel *>(self->records)->localMedia(id, path, media);
                        self->list->doItemsLayout();
                        emit self->cacheUpdated();
                    }
                    self->thumbnailTimer.start();
                });
        }
    }
}

QSet<QString> HistoryBrowser::protectedCachePaths() const
{
    QSet<QString> result;
    if (!valid || !isVisible()) return result;
    const auto rows = static_cast<RecordsModel *>(records);
    const auto bounds = list->viewport()->rect();
    const auto first = list->indexAt(QPoint(1, 0));
    for (int row = qMax(0, first.row()); row < rows->rows.size(); ++row)
    {
        const auto rect = list->visualRect(records->index(row));
        if (rect.top() > bounds.bottom()) break;
        if (rect.intersects(bounds))
            for (const auto &entry : rows->rows[row]["media"].toArray())
            {
                for (const auto &field : {"_local", "_poster"})
                {
                    const auto path = entry.toObject()[field].toString();
                    if (!path.isEmpty()) result.insert(QFileInfo(path).absoluteFilePath());
                }
            }
    }
    return result;
}
void HistoryBrowser::updateCachePins() { localFiles->pin(this, protectedCachePaths()); }
void HistoryBrowser::discardCached(const QStringList &paths)
{
    if (!valid) return;
    thumbnailTimer.start();
    if (paths.isEmpty()) return;
    auto *rows = static_cast<RecordsModel *>(records);
    QSet<QString> ids;
    QSet<QString> posterIds;
    for (const auto &row : rows->rows)
        for (const auto &entry : row["media"].toArray())
        {
            if (paths.contains(entry.toObject()["_local"].toString()))
                ids.insert(entry.toObject()["id"].toString());
            if (paths.contains(entry.toObject()["_poster"].toString()))
                posterIds.insert(entry.toObject()["id"].toString());
        }
    for (const auto &id : ids) rows->localMedia(id, {});
    for (const auto &id : posterIds) rows->localPoster(id, {});
}
void HistoryBrowser::activate(const QModelIndex &index, const QPoint &point)
{
    if (!valid || !index.isValid())
        return;
    const auto message = index.data(Qt::UserRole).toJsonObject();
    if (message["group"] != groupId)
        return;
    QJsonObject media;
    const auto layout = layoutRow(message, list->viewport()->width(), list->font());
    const auto relative = point - list->visualRect(index).topLeft();
    for (int i = 0; !point.isNull() && i < layout.media.size(); ++i)
        if (layout.media[i].contains(relative))
            media = message["media"].toArray()[i].toObject();
    if (!allowDownload && !media.isEmpty())
        media = {};
    emit recordActivated(message, media);
}

bool HistoryBrowser::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == list && event->type() == QEvent::KeyPress)
    {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            activate(list->currentIndex(), {});
            return true;
        }
    }
    if (watched == list->viewport())
    {
        if (event->type() == QEvent::MouseButtonRelease)
        {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton)
                activate(list->indexAt(mouse->position().toPoint()), mouse->position().toPoint());
        }
        else if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
        {
            updateCachePins();
            thumbnailTimer.start();
        }
        else if (event->type() == QEvent::Hide)
            localFiles->unpin(this);
    }
    return QDialog::eventFilter(watched, event);
}

void HistoryBrowser::updateMessage(const QJsonObject &message)
{
    const auto id = message["id"].toString();
    if (valid && message["group"] == groupId && !id.isEmpty() && id.size() <= 512)
    {
        if (!updates.contains(id))
            updateOrder.append(id);
        updates[id] = message;
        while (updateOrder.size() > retainedRecords)
            updates.remove(updateOrder.takeFirst());
        static_cast<RecordsModel *>(records)->update(localFiles->hydrate(message, groupId));
        list->doItemsLayout();
        thumbnailTimer.start();
    }
}

void HistoryBrowser::invalidate()
{
    valid = false;
    ++generation;
    ++*cacheRecoveryGeneration;
    searchTimer.stop();
    thumbnailTimer.stop();
    localFiles->unpin(this);
    close();
}

void HistoryBrowser::closeEvent(QCloseEvent *event)
{
    valid = false;
    ++generation;
    ++*cacheRecoveryGeneration;
    searchTimer.stop();
    thumbnailTimer.stop();
    localFiles->unpin(this);
    QDialog::closeEvent(event);
}

void HistoryBrowser::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        updateThemeStyle();
}

void HistoryBrowser::updateThemeStyle()
{
    if (updatingThemeStyle)
        return;
    const auto style = Theme::isDark() ? QStringLiteral(
        "#historyBrowser { background:#202020; }"
        "#historyRecords { background:#202020; border:0; outline:0; }"
        "#historySearch { background:#292929; border:1px solid #44474b; border-radius:6px; padding:10px; }"
        "#historyFilter { background:transparent; color:#75bfff; border:0; padding:3px 0; font-size:12px; }"
        "#historyFilter:checked { color:#49d49a; }"
        "#historyStatus { color:#999ca3; font-size:12px; }"
        "#historyMore { background:transparent; color:#75bfff; border:0; padding:5px; }"
        "QComboBox, QDateEdit { background:#292929; border:1px solid #44474b; border-radius:4px; padding:3px; }"
        "QCheckBox { font-size:12px; }") : QStringLiteral(
        "#historyBrowser { background:#fafafa; }"
        "#historyRecords { background:#fafafa; border:0; outline:0; }"
        "#historySearch { background:#fafafa; border:1px solid #e3e3e3; border-radius:6px; padding:10px; }"
        "#historyFilter { background:transparent; color:#334e88; border:0; padding:3px 0; font-size:12px; }"
        "#historyFilter:checked { color:#07a569; }"
        "#historyStatus { color:#9f9fa6; font-size:12px; }"
        "#historyMore { background:transparent; color:#334e88; border:0; padding:5px; }"
        "QComboBox, QDateEdit { background:#fafafa; border:1px solid #e3e3e3; border-radius:4px; padding:3px; }"
        "QCheckBox { font-size:12px; }");
    if (style == styleSheet())
        return;
    updatingThemeStyle = true;
    setStyleSheet(style);
    updatingThemeStyle = false;
}
