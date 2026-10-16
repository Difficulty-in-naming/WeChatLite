#pragma once
#include <QObject>
#include <QStringList>
#include <memory>

class WindowsNotificationBackend;
class MacNotificationBackend;

class SystemNotifier final : public QObject
{
    Q_OBJECT
  public:
    explicit SystemNotifier(QObject *parent = nullptr);
    ~SystemNotifier() override;
    void showMessage(const QString &groupId, const QString &title, const QString &body);
    // Filter the native COM-only arguments before handing argv to QCommandLineParser.
    static QStringList applicationArguments();
    // A cold activation forwards to the existing client. Do not construct another Window/helper.
    bool isActivationRelay() const;

  signals:
    void activated(const QString &groupId);
    // Observable intent; test mode emits this without calling any platform API or requesting permission.
    void requested(const QString &groupId, const QString &title, const QString &body);

  private:
    struct Native
    {
        virtual ~Native() = default;
        virtual void show(const QString &groupId, const QString &title, const QString &body) = 0;
        virtual bool isActivationRelay() const { return false; }
    };
    friend class WindowsNotificationBackend;
    friend class MacNotificationBackend;
    static std::unique_ptr<Native> createNative(SystemNotifier *owner);
    static bool activationLaunch();
    static bool disabled();
    static bool validGroup(const QString &groupId);
    static QString limitedText(const QString &text, qsizetype limit);
    std::unique_ptr<Native> native;
};
