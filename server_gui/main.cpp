#include "server_window.h"
#include "autostart.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    QCoreApplication::setOrganizationName("WeChatLite");
    QCoreApplication::setApplicationName("WeChatLiteServer");
    QCoreApplication::setApplicationVersion(QStringLiteral(WECHAT_LITE_VERSION));
    const auto profile = qEnvironmentVariable("WECHAT_LITE_PROFILE");
    if (!profile.isEmpty())
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile);
    }
    QFont font;
#ifdef Q_OS_WIN
    font.setFamily("Microsoft YaHei UI");
#else
    font.setFamily("PingFang SC");
#endif
    font.setPixelSize(14);
    app.setFont(font);
    app.setStyleSheet("QMainWindow,QDialog {background:#f7f8f8;} QPushButton {padding:7px 13px;} "
        "QLineEdit,QComboBox {padding:5px;} #serverStart {background:#08ae71;color:white;border:0;border-radius:4px;} "
        "#serverStart:disabled {background:#cdd7d2;} QTabWidget::pane {border:1px solid #dedede;} "
        "QListWidget {border:1px solid #dedede;border-radius:4px;} QLabel {color:#242827;}");
    QCommandLineParser parser;
    parser.setApplicationDescription("微信轻聊独立转发服务端：本机微信、群范围与客户端权限管理");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"config", "服务端配置文件", "path"});
    parser.addOption({"start", "打开后启动转发服务"});
    parser.addOption({"demo", "演示服务，不访问真实微信"});
    parser.addOption({"screenshot", "保存服务端窗口截图后退出（仅演示模式）", "path"});
    parser.process(app);
    if (parser.isSet("screenshot") && !parser.isSet("demo"))
        parser.showHelp(2);
    QString directory = profile.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : profile + "/server";
    if (parser.isSet("demo"))
        directory += "/demo";
    QString path = parser.isSet("config") ? parser.value("config") : directory + "/server.json";
    path = QFileInfo(path).absoluteFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QLockFile lock(path + ".gui.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock())
    {
        QMessageBox::information(nullptr, "转发服务端已打开", "此配置的服务端管理窗口已经在运行，请从系统托盘打开。");
        return 1;
    }
    ServerWindow window(path, parser.isSet("demo"));
    if (!parser.isSet("demo"))
    {
        QString error;
        ServerAutostart::currentUser(path).refreshIfEnabled(error);
    }
    window.show();
    if (parser.isSet("start"))
        QTimer::singleShot(0, &window, &ServerWindow::startService);
    if (parser.isSet("screenshot"))
    {
        const auto target = parser.value("screenshot");
        QTimer::singleShot(6500, &window, [&window, target]
        {
            QDir().mkpath(QFileInfo(target).absolutePath());
            window.grab().save(target);
            QApplication::quit();
        });
    }
    return app.exec();
}
