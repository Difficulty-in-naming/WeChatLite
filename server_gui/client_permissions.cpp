#include "client_permissions.h"
#include "server_settings.h"
#include "connection_info.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSslCertificate>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

ClientPermissions::ClientPermissions(const QString &path, QWidget *parent)
    : QWidget(parent), configPath(path), transport(this)
{
    setObjectName("clientPermissions");
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel("每个客户端使用独立密钥。实际可见群 = 本页授权的群 ∩ “转发群”页勾选的群。"
        "\n停用或撤销立即断开该密钥的连接；本机管理员密钥不能用于远程客户端。");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *actions = new QHBoxLayout;
    newButton = new QPushButton("新建客户端");
    newButton->setObjectName("clientAccessNew");
    refreshButton = new QPushButton("刷新列表");
    refreshButton->setObjectName("clientAccessRefresh");
    credentialButton = new QPushButton("复制连接信息");
    credentialButton->setObjectName("clientAccessCredential");
    credentialButton->setEnabled(false);
    actions->addWidget(newButton);
    actions->addWidget(refreshButton);
    actions->addWidget(credentialButton);
    actions->addStretch();
    layout->addLayout(actions);
    auto *split = new QSplitter;
    list = new QListWidget;
    list->setObjectName("clientAccessList");
    list->setMinimumWidth(210);
    split->addWidget(list);
    editor = new QWidget;
    auto *editLayout = new QVBoxLayout(editor);
    auto *form = new QFormLayout;
    name = new QLineEdit;
    name->setObjectName("clientAccessName");
    name->setMaxLength(80);
    form->addRow("客户端名称", name);
    enabled = new QCheckBox("允许连接");
    enabled->setObjectName("clientAccessEnabled");
    canSend = new QCheckBox("允许发送和撤回消息");
    canSend->setObjectName("clientAccessCanSend");
    canDownload = new QCheckBox("允许下载文件和媒体");
    canDownload->setObjectName("clientAccessCanDownload");
    form->addRow(enabled);
    form->addRow(canSend);
    form->addRow(canDownload);
    editLayout->addLayout(form);
    editLayout->addWidget(new QLabel("允许查看的群"));
    groups = new QListWidget;
    groups->setObjectName("clientAccessGroups");
    editLayout->addWidget(groups, 1);
    auto *buttons = new QHBoxLayout;
    applyButton = new QPushButton("保存权限");
    applyButton->setObjectName("clientAccessApply");
    revokeButton = new QPushButton("永久撤销密钥");
    revokeButton->setObjectName("clientAccessRevoke");
    rotateButton = new QPushButton("更换密钥");
    rotateButton->setObjectName("clientAccessRotate");
    rotateButton->setEnabled(false);
    buttons->addWidget(applyButton);
    buttons->addWidget(rotateButton);
    buttons->addWidget(revokeButton);
    editLayout->addLayout(buttons);
    split->addWidget(editor);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    status = new QLabel("启动服务后可管理客户端权限。");
    status->setObjectName("clientAccessStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    editor->setEnabled(false);
    newButton->setEnabled(false);
    connect(refreshButton, &QPushButton::clicked, this, &ClientPermissions::refresh);
    connect(newButton, &QPushButton::clicked, this, &ClientPermissions::newClient);
    connect(list, &QListWidget::currentRowChanged, this, &ClientPermissions::selectClient);
    connect(applyButton, &QPushButton::clicked, this, &ClientPermissions::apply);
    connect(revokeButton, &QPushButton::clicked, this, &ClientPermissions::revoke);
    connect(credentialButton, &QPushButton::clicked, this, &ClientPermissions::existingCredential);
    connect(rotateButton, &QPushButton::clicked, this, &ClientPermissions::rotateCredential);
}

void ClientPermissions::updateCredentialActions()
{
    const bool selected = !selectedId.isEmpty() && list->currentItem();
    const bool revoked = selected && list->currentItem()->data(Qt::UserRole).toJsonObject().value("revoked").toBool();
    credentialButton->setEnabled(!working && available && selected && !revoked);
    rotateButton->setEnabled(!working && available && selected && !revoked);
    editor->setEnabled(!working && available && !revoked);
}

void ClientPermissions::setWorking(bool value)
{
    working = value;
    refreshButton->setEnabled(!value);
    newButton->setEnabled(!value && available);
    list->setEnabled(!value);
    updateCredentialActions();
}

void ClientPermissions::refresh()
{
    if (working)
        return;
    QString error;
    if (!configureLocalAdmin(transport, configPath, error))
    {
        status->setText(error);
        return;
    }
    available = false;
    setWorking(true);
    status->setText("正在读取客户端与群权限…");
    transport.get("/api/local/forwarding", [this](const QJsonValue &value, const QString &error)
    {
        if (!error.isEmpty())
        {
            setWorking(false);
            status->setText(error);
            return;
        }
        catalog = value.toObject().value("groups").toArray();
        const auto global = value.toObject().value("selected").toArray();
        groups->clear();
        for (const auto &entry : catalog)
        {
            const auto group = entry.toObject();
            const auto id = group.value("id").toString();
            auto *item = new QListWidgetItem(group.value("name").toString() +
                (global.contains(id) ? "" : "（尚未转发）"), groups);
            item->setData(Qt::UserRole, id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
        }
        transport.get("/api/local/clients", [this](const QJsonValue &value, const QString &error)
        {
            available = error.isEmpty();
            setWorking(false);
            if (!error.isEmpty())
            {
                status->setText(error);
                return;
            }
            const auto previous = selectedId;
            list->blockSignals(true);
            list->clear();
            int row = -1;
            for (const auto &entry : value.toObject().value("clients").toArray())
            {
                const auto client = entry.toObject();
                QString state = client.value("revoked").toBool() ? "已撤销" :
                    client.value("enabled").toBool() ? "已启用" : "已停用";
                auto *item = new QListWidgetItem(client.value("name").toString() + " · " + state, list);
                item->setData(Qt::UserRole, client);
                if (client.value("id").toString() == previous)
                    row = list->count() - 1;
            }
            list->blockSignals(false);
            if (row < 0 && list->count())
                row = 0;
            list->setCurrentRow(row);
            if (row < 0)
                newClient();
            else
                selectClient();
            status->setText(QString("共 %1 个客户端。选中客户端后可随时复制连接信息或查看密钥。").arg(list->count()));
        }, 5000);
    }, 5000);
}

void ClientPermissions::newClient()
{
    if (!available || working)
        return;
    list->setCurrentRow(-1);
    selectedId.clear();
    editor->setEnabled(true);
    name->clear();
    enabled->setChecked(true);
    canSend->setChecked(false);
    canDownload->setChecked(false);
    for (int i = 0; i < groups->count(); ++i)
        groups->item(i)->setCheckState(Qt::Unchecked);
    applyButton->setText("创建接入密钥");
    revokeButton->setEnabled(false);
    updateCredentialActions();
    name->setFocus();
}

void ClientPermissions::selectClient()
{
    if (!list->currentItem())
    {
        selectedId.clear();
        updateCredentialActions();
        return;
    }
    const auto client = list->currentItem()->data(Qt::UserRole).toJsonObject();
    selectedId = client.value("id").toString();
    name->setText(client.value("name").toString());
    enabled->setChecked(client.value("enabled").toBool());
    canSend->setChecked(client.value("can_send").toBool());
    canDownload->setChecked(client.value("can_download").toBool());
    const auto allowed = client.value("groups").toArray();
    for (int i = 0; i < groups->count(); ++i)
        groups->item(i)->setCheckState(allowed.contains(groups->item(i)->data(Qt::UserRole).toString())
            ? Qt::Checked : Qt::Unchecked);
    applyButton->setText("保存权限（立即生效）");
    editor->setEnabled(!client.value("revoked").toBool());
    revokeButton->setEnabled(!client.value("revoked").toBool());
    updateCredentialActions();
}

void ClientPermissions::apply()
{
    if (!available || working || name->text().trimmed().isEmpty())
    {
        status->setText("请输入客户端名称后保存。");
        return;
    }
    QJsonArray allowed;
    for (int i = 0; i < groups->count(); ++i)
        if (groups->item(i)->checkState() == Qt::Checked)
            allowed.append(groups->item(i)->data(Qt::UserRole).toString());
    const bool create = selectedId.isEmpty();
    QJsonObject request{{"name", name->text().trimmed()}, {"groups", allowed},
        {"enabled", enabled->isChecked()}, {"can_send", canSend->isChecked()},
        {"can_download", canDownload->isChecked()}};
    setWorking(true);
    const auto submit = [this, create, request](const QJsonObject &p2p)
    {
      transport.post(create ? "/api/local/clients" : "/api/local/clients/" + selectedId, request,
        [this, create, p2p](const QJsonValue &value, const QString &error)
    {
        setWorking(false);
        if (!error.isEmpty())
        {
            status->setText(error);
            return;
        }
        const auto client = value.toObject().value("client").toObject();
        if (!client.isEmpty())
            selectedId = client.value("id").toString();
        if (create)
        {
            const auto token = value.toObject().value("token").toString();
            if (token.isEmpty())
            {
                status->setText("服务端未返回接入密钥，请检查版本后重新创建。");
                return;
            }
            showCredential(client.value("name").toString(), token, p2p);
        }
        refresh();
    }, 5000);
    };
    if (!create)
    {
        submit({});
        return;
    }
    withConnectionInfo(submit);
}

void ClientPermissions::withConnectionInfo(std::function<void(const QJsonObject &)> callback)
{
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        setWorking(false);
        status->setText("无法读取服务端连接配置。");
        return;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
    {
        setWorking(false);
        status->setText("服务端连接配置格式错误。");
        return;
    }
    const auto config = document.object();
    const auto p2p = config.value("p2p").toObject();
    if (!p2p.value("enabled").toBool())
    {
        callback({});
        return;
    }
    transport.get("/api/local/p2p", [this, callback, p2p, config](const QJsonValue &value, const QString &error)
    {
        auto connection = p2p;
        connection.remove("enabled");
        connection.insert("server_id", value.toObject().value("server_id"));
        QString validation;
        const auto certificates = QSslCertificate::fromPath(
            QFileInfo(configPath).dir().absoluteFilePath(config.value("tls_cert").toString()));
        if (!error.isEmpty() || !value.toObject().value("enabled").toBool() ||
            !ConnectionInfo::relaySettings(connection, validation, true) || certificates.isEmpty())
        {
            setWorking(false);
            status->setText("自动连接尚未就绪。请检查公网入口、TLS 配置并启动服务后重试；尚未创建或更换密钥。");
            return;
        }
        callback(connection);
    }, 5000);
}

void ClientPermissions::existingCredential()
{
    if (working || !available || selectedId.isEmpty() || !credentialButton->isEnabled())
        return;
    const auto id = selectedId;
    setWorking(true);
    withConnectionInfo([this, id](const QJsonObject &p2p)
    {
        transport.get("/api/local/clients/" + id + "/credential",
            [this, p2p](const QJsonValue &value, const QString &error)
        {
            setWorking(false);
            if (!error.isEmpty())
            {
                status->setText(error);
                return;
            }
            const auto token = value.toObject().value("token").toString();
            if (token.isEmpty())
            {
                status->setText("此旧版密钥尚未保存原文。让原客户端重新连接后会自动补存；"
                    "也可点击“更换密钥”生成新的连接信息，原群与权限设置保留。");
                return;
            }
            showCredential(value.toObject().value("client").toObject().value("name").toString(), token, p2p);
        }, 5000);
    });
}

void ClientPermissions::rotateCredential()
{
    if (working || !available || selectedId.isEmpty() || !rotateButton->isEnabled())
        return;
    if (QMessageBox::question(this, "更换客户端密钥",
        "更换后，原密钥立即失效，已连接的客户端需要粘贴新的连接信息。"
        "客户端名称、群及权限设置保持不变。是否继续？",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    const auto id = selectedId;
    setWorking(true);
    withConnectionInfo([this, id](const QJsonObject &p2p)
    {
        transport.post("/api/local/clients/" + id + "/rotate", {},
            [this, p2p](const QJsonValue &value, const QString &error)
        {
            setWorking(false);
            if (!error.isEmpty())
            {
                status->setText(error);
                return;
            }
            const auto token = value.toObject().value("token").toString();
            if (token.isEmpty())
            {
                status->setText("服务端未返回密钥，请点击“复制连接信息”重新读取。");
                return;
            }
            showCredential(value.toObject().value("client").toObject().value("name").toString(), token, p2p);
            refresh();
        }, 5000);
    });
}

void ClientPermissions::revoke()
{
    if (selectedId.isEmpty() || working || QMessageBox::question(this, "永久撤销密钥",
        "此密钥将立即失效且不能重新启用。该客户端以后需要新建接入密钥。") != QMessageBox::Yes)
        return;
    setWorking(true);
    transport.post("/api/local/clients/" + selectedId + "/revoke", {},
        [this](const QJsonValue &, const QString &error)
    {
        setWorking(false);
        if (!error.isEmpty())
            status->setText(error);
        else
            refresh();
    }, 5000);
}

void ClientPermissions::showCredential(const QString &clientName, const QString &token, const QJsonObject &p2p)
{
    QFile file(configPath);
    file.open(QIODevice::ReadOnly);
    const auto config = QJsonDocument::fromJson(file.readAll()).object();
    QUrl endpoint("http://" + config.value("listen").toString());
    const auto certificatePath = config.value("tls_cert").toString();
    const bool tls = !certificatePath.isEmpty();
    const auto certificates = tls ? QSslCertificate::fromPath(
        QFileInfo(configPath).dir().absoluteFilePath(certificatePath)) : QList<QSslCertificate>{};
    QString pin;
    if (tls && !certificates.isEmpty())
    {
        endpoint.setScheme("https");
        pin = QString::fromLatin1(certificates.first().digest(QCryptographicHash::Sha256).toHex());
    }
    QDialog dialog(this);
    dialog.setObjectName("clientCredentialDialog");
    dialog.setWindowTitle("客户端接入信息 · " + clientName);
    dialog.resize(650, 310);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel("此客户端的密钥已保存在本机，关闭窗口后仍可在“客户端权限”中再次查看或复制。"
        "\n客户端打开连接设置，点击“粘贴连接信息”后保存。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *form = new QFormLayout;
    auto *mode = new QComboBox;
    mode->setObjectName("clientCredentialMode");
    if (!p2p.isEmpty())
        mode->addItem("自动连接（直连优先）", "p2p");
    mode->addItem("直接地址（局域网）", "direct");
    form->addRow("连接方式", mode);
    if (!p2p.isEmpty())
    {
        auto *device = new QLabel(p2p.value("server_id").toString());
        device->setObjectName("clientCredentialServerId");
        device->setWordWrap(true);
        device->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow("服务器设备 ID", device);
    }
    auto *urls = new QComboBox;
    urls->setObjectName("clientCredentialUrl");
    urls->setEditable(true);
    QHostAddress address(endpoint.host());
    if (address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6 || address == QHostAddress::Any)
    {
        for (const auto &candidate : QNetworkInterface::allAddresses())
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol && !candidate.isLoopback())
            {
                auto url = endpoint;
                url.setHost(candidate.toString());
                urls->addItem(url.toString());
            }
        endpoint.setHost(address == QHostAddress::AnyIPv4 ? "127.0.0.1" : "::1");
    }
    urls->addItem(endpoint.toString());
    QStringList directAddresses;
    for (int i = 0; i < urls->count(); ++i)
        directAddresses.append(urls->itemText(i));
    QString directAddress = urls->currentText();
    auto *addressLabel = new QLabel;
    addressLabel->setObjectName("clientCredentialAddressLabel");
    auto *editRelay = new QPushButton("修改中转设置");
    editRelay->setObjectName("clientCredentialEditRelay");
    auto *addressRow = new QHBoxLayout;
    addressRow->addWidget(urls, 1);
    addressRow->addWidget(editRelay);
    form->addRow(addressLabel, addressRow);
    auto *addressHint = new QLabel;
    addressHint->setObjectName("clientCredentialAddressHint");
    addressHint->setWordWrap(true);
    form->addRow(QString(), addressHint);
    const auto updateAddress = [mode, urls, addressLabel, addressHint, editRelay, p2p,
                                directAddresses, &directAddress]
    {
        if (!urls->lineEdit()->isReadOnly())
            directAddress = urls->currentText();
        const bool automatic = mode->currentData().toString() == "p2p";
        urls->clear();
        if (automatic)
            urls->addItem(p2p.value("relay_url").toString());
        else
        {
            urls->addItems(directAddresses);
            urls->setCurrentText(directAddress);
        }
        urls->lineEdit()->setReadOnly(automatic);
        addressLabel->setText(automatic ? "公网中转地址" : "客户端可达的服务器地址");
        addressHint->setText(automatic ? "此地址来自服务端配置，修改后服务端和客户端需使用同一入口。" :
            "填写客户端可直接访问的微信电脑地址；此处不能填写只提供 P2P 中转的服务器地址。");
        editRelay->setVisible(automatic);
    };
    connect(mode, &QComboBox::currentIndexChanged, &dialog, updateAddress);
    connect(editRelay, &QPushButton::clicked, &dialog, [this, &dialog]
    {
        dialog.accept();
        QTimer::singleShot(0, this, [this] { emit editConnectionRequested(); });
    });
    updateAddress();
    auto *secret = new QLineEdit(token);
    secret->setObjectName("clientCredentialToken");
    secret->setReadOnly(true);
    secret->setEchoMode(QLineEdit::Password);
    form->addRow("独立接入密钥", secret);
    auto *reveal = new QCheckBox("显示密钥");
    reveal->setObjectName("clientCredentialReveal");
    connect(reveal, &QCheckBox::toggled, secret,
        [secret](bool visible) { secret->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password); });
    form->addRow(QString(), reveal);
    auto *fingerprint = new QLineEdit(pin);
    fingerprint->setReadOnly(true);
    form->addRow("TLS 证书指纹", fingerprint);
    layout->addLayout(form);
    if (address.isLoopback() && p2p.isEmpty())
        layout->addWidget(new QLabel("当前仅允许本机连接。其他电脑连接前，请停止服务，在连接与存储中启用 0.0.0.0 和 TLS。"));
    auto *copy = new QPushButton("复制客户端连接信息");
    copy->setObjectName("clientCredentialCopy");
    layout->addWidget(copy);
    connect(copy, &QPushButton::clicked, &dialog, [&, pin, token, p2p]
    {
        const bool automatic = mode->currentData().toString() == "p2p";
        QJsonObject connection{{"version", 1}, {"mode", automatic ? "p2p" : "direct"},
            {"serverUrl", automatic ? "p2p://" + p2p.value("server_id").toString() : urls->currentText().trimmed()},
            {"token", token}, {"certificatePin", pin}};
        if (automatic)
            connection.insert("p2p", p2p);
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(connection).toJson()));
        copy->setText("已复制，可在客户端粘贴");
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.exec();
}
