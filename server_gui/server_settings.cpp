#include "server_settings.h"
#include "connection_info.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QProcess>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSpinBox>
#include <QSslCertificate>
#include <QSslKey>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

ForwardingSettings::ForwardingSettings(const QString &path, QWidget *parent)
    : QWidget(parent), configPath(path), transport(this)
{
    setObjectName("forwardingSettings");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 14, 12, 12);
    auto *description = new QLabel("仅勾选的群会转发到客户端；取消勾选会立即从所有客户端移除。\n请先启动这台"
                                   "电脑上的服务，再读取群列表。",
                                   this);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *searchRow = new QHBoxLayout;
    search = new QLineEdit;
    search->setObjectName("forwardingSearch");
    search->setPlaceholderText("搜索本机微信群");
    search->setClearButtonEnabled(true);
    searchRow->addWidget(search, 1);
    refreshButton = new QPushButton("读取本机群列表");
    refreshButton->setObjectName("forwardingRefresh");
    searchRow->addWidget(refreshButton);
    layout->addLayout(searchRow);
    groups = new QListWidget;
    groups->setObjectName("forwardingGroups");
    groups->setFont(QApplication::font());
    groups->setIconSize({24, 24});
    groups->setMinimumHeight(210);
    layout->addWidget(groups, 1);
    auto *selection = new QHBoxLayout;
    auto *all = new QPushButton("全选"), *none = new QPushButton("清空");
    all->setObjectName("forwardingSelectAll");
    none->setObjectName("forwardingClear");
    selection->addWidget(all);
    selection->addWidget(none);
    count = new QLabel;
    count->setObjectName("forwardingCount");
    selection->addStretch();
    selection->addWidget(count);
    layout->addLayout(selection);
    status = new QLabel("正在读取本机服务配置…");
    status->setObjectName("forwardingStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    applyButton = new QPushButton("保存转发群（立即生效）");
    applyButton->setObjectName("forwardingApply");
    applyButton->setEnabled(false);
    layout->addWidget(applyButton);
    connect(search, &QLineEdit::textChanged, this,
            [this](const QString &query)
            {
                for (int i = 0; i < groups->count(); ++i)
                    groups->item(i)->setHidden(!groups->item(i)->text().contains(query, Qt::CaseInsensitive));
            });
    connect(all, &QPushButton::clicked, this,
            [this]
            {
                if (working || !available)
                    return;
                for (int i = 0; i < groups->count(); ++i)
                    groups->item(i)->setCheckState(Qt::Checked);
            });
    connect(none, &QPushButton::clicked, this,
            [this]
            {
                if (working || !available)
                    return;
                for (int i = 0; i < groups->count(); ++i)
                    groups->item(i)->setCheckState(Qt::Unchecked);
            });
    connect(groups, &QListWidget::itemChanged, this, &ForwardingSettings::updateCount);
    connect(refreshButton, &QPushButton::clicked, this, &ForwardingSettings::refresh);
    connect(applyButton, &QPushButton::clicked, this, &ForwardingSettings::apply);
    updateCount();
    QTimer::singleShot(0, this, &ForwardingSettings::refresh);
}
void ForwardingSettings::updateCount()
{
    int selected = 0;
    for (int i = 0; i < groups->count(); ++i)
        if (groups->item(i)->checkState() == Qt::Checked)
            ++selected;
    count->setText(QString("已选 %1 / %2 个群").arg(selected).arg(groups->count()));
}
void ForwardingSettings::refresh()
{
    if (working)
        return;
    available = false;
    applyButton->setEnabled(false);
    QString error;
    if (!configureLocalAdmin(transport, configPath, error))
    {
        status->setText(error);
        return;
    }
    working = true;
    refreshButton->setEnabled(false);
    groups->setEnabled(false);
    status->setText("正在读取本机服务的全部群…");
    transport.get(
        "/api/local/forwarding",
        [this](const QJsonValue &value, const QString &error)
        {
            working = false;
            refreshButton->setEnabled(true);
            groups->setEnabled(true);
            if (!error.isEmpty())
            {
                status->setText("无法读取本机群列表，请先启动服务，再点击读取。\n" + error);
                return;
            }
            const auto object = value.toObject();
            if (!object.value("groups").isArray() || !object.value("selected").isArray())
            {
                status->setText("本机服务未提供转发群设置，请更新服务后重试。");
                return;
            }
            QSet<QString> selected;
            for (const auto &id : object.value("selected").toArray())
                selected.insert(id.toString());
            groups->blockSignals(true);
            groups->clear();
            for (const auto &entry : object.value("groups").toArray())
            {
                const auto group = entry.toObject();
                auto *item = new QListWidgetItem(group.value("name").toString(), groups);
                item->setData(Qt::UserRole, group.value("id").toString());
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setSizeHint({1, 34});
                item->setCheckState(selected.contains(group.value("id").toString()) ? Qt::Checked
                                                                                    : Qt::Unchecked);
                item->setHidden(!item->text().contains(search->text(), Qt::CaseInsensitive));
            }
            groups->blockSignals(false);
            available = true;
            applyButton->setEnabled(true);
            updateCount();
            status->setText("勾选需要转发的群，保存后立即生效；客户端还需单独获得这些群的权限。");
        },
        5000);
}

