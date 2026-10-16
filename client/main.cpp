#include "window.h"
#include "system_notifier.h"
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
    QCoreApplication::setApplicationVersion("0.3.0");
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
    app.setStyleSheet(R"(
        QWidget { color:#19191a; }
        QMainWindow, QDialog { background:#fafafa; }
        QLabel { background:transparent; }
        #titleBar { background:#e0e0e0; }
        #sidebar, #groups { background:#eeeef0; border:0; outline:0; }
        #main, #messages, #composeArea { background:#fafafa; border:0; }
        #composeFrame { background:#fafafa; border:1px solid #e3e3e3; border-radius:9px; }
        #chatTitle { font-size:14px; font-weight:400; color:#19191a; }
        #muted { color:#9f9fa6; font-size:12px; }
        #footnote { color:#9f9fa6; font-size:11px; }
        #demoLabel { color:#8a8a8a; font-size:11px; }
        #divider { background:#f1f1f1; border:0; }
        #composer { background:transparent; border:0; font-size:14px; selection-background-color:#b7d6f1; padding:0; }
        QLineEdit { border:1px solid #dedede; border-radius:5px; background:#fafafa; padding:6px 9px; selection-background-color:#b7d6f1; }
        QLineEdit:focus { border:1px solid #bcbcbc; }
        #search { background:#fafafa; border:0; padding:0 8px; color:#333333; font-size:12px; }
        QPushButton { background:#f3f3f3; border:1px solid #dedede; border-radius:4px; padding:6px 12px; }
        QPushButton:hover { background:#e9e9e9; }
        QPushButton:pressed { background:#dedede; }
        QPushButton:disabled { color:#b0b0b0; background:#efefef; border-color:#e8e8e8; }
        #sendButton { background:#e9e9e9; color:#07a569; border:0; font-size:12px; padding:0; }
        #sendButton:hover { background:#dedede; }
        #sendButton:disabled { background:#ededed; color:#b0b0b0; }
        #iconButton, #attachButton, #windowButton, #closeWindow { background:transparent; border:0; border-radius:3px; padding:0; }
        #iconButton:hover, #attachButton:hover { background:#e5e5e5; }
        #windowButton:hover { background:#d4d4d4; }
        #closeWindow:hover { background:#e55555; }
        #textButton { background:transparent; border:0; color:#9f9fa6; padding:2px 0; font-size:11px; }
        #attachmentCard { background:#ffffff; border:1px solid #dedede; border-radius:5px; }
        #fileType { color:#e49338; font-size:16px; padding:0 5px; }
        #removeAttachment { background:transparent; border:0; color:#999999; padding:0; font-size:18px; }
        #mentionPopup { font-size:14px; background:#ededed; border:1px solid #d3d3d3; border-radius:6px; padding:3px; }
        #mentionPopup::item { border-radius:3px; color:#19191a; padding-left:1px; }
        #mentionPopup::item:selected { background:#d4d4d4; color:#19191a; }
        QScrollBar:vertical { background:transparent; width:6px; margin:2px 0; }
        QScrollBar::handle:vertical { background:#d2d2d2; border-radius:3px; min-height:24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
        QScrollBar:horizontal { height:6px; background:transparent; }
        QScrollBar::handle:horizontal { background:#d2d2d2; border-radius:3px; min-width:24px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width:0; }
        QSplitter::handle { background:#dedede; }
    )");
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
