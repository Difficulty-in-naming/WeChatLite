#include "tunnel_session.h"
#include "connection_info.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

TunnelSession::TunnelSession(QObject *parent, const QString &executable) : QObject(parent), program(executable)
{
    if (program.isEmpty())
    {
        program = QCoreApplication::applicationDirPath() + "/wechat-link";
#ifdef Q_OS_WIN
        program += ".exe";
#endif
    }
    startup.setSingleShot(true);
    connect(&startup, &QTimer::timeout, this, [this] { fail("自动连接启动超时，请检查公网入口和网络。"); });
}

TunnelSession::~TunnelSession() { stop(); }

void TunnelSession::start(const QJsonObject &settings)
{
    stop();
    QString error;
    if (!ConnectionInfo::relaySettings(settings, error, true))
    {
        emit failed(error);
        return;
    }
    if (!QFileInfo(program).isFile())
    {
        emit failed("找不到自动连接组件 wechat-link，请保留完整客户端文件夹。");
        return;
    }
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(directory))
    {
        emit failed("无法创建客户端设备身份目录。");
        return;
    }
    configuration = settings;
    auto *child = new QProcess(this);
    process = child;
    const auto expected = generation;
#ifdef Q_OS_WIN
    child->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments)
                                            { arguments->flags |= 0x08000000; });
#endif
    child->setProgram(program);
    child->setArguments({"--client"});
    child->setProcessChannelMode(QProcess::SeparateChannels);
    connect(child, &QProcess::started, this, [this, child, expected, directory]
    {
        if (process != child || generation != expected)
            return;
        QJsonObject input;
        for (const auto &key : {"server_id", "relay_url", "relay_token", "relay_ca", "relay_quic_port"})
            input.insert(key, configuration.value(key));
        input.insert("key_file", directory + "/p2p-client.key");
        const auto bytes = QJsonDocument(input).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() > ConnectionInfo::maximumBytes || child->write(bytes) != bytes.size())
            fail("无法初始化自动连接组件。");
        // The write channel stays open. EOF is the helper's ownership/exit signal.
    });
    connect(child, &QProcess::readyReadStandardOutput, this,
            [this, child, expected] { readOutput(child, expected); });
    connect(child, &QProcess::readyReadStandardError, this, [child]
    {
        // Helper diagnostics may contain private configuration; never expose or retain them.
        child->setReadChannel(QProcess::StandardError);
        while (child->bytesAvailable() > 0)
            child->read(4096);
        child->setReadChannel(QProcess::StandardOutput);
    });
    connect(child, &QProcess::errorOccurred, this, [this, child, expected](QProcess::ProcessError error)
    {
        if (process == child && generation == expected && error == QProcess::FailedToStart)
            fail("无法启动自动连接组件，请检查客户端安装文件。");
    });
    connect(child, &QProcess::finished, this, [this, child, expected](int, QProcess::ExitStatus)
    {
        if (process == child && generation == expected)
            fail("自动连接组件已退出，请重新连接。");
    });
    startup.start(60000);
    child->start();
}

void TunnelSession::stop()
{
    ++generation;
    startup.stop();
    buffer.clear();
    configuration = {};
    announced = false;
    auto *child = process;
    process = nullptr;
    if (!child)
        return;
    disconnect(child, nullptr, this, nullptr);
    child->closeWriteChannel();
    if (child->state() != QProcess::NotRunning && !child->waitForFinished(250))
    {
        child->terminate();
        if (!child->waitForFinished(250))
        {
            child->kill();
            child->waitForFinished(1000);
        }
    }
    child->deleteLater();
}

void TunnelSession::fail(const QString &message)
{
    auto safe = message;
    const auto secret = configuration.value("relay_token").toString();
    if (!secret.isEmpty())
        safe.replace(secret, "[密钥已隐藏]");
    safe.remove(QRegularExpression("[\\x00-\\x1f\\x7f]"));
    stop();
    emit failed(safe.left(500));
}

void TunnelSession::readOutput(QProcess *source, quint64 expected)
{
    if (process != source || generation != expected)
        return;
    while (source->bytesAvailable() > 0)
    {
        buffer.append(source->read(4096));
        qsizetype newline;
        while ((newline = buffer.indexOf('\n')) >= 0)
        {
            if (newline > 16 * 1024)
            {
                fail("自动连接组件返回的信息过大。");
                return;
            }
            const auto line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
            {
                fail("自动连接组件返回了无效信息。");
                return;
            }
            const auto object = document.object();
            const auto type = object.value("type").toString();
            if (type == "ready")
            {
                const auto raw = object.value("url").toString();
                const QUrl url(raw, QUrl::StrictMode);
                if (announced || !url.isValid() || url.scheme() != "https" || url.host() != "127.0.0.1" ||
                    url.port() < 1 || url.authority(QUrl::FullyEncoded).contains('@') || url.hasQuery() ||
                    url.hasFragment() || (!url.path().isEmpty() && url.path() != "/"))
                {
                    fail("自动连接组件返回了无效的本机地址。");
                    return;
                }
                announced = true;
                startup.stop();
                emit ready(raw);
            }
            else if (type == "path" &&
                     (object.value("path") == "direct" || object.value("path") == "relay"))
                emit pathChanged(object.value("path").toString());
            else if (type == "error")
            {
                fail(object.value("message").toString("自动连接失败，请检查网络。"));
                return;
            }
            else
            {
                fail("自动连接组件返回了未知信息。");
                return;
            }
            if (process != source || generation != expected)
                return;
        }
        if (buffer.size() > 16 * 1024)
        {
            fail("自动连接组件返回的信息过大。");
            return;
        }
    }
}
