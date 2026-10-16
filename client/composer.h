#pragma once
#include <QJsonArray>
#include <QTextEdit>
class QListWidget;
class Composer : public QTextEdit
{
    Q_OBJECT
  public:
    explicit Composer(QWidget *parent = nullptr);
    void setMembers(const QJsonArray &members);
    void insertMention(const QJsonObject &member);
    QJsonArray mentions() const;
    void restore(const QString &text, const QJsonArray &mentions);
    bool popupVisible() const;
  signals:
    void filesAttached(const QStringList &paths);
    void imageAttached(const QMimeData *mime);
    void sendRequested();

  protected:
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

  private:
    void updatePopup();
    void choose(int row);
    bool isComposing() const;
    QJsonArray members;
    QListWidget *popup;
    int mentionStart = -1;
    bool choosing = false;
};
