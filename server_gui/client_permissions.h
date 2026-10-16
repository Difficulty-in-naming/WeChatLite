#pragma once
#include "transport.h"
#include <QJsonArray>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class ClientPermissions : public QWidget
{
    Q_OBJECT
  public:
    explicit ClientPermissions(const QString &configPath, QWidget *parent = nullptr);
    void refresh();

  private:
    void selectClient();
    void newClient();
    void apply();
    void revoke();
    void setWorking(bool value);
    void showCredential(const QString &name, const QString &token, const QJsonObject &p2p = {});
    QString configPath, selectedId;
    Transport transport;
    QJsonArray catalog;
    bool working = false, available = false;
    QListWidget *list, *groups;
    QLineEdit *name;
    QCheckBox *enabled, *canSend, *canDownload;
    QLabel *status;
    QWidget *editor;
    QPushButton *refreshButton, *newButton, *applyButton, *revokeButton;
};
