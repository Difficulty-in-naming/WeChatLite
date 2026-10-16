#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QObject>

struct Attachment
{
    QString path, name, kind, uploaded;
    qint64 size = 0;
    QDateTime modified;
    bool temporary = false;
};

class AttachmentStore : public QObject
{
    Q_OBJECT
  public:
    explicit AttachmentStore(QObject *parent = nullptr);
    QList<Attachment> items;
    void addFiles(const QStringList &paths);
    bool addImage(const class QMimeData *mime);
    void remove(int index);
    void clear(bool discard = true);
    QJsonArray serialize() const;
    void restore(const QJsonArray &array);
    QString validate() const;
    void setLocked(bool value) { locked = value; }
    static QString kindFor(const QString &path);
    static QString safeName(const QString &name);
    static QString sizeLabel(qint64 bytes);
  signals:
    void changed();
    void warning(const QString &message);

  private:
    bool locked = false;
};
