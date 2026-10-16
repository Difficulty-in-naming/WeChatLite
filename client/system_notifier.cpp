#include "system_notifier.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QSettings>
#include <QThread>

namespace
{
bool nativeArgument(const QString &argument)
{
    return argument == "--notification-activation" ||
           argument.compare("-Embedding", Qt::CaseInsensitive) == 0 ||
           argument.compare("/Embedding", Qt::CaseInsensitive) == 0;
}
} // namespace

SystemNotifier::SystemNotifier(QObject *parent) : QObject(parent)
{
    if (!disabled())
        native = createNative(this);
}

SystemNotifier::~SystemNotifier() = default;

QStringList SystemNotifier::applicationArguments()
{
    auto arguments = QCoreApplication::arguments();
    for (qsizetype index = arguments.size() - 1; index > 0; --index)
        if (nativeArgument(arguments[index]))
            arguments.removeAt(index);
    return arguments;
}

bool SystemNotifier::activationLaunch()
{
    const auto arguments = QCoreApplication::arguments();
    for (qsizetype index = 1; index < arguments.size(); ++index)
        if (nativeArgument(arguments[index]))
            return true;
    return false;
}

bool SystemNotifier::disabled()
{
    return QStandardPaths::isTestModeEnabled() ||
           qEnvironmentVariable("WECHAT_LITE_DISABLE_SYSTEM_NOTIFICATIONS") == "1" ||
           QGuiApplication::platformName() == "offscreen" || QGuiApplication::platformName() == "minimal";
}

bool SystemNotifier::validGroup(const QString &groupId)
{
    if (groupId.isEmpty() || groupId.size() > 1024)
        return false;
    for (const auto character : groupId)
        if (character.isNull() || character.category() == QChar::Other_Control)
            return false;
    return true;
}

QString SystemNotifier::limitedText(const QString &text, qsizetype limit)
{
    QString result;
    result.reserve(qMin(text.size(), limit));
    for (const auto character : text)
    {
        if (result.size() == limit)
            break;
        if (character.isNull())
            continue;
        result += character.category() == QChar::Other_Control ? QChar(' ') : character;
    }
    if (!result.isEmpty() && result.back().isHighSurrogate())
        result.chop(1);
    if (text.size() > limit)
        result += QChar(0x2026);
    return result;
}

void SystemNotifier::showMessage(const QString &groupId, const QString &title, const QString &body)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!validGroup(groupId))
        return;
    const auto heading = limitedText(title.isEmpty() ? QStringLiteral("微信轻聊") : title, 128);
    const auto message = limitedText(body, 512);
    emit requested(groupId, heading, message);
    const auto sound = soundEnabled();
    emit soundRequested(sound);
    if (!disabled() && native)
        native->show(groupId, heading, message, sound);
}

bool SystemNotifier::soundEnabled() { return QSettings().value("notificationSound", true).toBool(); }

void SystemNotifier::setUnreadTotal(int count)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto bounded = qBound(0, count, 9999);
    if (bounded == unreadTotal)
        return;
    unreadTotal = bounded;
    emit badgeChanged(bounded);
    if (!disabled() && native)
        native->setBadge(bounded);
}

bool SystemNotifier::isActivationRelay() const { return native && native->isActivationRelay(); }

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
std::unique_ptr<SystemNotifier::Native> SystemNotifier::createNative(SystemNotifier *) { return {}; }
void SystemNotifier::previewSound() {}
#endif
