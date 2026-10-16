#include "server_window.h"
#include "client_permissions.h"
#include "server_settings.h"
#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QNetworkInterface>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSslCertificate>
#include <QStandardPaths>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

static QString relayBinary()
{
    auto binary = QCoreApplication::applicationDirPath() + "/wechat-relay";
#ifdef Q_OS_WIN
    binary += ".exe";
#endif
    return binary;
}

static QIcon serverIcon()
{
    QPixmap image(64, 64);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#08ae71"));
    painter.drawRoundedRect(QRect(2, 2, 60, 60), 14, 14);
    painter.setBrush(Qt::white);
    for (int y : {16, 30, 44})
        painter.drawRoundedRect(QRect(15, y, 34, 8), 2, 2);
    return QIcon(image);
}

ServerWindow::ServerWindow(const QString &path, bool demonstration, QWidget *parent)
    : QMainWindow(parent), configPath(QFileInfo(path).absoluteFilePath()), demo(demonstration),
      admin(this), service(this), tray(this)
{
    setObjectName("serverWindow");
    setWindowTitle(demo ? "微信轻聊 · 转发服务端（演示）" : "微信轻聊 · 转发服务端");
    setWindowIcon(serverIcon());
    resize(940, 700);
    setMinimumSize(760, 580);
    QString error;
    ensureConfiguration(error);
    auto *root = new QWidget;
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(14);
    auto *header = new QHBoxLayout;
    auto *heading = new QLabel("转发服务端");
    auto font = heading->font();
    font.setPixelSize(23);
    font.setBold(true);
    heading->setFont(font);
    header->addWidget(heading);
    header->addStretch();
    start = new QPushButton("启动服务");
    start->setObjectName("serverStart");
    stop = new QPushButton("停止服务");
    stop->setObjectName("serverStop");
    stop->setEnabled(false);
    settings = new QPushButton("连接与存储");
    settings->setObjectName("serverConfigure");
    header->addWidget(start);
    header->addWidget(stop);
    header->addWidget(settings);
    layout->addLayout(header);
    auto *description = new QLabel("此应用运行在已登录微信的电脑上。客户端可以安装在其他电脑；关闭客户端不会停止转发服务。"
        "\n先启动服务并选择转发群，再为每个客户端创建独立接入密钥。权限修改立即生效。");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("serverTabs");
    auto *overview = new QWidget;
    auto *overviewLayout = new QVBoxLayout(overview);
    auto *form = new QFormLayout;
    state = new QLabel("已停止");
    state->setObjectName("serverState");
    bridge = new QLabel("尚未启动");
    bridge->setObjectName("serverBridgeStatus");
    bridge->setWordWrap(true);
    address = new QLabel;
    address->setObjectName("serverAddresses");
    address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    address->setWordWrap(true);
    online = new QLabel("0");
    online->setObjectName("serverOnlineClients");
    form->addRow("服务状态", state);
    form->addRow("本机微信", bridge);
    form->addRow("客户端地址", address);
    form->addRow("已连接客户端密钥数", online);
    p2pState = new QLabel("未启用");
    p2pState->setObjectName("serverP2pState");
    p2pId = new QLabel;
    p2pId->setObjectName("serverP2pId");
    p2pId->setWordWrap(true);
    p2pId->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("自动连接", p2pState);
    form->addRow("服务器设备 ID", p2pId);
    auto *configuration = new QLabel(QDir::toNativeSeparators(configPath));
    configuration->setWordWrap(true);
    configuration->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("服务端配置", configuration);
    overviewLayout->addLayout(form);
    auto *folder = new QPushButton("打开服务端数据目录");
    overviewLayout->addWidget(folder, 0, Qt::AlignLeft);
    log = new QPlainTextEdit;
    log->setObjectName("serverLog");
    log->setReadOnly(true);
    log->setMaximumBlockCount(200);
    log->setPlaceholderText("服务启动与连接状态将显示在这里。");
    overviewLayout->addWidget(log, 1);
    tabs->addTab(overview, "服务状态");
    forwarding = new ForwardingSettings(configPath);
    tabs->addTab(forwarding, "转发群");
    clients = new ClientPermissions(configPath);
    tabs->addTab(clients, "客户端权限");
    forwarding->setEnabled(false);
    clients->setEnabled(false);
    layout->addWidget(tabs, 1);
    feedback = new QLabel(error.isEmpty() ? "服务端只访问这台电脑上的微信。配置保存在用户数据目录，不随安装包分发。" : error);
    feedback->setObjectName("serverFeedback");
    feedback->setWordWrap(true);
    layout->addWidget(feedback);
    setCentralWidget(root);
    statusTimer.setInterval(5000);
#ifdef Q_OS_WIN
    service.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments)
        { arguments->flags |= 0x08000000; });
#endif
    service.setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    // Native adapter dependencies are deployed beside the server, not in the user's data directory.
#ifdef Q_OS_WIN
    environment.insert("PATH", QDir::toNativeSeparators(QCoreApplication::applicationDirPath()) + ";" +
        environment.value("PATH"));
