#pragma once
#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class PreferencesDialog : public QDialog
{
    Q_OBJECT
  public:
    struct Values
    {
        QString downloads;
        QString theme = "system";
        bool notificationSound = true;
        qint64 cacheLimitBytes = 512LL * 1024 * 1024;
    };
    explicit PreferencesDialog(const Values &initial, QWidget *parent = nullptr);
    Values values() const;

  public slots:
    void refreshUsage();
    void setUsage(quint64 requestId, qint64 downloadsBytes, qint64 cacheBytes,
                  const QString &error = {});
    void cacheCleared(quint64 requestId, const QString &error = {});
    void accept() override;

  signals:
    void usageRequested(const QString &downloads, quint64 requestId);
    // The path identifies the displayed statistics. Only the application's fixed
    // preview/voice cache is eligible for deletion; this is never a downloads purge.
    void clearCacheRequested(const QString &downloads, quint64 requestId);
    void previewSoundRequested();
    void applied();

  private:
    void chooseDirectory();
    void clearCache();
    void updateUsageState();
    QLineEdit *directory;
    QComboBox *theme, *cacheLimit;
    QCheckBox *sound;
    QLabel *downloadsUsage, *cacheUsage, *usageStatus, *validation;
    QPushButton *choose, *refresh, *clear, *preview;
    quint64 sequence = 0, usageRequest = 0, clearRequest = 0;
    bool scanning = false, clearing = false;
    QString actionFeedback;
};
