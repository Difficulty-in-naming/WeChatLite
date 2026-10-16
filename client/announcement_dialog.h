#pragma once
#include <QDialog>
#include <QPointer>

class Transport;
class QLabel;
class QPushButton;
class QTextBrowser;

class AnnouncementDialog : public QDialog
{
    Q_OBJECT
  public:
    AnnouncementDialog(Transport *transport, const QString &group, const QString &name,
                       QWidget *parent = nullptr);
    QString group() const { return groupId; }
    void invalidate();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

  private:
    void reload();
    QPointer<Transport> transport;
    QString groupId;
    QLabel *avatar, *author, *updated, *state;
    QTextBrowser *body;
    QPushButton *retry;
    quint64 generation = 0;
    bool valid = true;
};