#endif
    service.setProcessEnvironment(environment);
    connect(&service, &QProcess::readyReadStandardOutput, this,
        [this] { appendLog(QString::fromUtf8(service.readAllStandardOutput())); });
    connect(&service, &QProcess::started, this, [this]
    {
        state->setText("运行中");
        start->setEnabled(false);
        stop->setEnabled(true);
        settings->setEnabled(false);
        statusTimer.start();
        pollStatus();
    });
    connect(&service, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status)
    {
        statusTimer.stop();
        observedHttp = false;
        polling = false;
        p2pPolling = false;
        ++serviceGeneration;
        admin.disconnectService();
        start->setEnabled(true);
        stop->setEnabled(false);
        settings->setEnabled(true);
        state->setText(code == 0 && status == QProcess::NormalExit ? "已停止" : "启动失败或异常退出");
        bridge->setText("未连接");
        online->setText("0");
        p2pState->setText("已停止");
        forwarding->setEnabled(false);
        clients->setEnabled(false);
        appendLog(QString("转发服务已退出（%1）。").arg(code));
    });
    connect(&service, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
    {
        if (error == QProcess::FailedToStart)
        {
            state->setText("无法启动服务");
            feedback->setText(service.errorString());
            start->setEnabled(true);
            settings->setEnabled(true);
        }
    });
    connect(&statusTimer, &QTimer::timeout, this, &ServerWindow::pollStatus);
    connect(start, &QPushButton::clicked, this, &ServerWindow::startService);
    connect(stop, &QPushButton::clicked, this, &ServerWindow::stopService);
    connect(settings, &QPushButton::clicked, this, [this]
    {
        if (service.state() == QProcess::NotRunning && editLocalService(this, configPath))
            feedback->setText("连接配置已保存，启动服务后生效。");
    });
    connect(folder, &QPushButton::clicked, this, [this]
    {
        QFile file(configPath);
        if (file.open(QIODevice::ReadOnly))
        {
            auto directory = QJsonDocument::fromJson(file.readAll()).object().value("data").toString();
            QDir().mkpath(directory);
            QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
        }
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this](int index)
    {
        if (!observedHttp)
            return;
        if (index == 1)
            forwarding->refresh();
        else if (index == 2)
            clients->refresh();
    });
    tray.setIcon(windowIcon());
    auto *menu = new QMenu(this);
    menu->addAction("打开转发服务端", this, [this] { showNormal(); raise(); activateWindow(); });
    menu->addSeparator();
    menu->addAction("停止服务并退出", this, [this]
    {
        quitting = true;
        stopService();
        QApplication::quit();
    });
    tray.setContextMenu(menu);
    connect(&tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason)
    {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
        {
            showNormal();
            raise();
            activateWindow();
        }
    });
    if (QSystemTrayIcon::isSystemTrayAvailable())
        tray.show();
}

ServerWindow::~ServerWindow()
{
    stopService();
}