bool configureLocalAdmin(Transport &transport, const QString &configPath, QString &error)
{
    QFile file(configPath);
    QJsonObject config;
    if (file.open(QIODevice::ReadOnly))
        config = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QUrl endpoint("http://" + config.value("listen").toString());
    QHostAddress address(endpoint.host());
    if (address == QHostAddress::AnyIPv4)
        endpoint.setHost("127.0.0.1");
    else if (address == QHostAddress::AnyIPv6 || address == QHostAddress::Any)
        endpoint.setHost("::1");
    const auto host = endpoint.host();
    if (!endpoint.isValid() || endpoint.port() < 1 || config.value("token").toString().size() < 24 ||
        !(QHostAddress(host).isLoopback() || host == "localhost"))
    {
        error = "请先保存服务配置并启动服务。管理仅允许本机回环连接；监听 IP 请用 "
                "127.0.0.1、0.0.0.0、::1 或 ::。";
        return false;
    }
    QString pin;
    const auto certificatePath = config.value("tls_cert").toString();
    if (!certificatePath.isEmpty())
    {
        const auto certs =
            QSslCertificate::fromPath(QFileInfo(configPath).dir().absoluteFilePath(certificatePath));
        if (certs.isEmpty())
        {
            error = "无法读取服务证书，请在“连接与存储”检查证书路径。";
            return false;
        }
        endpoint.setScheme("https");
        pin = QString::fromLatin1(certs.first().digest(QCryptographicHash::Sha256).toHex());
    }
    transport.configure(endpoint.toString(), config.value("token").toString(), pin);
    return true;
}
void ForwardingSettings::apply()
{
    if (!available || working)
        return;
    QJsonArray selected;
    for (int i = 0; i < groups->count(); ++i)
        if (groups->item(i)->checkState() == Qt::Checked)
            selected.append(groups->item(i)->data(Qt::UserRole).toString());
    working = true;
    applyButton->setEnabled(false);
    refreshButton->setEnabled(false);
    groups->setEnabled(false);
    status->setText("正在保存转发群…");
    transport.post(
        "/api/local/forwarding", {{"selected", selected}},
        [this](const QJsonValue &, const QString &error)
        {
            working = false;
            refreshButton->setEnabled(true);
            groups->setEnabled(true);
            if (!error.isEmpty())
            {
                applyButton->setEnabled(true);
                status->setText("保存失败，勾选已保留，可重试。\n" + error);
                return;
            }
            status->setText("转发群已保存并立即生效。");
            refresh();
        },
        5000);
}

