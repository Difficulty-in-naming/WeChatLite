#pragma once
#include "transport.h"
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QPlainTextEdit;

class ForwardingSettings : public QWidget
{
    Q_OBJECT
  public:
    explicit ForwardingSettings(const QString &configPath, QWidget *parent = nullptr);
    void refresh();

  private:
    void apply();
    void updateCount();
    QString configPath;
    Transport transport;
    QLineEdit *search;
    QPlainTextEdit *patterns;
    QListWidget *groups;
    QLabel *status, *count;
    QPushButton *refreshButton, *applyButton;
    bool available = false, working = false;
};

bool configureLocalAdmin(Transport &transport, const QString &configPath, QString &error);
bool editLocalService(QWidget *parent, const QString &configPath, bool showAutomatic = false);