bool ServerWindow::ensureConfiguration(QString &error)
{
    auto directory = QFileInfo(configPath).dir();
    if (!directory.mkpath("."))
    {
        error = "无法创建服务端配置目录。";
        return false;
    }
    QJsonObject config;
    QFile old(configPath);
    QString origin = configPath;
    if (!old.exists() && !demo)
    {
        const auto legacy = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
            "/../WeChatLite/local-server/server.json";
        if (QFileInfo::exists(legacy))
        {
            old.setFileName(legacy);
            origin = QFileInfo(legacy).absoluteFilePath();
        }
    }
    if (old.exists())
    {
        QJsonParseError parse;
        if (!old.open(QIODevice::ReadOnly))
        {
            error = old.errorString();
            return false;
        }
        const auto document = QJsonDocument::fromJson(old.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject())
        {
            error = "服务端配置不是有效 JSON；原文件未覆盖。";
            return false;
        }
        config = document.object();
        old.close();
    }
    if (config.value("listen").toString().isEmpty())
        config["listen"] = "127.0.0.1:8765";
    if (config.value("token").toString().size() < 24)
        config["token"] = QUuid::createUuid().toString(QUuid::WithoutBraces) +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (config.value("data").toString().isEmpty())
        config["data"] = directory.absoluteFilePath("data");
    for (const auto &field : {"data", "tls_cert", "tls_key"})
    {
        const auto value = config.value(field).toString();
        if (!value.isEmpty() && !QDir::isAbsolutePath(value))
            config[field] = QFileInfo(origin).dir().absoluteFilePath(value);
    }
#ifdef Q_OS_WIN
    const auto configured = config.value("bridge_program").toString();
    if (configured.isEmpty() || QFileInfo(configured).fileName() == "wechat-adapter.exe")
    {
        config["bridge_program"] = QCoreApplication::applicationDirPath() + "/adapter/wechat-adapter.exe";
        config["bridge_args"] = QJsonArray();
    }
#endif
    QSaveFile output(configPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(config).toJson()) < 0 || !output.commit())
    {
        error = output.errorString();
        return false;
    }
    QFile::setPermissions(configPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

void ServerWindow::startService()
{
    if (service.state() != QProcess::NotRunning)
        return;
    ++serviceGeneration;
    p2pPolling = false;
    QString error;
    if (!ensureConfiguration(error) || !configureLocalAdmin(admin, configPath, error))
    {
        feedback->setText(error);
        return;
    }
    if (!QFileInfo::exists(relayBinary()))
    {
        feedback->setText("找不到同目录的 wechat-relay，请保留完整服务端文件夹。");
        return;
    }
    QFile file(configPath);
    file.open(QIODevice::ReadOnly);
    runningConfig = QJsonDocument::fromJson(file.readAll()).object();
    p2pState->setText(runningConfig.value("p2p").toObject().value("enabled").toBool() ? "正在启动…" : "未启用");
    p2pId->clear();
    QString pin;
    const auto certificate = runningConfig.value("tls_cert").toString();
    if (!certificate.isEmpty())
    {
        auto certificates = QSslCertificate::fromPath(certificate);
        if (!certificates.isEmpty())
            pin = QString::fromLatin1(certificates.first().digest(QCryptographicHash::Sha256).toHex());
    }
    admin.setOwnedServer(admin.baseUrl().toString(), runningConfig.value("token").toString(), pin);
    QStringList addresses;
    auto endpoint = admin.baseUrl();
    const auto host = QUrl("http://" + runningConfig.value("listen").toString()).host();
    QHostAddress bind(host);
    if (!bind.isLoopback())
        for (const auto &candidate : QNetworkInterface::allAddresses())
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol && !candidate.isLoopback())
            {
                auto reachable = endpoint;
                reachable.setHost(candidate.toString());
                addresses << reachable.toString();
            }
    addresses << endpoint.toString();
    address->setText(addresses.join('\n'));
    state->setText("正在启动…");
    bridge->setText("正在连接本机微信…");
    start->setEnabled(false);
    settings->setEnabled(false);
    observedHttp = false;
    service.setProgram(relayBinary());
    QStringList arguments{"--config", configPath};
    if (demo)
        arguments << "--demo";
    service.setArguments(arguments);
    service.setWorkingDirectory(QFileInfo(configPath).absolutePath());
    service.start();
    feedback->setText(demo ? "演示模式，不连接真实微信。" :
        "服务运行后可关闭客户端；关闭此窗口会收起到托盘。停止服务请用上方按钮或服务端托盘菜单。");
}

void ServerWindow::stopService()
{
    ++serviceGeneration;
    p2pPolling = false;
    statusTimer.stop();
    if (service.state() == QProcess::NotRunning)
        return;
    stop->setEnabled(false);
    state->setText("正在停止…");
    admin.shutdownLocalServer(&service);
}

void ServerWindow::pollStatus()
{
    if (polling || service.state() != QProcess::Running)
        return;
    polling = true;
    admin.get("/api/status", [this](const QJsonValue &value, const QString &error)
    {
        polling = false;
        if (service.state() != QProcess::Running)
            return;
        if (!error.isEmpty())
        {
            bridge->setText("等待服务响应：" + error);
            return;
        }
        const auto status = value.toObject();
        bridge->setText(status.value("ready").toBool() ? "已连接" :
            status.value("text").toString("微信尚未连接"));
        online->setText(QString::number(status.value("online_clients").toInt()));
        if (!observedHttp)
        {
            observedHttp = true;
            forwarding->setEnabled(true);
            clients->setEnabled(true);
            forwarding->refresh();
            clients->refresh();
        }
        if (!p2pPolling && runningConfig.value("p2p").toObject().value("enabled").toBool())
        {
            p2pPolling = true;
            const auto generation = serviceGeneration;
            admin.get("/api/local/p2p", [this, generation](const QJsonValue &value, const QString &error)
            {
                if (generation != serviceGeneration || service.state() != QProcess::Running)
                    return;
                p2pPolling = false;
                if (!error.isEmpty())
                {
                    p2pState->setText("等待自动连接服务响应");
                    return;
                }
                const auto info = value.toObject();
                const auto status = info.value("status").toString();
                p2pState->setText(!info.value("enabled").toBool() ? "未启用" :
                    status == "listening" ? "已就绪 · 直连优先" :
                    status == "failed" ? "连接失败，请检查公网入口" : "已停止");
                p2pId->setText(info.value("server_id").toString().left(128));
            }, 2500);
        }
    }, 2500);
}

void ServerWindow::appendLog(const QString &text)
{
    auto safe = text;
    const auto token = runningConfig.value("token").toString();
    if (!token.isEmpty())
        safe.replace(token, "[管理员密钥已隐藏]");
    const auto relayToken = runningConfig.value("p2p").toObject().value("relay_token").toString();
    if (!relayToken.isEmpty())
        safe.replace(relayToken, "[入口凭据已隐藏]");
    safe = safe.left(8192);
    if (!safe.trimmed().isEmpty())
        log->appendPlainText(safe.trimmed());
}

void ServerWindow::closeEvent(QCloseEvent *event)
{
    if (!quitting && service.state() != QProcess::NotRunning && tray.isVisible())
    {
        hide();
        event->ignore();
        return;
    }
    stopService();
    event->accept();
}
