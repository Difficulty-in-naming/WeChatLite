#include "autostart.h"
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace
{
const QString entryName = QStringLiteral("WeChatLiteServer");
const QString agentLabel = QStringLiteral("local.wechatlite.server");

bool ownedArguments(const QStringList &arguments)
{
    const auto file = QFileInfo(arguments.value(0)).fileName();
    return QDir::isAbsolutePath(arguments.value(0)) &&
        (file == "wechat-server.exe" || file == "wechat-server") &&
        ((arguments.size() == 2 && arguments.at(1) == "--start") ||
            (arguments.size() == 4 && arguments.at(1) == "--config" &&
                QDir::isAbsolutePath(arguments.at(2)) && arguments.at(3) == "--start"));
}
}

ServerAutostart::ServerAutostart(Platform platform, QString location, QString executable, QString configPath)
    : platform(platform), location(std::move(location)), executable(std::move(executable)),
      configPath(std::move(configPath))
{
}

ServerAutostart ServerAutostart::currentUser(const QString &configPath)
{
#ifdef Q_OS_WIN
    return {Platform::Windows, "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        QCoreApplication::applicationFilePath(), configPath};
#elif defined(Q_OS_MACOS)
    return {Platform::MacOS, QDir::homePath() + "/Library/LaunchAgents/" + agentLabel + ".plist",
        QCoreApplication::applicationFilePath(), configPath};
#else
    return {Platform::Unsupported, {}, QCoreApplication::applicationFilePath(), configPath};
#endif
}

QStringList ServerAutostart::arguments() const
{
    const auto defaultConfig = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                   .filePath("server.json");
    // The default path is implicit, keeping Windows Run commands below its 260-character limit.
    const auto program = platform == Platform::Windows ? QDir::toNativeSeparators(executable) : executable;
    const auto config = platform == Platform::Windows ? QDir::toNativeSeparators(configPath) : configPath;
    if (QDir::cleanPath(configPath) == QDir::cleanPath(defaultConfig))
        return {program, "--start"};
    return {program, "--config", config, "--start"};
}

bool ServerAutostart::readArguments(QStringList &arguments, QString &error) const
{
    arguments.clear();
    error.clear();
    if (platform == Platform::Windows)
    {
#ifdef Q_OS_WIN
        QSettings settings(location, QSettings::NativeFormat);
        const auto command = settings.value(entryName).toString();
        if (settings.status() != QSettings::NoError)
        {
            error = "无法读取开机自启设置。";
            return false;
        }
        if (!command.isEmpty()) arguments = QProcess::splitCommand(command);
#else
        error = "当前系统不支持 Windows 启动项。";
        return false;
#endif
    }
    else if (platform == Platform::MacOS)
    {
        const QFileInfo info(location);
        if (!info.exists() && !info.isSymLink()) return true;
        QFile file(location);
        if (info.isSymLink() || !info.isFile() || info.size() > 64 * 1024 ||
            !file.open(QIODevice::ReadOnly))
        {
            error = "无法读取本应用的开机自启文件。";
            return false;
        }
        QXmlStreamReader xml(file.readAll());
        QString label;
        bool runAtLoad = false;
        if (xml.readNextStartElement() && xml.name() == u"plist" &&
            xml.readNextStartElement() && xml.name() == u"dict")
            while (xml.readNextStartElement())
            {
                if (xml.name() != u"key") { xml.raiseError("Expected key"); break; }
                const auto key = xml.readElementText();
                if (!xml.readNextStartElement()) break;
                if (key == "Label" && xml.name() == u"string") label = xml.readElementText();
                else if (key == "ProgramArguments" && xml.name() == u"array")
                    while (xml.readNextStartElement())
                    {
                        if (xml.name() != u"string") { xml.raiseError("Expected argument"); break; }
                        arguments.append(xml.readElementText());
                    }
                else if (key == "RunAtLoad")
                {
                    runAtLoad = xml.name() == u"true";
                    xml.skipCurrentElement();
                }
                else xml.skipCurrentElement();
            }
        while (!xml.atEnd()) xml.readNext();
        if (xml.hasError() || label != agentLabel || !runAtLoad || arguments.isEmpty())
        {
            error = "开机自启文件不是本应用的有效启动项，未修改。";
            return false;
        }
    }
    else
    {
        error = "当前系统暂不支持开机自启。";
        return false;
    }
    if (!arguments.isEmpty() && !ownedArguments(arguments))
    {
        error = "启动项内容已被其他程序修改，未修改。";
        return false;
    }
    return true;
}

bool ServerAutostart::read(bool &enabled, QString &error) const
{
    QStringList stored;
    const bool success = readArguments(stored, error);
    enabled = success && !stored.isEmpty();
    if (enabled && stored != arguments())
        error = "已开启，但启动项仍指向其他版本或配置；请关闭后重新开启以更新路径。";
    return success;
}

bool ServerAutostart::setEnabled(bool enabled, QString &error) const
{
    QStringList stored;
    if (!readArguments(stored, error)) return false;
    if (!enabled && stored.isEmpty()) return true;
    const auto desired = arguments();
    if (enabled && (!QFileInfo(executable).isFile() || !QDir::isAbsolutePath(executable) ||
            !QDir::isAbsolutePath(configPath) || !ownedArguments(desired)))
    {
        error = "当前程序或配置路径无效，无法设置开机自启。";
        return false;
    }
    if (platform == Platform::Windows)
    {
#ifdef Q_OS_WIN
        QStringList quoted;
        for (const auto &argument : desired)
        {
            if (argument.contains('"') || argument.contains('\n') || argument.contains('\r'))
            {
                error = "路径包含不支持的字符，无法设置开机自启。";
                return false;
            }
            quoted.append('"' + QDir::toNativeSeparators(argument) + '"');
        }
        const auto command = quoted.join(' ');
        if (enabled && command.size() > 260)
        {
            error = "启动命令路径过长，请将程序和配置移到较短路径后重试。";
            return false;
        }
        QSettings settings(location, QSettings::NativeFormat);
        if (enabled) settings.setValue(entryName, command);
        else settings.remove(entryName);
        settings.sync();
        if (settings.status() != QSettings::NoError)
        {
            error = "无法保存开机自启设置，请检查当前用户权限。";
            return false;
        }
#endif
    }
    else if (enabled)
    {
        if (executable.startsWith("/Volumes/") || executable.contains("/AppTranslocation/"))
        {
            error = "请先将服务端 app 复制到应用程序目录，再开启开机自启。";
            return false;
        }
        if (!QDir().mkpath(QFileInfo(location).absolutePath()))
        {
            error = "无法创建当前用户的启动项目录。";
            return false;
        }
        QSaveFile file(location);
        if (!file.open(QIODevice::WriteOnly))
        {
            error = "无法写入开机自启文件，请检查当前用户权限。";
            return false;
        }
        QXmlStreamWriter xml(&file);
        xml.setAutoFormatting(true);
        xml.writeStartDocument();
        xml.writeDTD("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">");
        xml.writeStartElement("plist");
        xml.writeAttribute("version", "1.0");
        xml.writeStartElement("dict");
        xml.writeTextElement("key", "Label");
        xml.writeTextElement("string", agentLabel);
        xml.writeTextElement("key", "ProgramArguments");
        xml.writeStartElement("array");
        for (const auto &argument : desired) xml.writeTextElement("string", argument);
        xml.writeEndElement();
        xml.writeTextElement("key", "RunAtLoad");
        xml.writeEmptyElement("true");
        xml.writeEndElement();
        xml.writeEndElement();
        xml.writeEndDocument();
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (xml.hasError() || !file.commit())
        {
            error = "无法保存开机自启文件，原设置已保留。";
            return false;
        }
    }
    else if (!QFile::remove(location))
    {
        error = "无法删除本应用的开机自启文件。";
        return false;
    }
    bool actual = false;
    if (!read(actual, error)) return false;
    if (actual != enabled)
    {
        error = "开机自启设置未能生效，请重试。";
        return false;
    }
    return true;
}

bool ServerAutostart::refreshIfEnabled(QString &error) const
{
    QStringList stored;
    if (!readArguments(stored, error)) return false;
    return stored.isEmpty() || stored == arguments() || setEnabled(true, error);
}

AutostartSettings::AutostartSettings(const ServerAutostart &registration, QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *enabled = new QCheckBox("开机自启转发服务端");
    enabled->setObjectName("serverAutostartEnabled");
    auto *description = new QLabel("登录当前系统用户后，自动打开服务端并启动转发服务。\n微信仍需要在这台电脑上登录。");
    description->setWordWrap(true);
    auto *status = new QLabel;
    status->setObjectName("serverAutostartStatus");
    status->setWordWrap(true);
    bool current = false;
    QString error;
    registration.read(current, error);
    enabled->setChecked(current);
    status->setText(error.isEmpty() ? (current ? "已开启，下次登录时生效。" : "未开启。") : error);
    layout->addWidget(enabled);
    layout->addWidget(description);
    layout->addWidget(status);
    layout->addStretch();
    connect(enabled, &QCheckBox::toggled, this, [registration, enabled, status](bool desired)
    {
        QString error;
        const bool saved = registration.setEnabled(desired, error);
        bool actual = false;
        QString readError;
        registration.read(actual, readError);
        const QSignalBlocker blocker(enabled);
        enabled->setChecked(actual);
        status->setText(saved ? (actual ? "已开启，下次登录时生效。" : "未开启。") : error);
    });
}
