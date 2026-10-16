#include "window.h"
#include "system_notifier.h"
#include "theme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPixmapCache>
#include <QSettings>
#include <QTimer>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("WeChatLite");
    QCoreApplication::setApplicationName("WeChatLite");
    QCoreApplication::setApplicationVersion(QStringLiteral(WECHAT_LITE_VERSION));
    const auto profile = qEnvironmentVariable("WECHAT_LITE_PROFILE");
    if (!profile.isEmpty())
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile);
    }
    QImageReader::setAllocationLimit(48);
    QPixmapCache::setCacheLimit(16 * 1024);
    QFont font;
#ifdef Q_OS_WIN
    font.setFamily("Microsoft YaHei UI");
    font.setPixelSize(14);
#else
    font.setFamily("PingFang SC");
    font.setPixelSize(14);
#endif
    app.setFont(font);
    Theme::initialize(QSettings().value("themeMode", "system").toString());
    QCommandLineParser parser;
    parser.setApplicationDescription("微信轻聊客户端 · 连接独立部署的服务器");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"demo", "启动本机演示服务，不连接微信"});
    parser.addOption({"screenshot", "保存界面截图后退出", "path"});
    parser.process(SystemNotifier::applicationArguments());
    SystemNotifier notifier;
    if (notifier.isActivationRelay())
        return app.exec();
    Window window(parser.isSet("demo"));
    QObject::connect(&window, &Window::systemNotificationRequested, &notifier, &SystemNotifier::showMessage);
    QObject::connect(&window, &Window::notificationSoundPreviewRequested, &notifier, &SystemNotifier::previewSound);
    QObject::connect(&notifier, &SystemNotifier::activated, &window, &Window::activateConversation);
    window.show();
    if (!parser.isSet("demo") && (QSettings().value("serverUrl").toString().trimmed().isEmpty() ||
                                  QSettings().value("token").toString().trimmed().size() < 24))
        QTimer::singleShot(0, &window, &Window::showSettings);
    if (parser.isSet("screenshot"))
    {
        auto path = parser.value("screenshot");
        QTimer::singleShot(4500, &window,
                           [&window, path]
                           {
                               QDir().mkpath(QFileInfo(path).absolutePath());
                               window.grab().save(path);
                               QCoreApplication::quit();
                           });
    }
    return app.exec();
}
