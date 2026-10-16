#pragma once
#include "release_update.h"
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;

class UpdateWidget final : public QWidget
{
    Q_OBJECT
  public:
    explicit UpdateWidget(ReleaseUpdate::Role role, QWidget *parent = nullptr);
  private:
    void check();
    void bind(ReleaseUpdate *service);
    void refresh();
    ReleaseUpdate::Role role;
    ReleaseUpdate *updater = nullptr;
    QLabel *status;
    QProgressBar *progress;
    QPushButton *checkButton, *cancelButton, *folderButton;
};