bool editLocalService(QWidget *parent, const QString &configPath, bool showAutomatic)
{
    QString directory = QFileInfo(configPath).absolutePath();
    QDir().mkpath(directory);
    QJsonObject config;
    QFile old(configPath);
    if (old.open(QIODevice::ReadOnly))
    {
        config = QJsonDocument::fromJson(old.readAll()).object();
        old.close();
    }
    QDialog dialog(parent);
    dialog.setObjectName("serviceConnectionSettings");
    dialog.setWindowTitle("转发服务端 · 连接与存储");
    dialog.resize(700, 640);
    auto *rootLayout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("serviceConnectionTabs");
    rootLayout->addWidget(tabs, 1);
    auto *connectionPage = new QWidget;
    tabs->addTab(connectionPage, "连接与存储");
    auto *layout = new QVBoxLayout(connectionPage);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto *form = new QFormLayout;
    form->setSpacing(12);
    auto current = config["listen"].toString("127.0.0.1:8765");
    int split = current.lastIndexOf(':');
    auto initialHost = current.left(split);
    if (initialHost.startsWith('[') && initialHost.endsWith(']'))
        initialHost = initialHost.mid(1, initialHost.size() - 2);
    auto *host = new QLineEdit(initialHost);
    host->setObjectName("serviceListenHost");
    auto *port = new QSpinBox;
    port->setRange(1024, 65535);
    port->setValue(current.mid(split + 1).toInt());
    port->setObjectName("serviceListenPort");
    auto *row = new QHBoxLayout;
    row->addWidget(host);
    row->addWidget(port);
    form->addRow("监听 IP 与端口", row);
    auto *data = new QLineEdit(config["data"].toString(directory + "/data"));
    auto *dataRow = new QHBoxLayout;
    dataRow->addWidget(data);
    auto *browseData = new QPushButton("选择…");
    dataRow->addWidget(browseData);
    form->addRow("服务数据目录（含历史和权限）", dataRow);
    QObject::connect(browseData, &QPushButton::clicked, &dialog,
                     [&]
                     {
                         auto selected =
                             QFileDialog::getExistingDirectory(&dialog, "选择服务端保存目录", data->text());
                         if (!selected.isEmpty())
                             data->setText(selected);
                     });
    auto secret = config["token"].toString();
    if (secret.size() < 24)
        secret = QUuid::createUuid().toString(QUuid::WithoutBraces) +
                 QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto *token = new QLineEdit(secret);
    token->setEchoMode(QLineEdit::Password);
    token->setReadOnly(true);
    form->addRow("本机管理员密钥", token);
    auto *certificate = new QLineEdit(config["tls_cert"].toString()),
         *key = new QLineEdit(config["tls_key"].toString());
    certificate->setObjectName("serviceCertificate");
    key->setObjectName("servicePrivateKey");
    auto addFile = [&](const QString &label, QLineEdit *edit)
    {
        auto *line = new QHBoxLayout;
        line->addWidget(edit);
        auto *choose = new QPushButton("选择…");
        line->addWidget(choose);
        form->addRow(label, line);
        QObject::connect(choose, &QPushButton::clicked, &dialog,
                         [&, edit]
                         {
                             auto path = QFileDialog::getOpenFileName(&dialog, "选择 PEM 文件", directory,
                                                                      "PEM (*.pem);;全部文件 (*)");
                             if (!path.isEmpty())
                                 edit->setText(path);
                         });
    };
    addFile("TLS 证书", certificate);
    addFile("TLS 私钥", key);
    auto *fingerprint = new QLineEdit;
    fingerprint->setReadOnly(true);
    form->addRow("证书 SHA-256", fingerprint);
    layout->addLayout(form);
    auto updateFingerprint = [&]
    {
        const auto certificatePath = certificate->text().trimmed();
        if (certificatePath.isEmpty())
        {
            fingerprint->clear();
            return;
        }
        auto certs = QSslCertificate::fromPath(certificatePath);
        fingerprint->setText(
            certs.isEmpty() ? ""
                            : QString::fromLatin1(certs.first().digest(QCryptographicHash::Sha256).toHex()));
    };
    updateFingerprint();
    QObject::connect(certificate, &QLineEdit::textChanged, &dialog, updateFingerprint);
    auto *generate = new QPushButton("生成本机自签名证书");
    generate->setObjectName("serviceGenerateCertificate");
    layout->addWidget(generate);
    QObject::connect(
        generate, &QPushButton::clicked, &dialog,
        [&]
        {
            QString binary = QCoreApplication::applicationDirPath() + "/wechat-relay";
#ifdef Q_OS_WIN
            binary += ".exe";
#endif
            if (!QFileInfo::exists(binary))
            {
                QDir project(QCoreApplication::applicationDirPath());
                for (int i = 0; i < 6 && !QFileInfo::exists(project.filePath("Cargo.toml")); ++i)
                    project.cdUp();
                binary = project.filePath("target/debug/wechat-relay"
#ifdef Q_OS_WIN
                                          ".exe"
#endif
                );
            }
            QStringList names{"localhost", "127.0.0.1", QSysInfo::machineHostName()};
            for (const auto &address : QNetworkInterface::allAddresses())
                if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
                    names << address.toString();
            names.removeDuplicates();
            if (QFileInfo::exists(directory + "/.state/tls/key.pem") &&
                !QSslCertificate::fromPath(directory + "/.state/tls/cert.pem").isEmpty())
            {
                certificate->setText(directory + "/.state/tls/cert.pem");
                key->setText(directory + "/.state/tls/key.pem");
                updateFingerprint();
                return;
            }
            QProcess process;
#ifdef Q_OS_WIN
            process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments)
                                                      { arguments->flags |= 0x08000000; });
#endif
            process.setWorkingDirectory(directory);
            process.start(binary, {"--make-cert", "--names", names.join(',')});
            if (!process.waitForFinished(5000) || process.exitCode() != 0)
            {
                QMessageBox::warning(&dialog, "生成失败", QString::fromUtf8(process.readAllStandardError()));
                return;
            }
            certificate->setText(directory + "/.state/tls/cert.pem");
            key->setText(directory + "/.state/tls/key.pem");
            updateFingerprint();
        });
    auto *description = new QLabel(
        "仅本机使用时，监听 127.0.0.1 即可。\nMac 等其他电脑连接时，监听 0.0.0.0 并配置 TLS "
        "证书与私钥；客户端填 Windows 电脑的实际 "
        "IP、独立接入密钥和证书指纹。\n本机管理员密钥不能发给客户端；请在“客户端权限”页创建接入密钥。"
        "\n此目录保存历史、附件、群选择和客户端权限；更换空目录将使用新的数据和权限库。"
        "\n保存后启动服务生效。防火墙需允许所选服务端口。");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto automatic = config.value("p2p").toObject();
    auto *automaticPage = new QWidget;
    tabs->addTab(automaticPage, "自动连接");
    if (showAutomatic)
        tabs->setCurrentWidget(automaticPage);
    auto *automaticLayout = new QVBoxLayout(automaticPage);
    automaticLayout->setContentsMargins(24, 24, 24, 24);
    automaticLayout->setSpacing(16);
    auto *automaticEnabled = new QCheckBox("启用自动连接（直连优先，中继备用）");
    automaticEnabled->setObjectName("serviceP2pEnabled");
    automaticEnabled->setChecked(automatic.value("enabled").toBool());
    automaticLayout->addWidget(automaticEnabled);
    auto *automaticDescription = new QLabel("粘贴管理员提供的公网入口信息。启动服务后会生成稳定设备 ID，"
        "可在“客户端权限”中复制自动连接信息。\n自动连接必须配置 TLS 证书与私钥，局域网地址仍可使用。"
        "公网入口不包含本机管理员密钥或 SSH 登录凭据。");
    automaticDescription->setWordWrap(true);
    automaticLayout->addWidget(automaticDescription);
    auto *entryForm = new QFormLayout;
    auto *relayUrl = new QLineEdit(automatic.value("relay_url").toString());
    relayUrl->setObjectName("serviceRelayUrl");
    relayUrl->setPlaceholderText("https://relay.example.invalid:54443/");
    auto *relayToken = new QLineEdit(automatic.value("relay_token").toString());
    relayToken->setObjectName("serviceRelayToken");
    relayToken->setReadOnly(true);
    relayToken->setEchoMode(QLineEdit::Password);
    auto *relayPort = new QSpinBox;
    relayPort->setObjectName("serviceRelayQuicPort");
    relayPort->setRange(1, 65535);
    relayPort->setValue(automatic.value("relay_quic_port").toInt(443));
    relayPort->setReadOnly(true);
    relayPort->setButtonSymbols(QAbstractSpinBox::NoButtons);
    auto *relayCa = new QPlainTextEdit(automatic.value("relay_ca").toString());
    relayCa->setObjectName("serviceRelayCa");
    relayCa->setReadOnly(true);
    relayCa->setMaximumHeight(125);
    entryForm->addRow("公网中转地址", relayUrl);
    entryForm->addRow("入口接入凭据", relayToken);
    entryForm->addRow("QUIC 端口", relayPort);
    entryForm->addRow("入口 CA 证书", relayCa);
    automaticLayout->addLayout(entryForm);
    auto *relayHelp = new QLabel("修改地址会保存到服务端配置，并用于之后复制的自动连接信息；"
        "入口接入凭据、CA 证书和 QUIC 端口保持不变。若新中转的凭据或证书不同，"
        "请粘贴完整公网入口信息。保存后重新启动服务生效。");
    relayHelp->setObjectName("serviceRelayHelp");
    relayHelp->setWordWrap(true);
    automaticLayout->addWidget(relayHelp);
    auto *pasteEntry = new QPushButton("粘贴公网入口信息");
    pasteEntry->setObjectName("servicePasteP2pEntry");
    automaticLayout->addWidget(pasteEntry, 0, Qt::AlignLeft);
    auto *entryStatus = new QLabel;
    entryStatus->setObjectName("serviceP2pEntryStatus");
    entryStatus->setWordWrap(true);
    automaticLayout->addWidget(entryStatus);
    automaticLayout->addStretch();
    QObject::connect(pasteEntry, &QPushButton::clicked, &dialog, [&]
    {
        const auto text = QApplication::clipboard()->text();
        QJsonObject parsed;
        QString error;
        if (text.size() > ConnectionInfo::maximumBytes ||
            !ConnectionInfo::publicEntry(text.toUtf8(), parsed, error))
        {
            entryStatus->setText(error.isEmpty() ? "公网入口信息超过 64 KiB，未导入。" : error);
            return;
        }
        automatic = parsed;
        relayUrl->setText(parsed.value("relay_url").toString());
        relayToken->setText(parsed.value("relay_token").toString());
        relayCa->setPlainText(parsed.value("relay_ca").toString());
        relayPort->setValue(parsed.value("relay_quic_port").toInt());
        entryStatus->setText("已填入。请核对 TLS 配置，保存后启动服务生效。");
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setObjectName("serviceConnectionSave");
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(
        buttons, &QDialogButtonBox::accepted, &dialog,
        [&]
        {
            if (token->text().size() < 24 || !QDir(data->text()).isAbsolute())
            {
                QMessageBox::information(&dialog, "配置未保存",
                                         "访问密钥至少 24 个字符，保存目录需要完整路径。");
                return;
            }
            auto hostValue = host->text().trimmed();
            if (hostValue == "localhost")
                hostValue = "127.0.0.1";
            QHostAddress address;
            if (!address.setAddress(hostValue))
            {
                QMessageBox::information(&dialog, "配置未保存",
                                         "请输入有效的监听 IP，例如 127.0.0.1 或 0.0.0.0。");
                return;
            }
            if (!address.isLoopback() && address != QHostAddress::AnyIPv4 &&
                address != QHostAddress::AnyIPv6 && address != QHostAddress::Any)
            {
                QMessageBox::information(&dialog, "配置未保存",
                    "其他电脑连接时请监听 0.0.0.0 或 ::，以保留本机管理通道。");
                return;
            }
            bool loopback = address.isLoopback();
            automatic["relay_url"] = relayUrl->text().trimmed();
            if (automaticEnabled->isChecked())
            {
                QString error;
                if (!ConnectionInfo::relaySettings(automatic, error))
                {
                    entryStatus->setText(error);
                    tabs->setCurrentWidget(automaticPage);
                    return;
                }
                if (certificate->text().isEmpty() || key->text().isEmpty())
                {
                    entryStatus->setText("自动连接需要 TLS 证书和私钥，请先在“连接与存储”中配置或生成。");
                    tabs->setCurrentWidget(automaticPage);
                    return;
                }
            }
            if (certificate->text().isEmpty() != key->text().isEmpty())
            {
                QMessageBox::information(&dialog, "配置未保存", "TLS 证书和私钥必须同时设置。");
                return;
            }
            if (!loopback && (!QFileInfo::exists(certificate->text()) || !QFileInfo::exists(key->text())))
            {
                QMessageBox::information(&dialog, "需要 TLS",
                                         "对其他电脑开放服务需要证书和私钥，可先点击生成本机自签名证书。");
                return;
            }
            if (!certificate->text().isEmpty())
            {
                QFile privateFile(key->text());
                privateFile.open(QIODevice::ReadOnly);
                const auto pem = privateFile.readAll();
                const bool parsedKey = !QSslKey(pem, QSsl::Rsa).isNull() ||
                    !QSslKey(pem, QSsl::Ec).isNull() || !QSslKey(pem, QSsl::Dsa).isNull();
                if (QSslCertificate::fromPath(certificate->text()).isEmpty() || !parsedKey)
                {
                    QMessageBox::information(&dialog, "配置未保存", "无法解析 PEM 证书或私钥，请重新选择或生成。");
                    return;
                }
            }
            config["listen"] = (hostValue.contains(':') ? "[" + hostValue + "]" : hostValue) + ":" +
                               QString::number(port->value());
            config["token"] = token->text();
            config["data"] = QDir(data->text()).absolutePath();
            config["tls_cert"] = certificate->text();
            config["tls_key"] = key->text();
            automatic["enabled"] = automaticEnabled->isChecked();
            config["p2p"] = automatic;
            QSaveFile file(configPath);
            if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(config).toJson()) < 0 ||
                !file.commit())
            {
                QMessageBox::warning(&dialog, "无法保存", file.errorString());
                return;
            }
            QFile::setPermissions(configPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            dialog.accept();
        });
    buttons->button(QDialogButtonBox::Save)->setText("保存连接配置");
    buttons->button(QDialogButtonBox::Cancel)->setText("关闭");
    rootLayout->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}
