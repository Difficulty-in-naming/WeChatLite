#include "transport.h"
#include "attachment.h"
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QUuid>
#include <memory>

namespace
{
QByteArray normalizedPin(const QString &value)
{
    return value.toLower().remove(':').remove(' ').trimmed().toUtf8();
}

bool matchesPin(const QSslCertificate &certificate, const QByteArray &expected)
{
    return expected.size() == 64 && !certificate.isNull() &&
           certificate.digest(QCryptographicHash::Sha256).toHex() == expected;
}

QSslConfiguration tlsConfiguration(const QByteArray &expectedPin)
{
    auto configuration = QSslConfiguration::defaultConfiguration();
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    if (!expectedPin.isEmpty())
    {
        // An explicit empty CA list disables implicit system roots. Every peer,
        // including a normally trusted one, must pass our leaf-pin sslErrors gate
        // before QSslSocket releases any buffered HTTP/WebSocket credentials.
        configuration.setCaCertificates({});
    }
    return configuration;
}

QString pinError()
{
    return QStringLiteral("服务器证书指纹不匹配，已拒绝连接");
}
} // namespace

Transport::Transport(QObject *parent) : QObject(parent)
{
    reconnect.setSingleShot(true);
    connect(&reconnect, &QTimer::timeout, this, &Transport::connectEvents);
    connect(&socket, &QWebSocket::connected, this,
            [this]
            {
                if (!eventPin.isEmpty() &&
                    !matchesPin(socket.sslConfiguration().peerCertificate(), eventPin))
                {
                    socket.abort();
                    emit warning(pinError());
                    return;
                }
                delay = 1000;
                emit connectionChanged(true);
            });
    connect(&socket, &QWebSocket::disconnected, this,
            [this]
            {
                emit connectionChanged(false);
                if (!stopping)
                {
                    reconnect.start(delay);
                    delay = qMin(delay * 2, 30000);
                }
            });
    connect(&socket, &QWebSocket::textMessageReceived, this,
            [this](const QString &text)
            {
                QJsonParseError error;
                auto object = QJsonDocument::fromJson(text.toUtf8(), &error).object();
                if (error.error != QJsonParseError::NoError)
                    return;
                if (object["type"] == "reset")
                {
                    cursor = 0;
                    QSettings().setValue("cursors/" + identity, cursor);
                    emit event(object);
                    return;
                }
                auto seq = object["seq"].toInteger();
                if (seq > 0 && object["type"] != "synced" && seq <= cursor)
                    return;
                if (seq > cursor)
                {
                    cursor = seq;
                    QSettings().setValue("cursors/" + identity, cursor);
                }
                emit event(object);
            });
    connect(&socket, &QWebSocket::sslErrors, this,
            [this](const QList<QSslError> &errors)
            {
                if (!eventPin.isEmpty() &&
                    matchesPin(socket.sslConfiguration().peerCertificate(), eventPin))
                    socket.ignoreSslErrors(errors);
                else
                {
                    socket.abort();
                    emit warning("服务器证书校验失败，请检查服务端证书或配置证书 SHA-256 指纹");
                }
            }, Qt::DirectConnection);
    connect(&socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError)
            {
                emit warning(socket.errorString());
                if (!stopping && !reconnect.isActive())
                    reconnect.start(delay);
            });
}
void Transport::configure(const QString &url, const QString &secret, const QString &certificatePin)
{
    cancelSend();
    disconnectService();
    // Do not reuse a TLS connection approved under a previous endpoint/pin.
    const auto active = network.findChildren<QNetworkReply *>();
    for (auto *reply : active)
        if (!reply->isFinished())
            reply->abort();
    network.clearAccessCache();
    base = QUrl(url.trimmed());
    if (base.path().endsWith('/'))
    {
        auto path = base.path();
        path.chop(1);
        base.setPath(path);
    }
    token = secret.toUtf8();
    pin = normalizedPin(certificatePin);
    identity = QString::fromLatin1(QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex());
    cursor = QSettings().value("cursors/" + identity, -1).toLongLong();
    stopping = false;
}
void Transport::disconnectService()
{
    stopping = true;
    reconnect.stop();
    socket.abort();
}
void Transport::setOwnedServer(const QString &url, const QString &secret, const QString &certificatePin)
{
    ownedServer = QUrl(url);
    ownedToken = secret.toUtf8();
    ownedPin = normalizedPin(certificatePin);
}
void Transport::shutdownLocalServer(QProcess *process)
{
    if (process->state() == QProcess::NotRunning)
        return;
    if (ownedServer.isValid() && QStringList{"127.0.0.1", "localhost", "::1"}.contains(ownedServer.host()))
    {
        QEventLoop loop;
        QPointer<QEventLoop> guard(&loop);
        // A dedicated manager cannot inherit pooled connections or credentials
        // from a different chat server configured after this process was started.
        QNetworkAccessManager shutdownNetwork;
        QNetworkRequest request(ownedServer.resolved(QUrl("/api/shutdown")));
        request.setRawHeader("Authorization", "Bearer " + ownedToken);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setSslConfiguration(tlsConfiguration(ownedPin));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setTransferTimeout(1500);
        if (!ownedPin.isEmpty() && ownedServer.scheme() != "https")
            request.setUrl(QUrl());
        auto *reply = shutdownNetwork.post(request, "{}");
        setupTls(reply, ownedPin);
        connect(reply, &QNetworkReply::finished, &loop,
                [guard]
                {
                    if (guard)
                        guard->quit();
                });
        QTimer::singleShot(1700, &loop, &QEventLoop::quit);
        loop.exec();
        reply->deleteLater();
    }
    if (!process->waitForFinished(3500))
    {
        process->terminate();
        if (!process->waitForFinished(1000))
        {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}
QNetworkRequest Transport::request(const QString &path) const
{
    QUrl url = base;
    auto relative = QUrl(path);
    url.setPath(base.path() + relative.path());
    url.setQuery(relative.query());
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + token);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setSslConfiguration(tlsConfiguration(pin));
    if (!pin.isEmpty() && url.scheme() != "https")
        request.setUrl(QUrl());
    request.setTransferTimeout(180000);
    return request;
}
void Transport::connectEvents()
{
    if (stopping || !base.isValid() || token.isEmpty())
        return;
    if (!pin.isEmpty() && base.scheme() != "https")
    {
        emit warning("配置证书指纹的服务器必须使用 HTTPS");
        return;
    }
    auto req = request("/api/events?after=" + QString::number(cursor));
    auto url = req.url();
    url.setScheme(url.scheme() == "https" ? "wss" : "ws");
    req.setUrl(url);
    eventPin = pin;
    socket.ignoreSslErrors(QList<QSslError>{});
    socket.setSslConfiguration(tlsConfiguration(eventPin));
    socket.open(req);
}
void Transport::setupTls(QNetworkReply *reply, const QByteArray &expectedPin)
{
    connect(reply, &QNetworkReply::sslErrors, this,
            [this, reply, expectedPin](const QList<QSslError> &errors)
            {
                if (expectedPin.isEmpty())
                    return;
                if (matchesPin(reply->sslConfiguration().peerCertificate(), expectedPin))
                    reply->ignoreSslErrors(errors);
                else
                {
                    reply->setProperty("pinError", pinError());
                    reply->abort();
                    emit warning(pinError());
                }
            }, Qt::DirectConnection);
    connect(reply, &QNetworkReply::encrypted, this,
            [this, reply, expectedPin]
            {
                if (!expectedPin.isEmpty() &&
                    !matchesPin(reply->sslConfiguration().peerCertificate(), expectedPin))
                {
                    reply->setProperty("pinError", pinError());
                    reply->abort();
                    emit warning(pinError());
                }
            }, Qt::DirectConnection);
}
void Transport::finishJson(QNetworkReply *reply, JsonCallback callback)
{
    setupTls(reply, pin);
    reply->setReadBufferSize(128 * 1024);
    auto data = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this,
            [reply, data]
            {
                if (reply->isReadable())
                    data->append(reply->readAll());
                if (data->size() > 8 * 1024 * 1024)
                    reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this,
            [reply, callback, data]
            {
                if (reply->isReadable())
                    data->append(reply->readAll());
                QJsonParseError parse;
                auto document = QJsonDocument::fromJson(*data, &parse);
                auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                QString error;
                if (!reply->property("pinError").toString().isEmpty())
                    error = reply->property("pinError").toString();
                else if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
                    error = document.object()["error"].toString(reply->errorString());
                else if (parse.error != QJsonParseError::NoError)
                    error = "服务器返回无效数据";
                callback(document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object()),
                         error);
                reply->deleteLater();
            });
}
void Transport::get(const QString &path, JsonCallback callback, int timeoutMs)
{
    auto req = request(path);
    req.setTransferTimeout(timeoutMs);
    finishJson(network.get(req), std::move(callback));
}
void Transport::post(const QString &path, const QJsonObject &object, JsonCallback callback, int timeoutMs)
{
    auto req = request(path);
    req.setTransferTimeout(timeoutMs);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply = network.post(req, QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (path == "/api/messages")
        trackSend(reply);
    finishJson(reply, std::move(callback));
}
void Transport::trackSend(QNetworkReply *reply)
{
    sendReplies.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { sendReplies.remove(reply); });
}
void Transport::cancelSend()
{
    const auto pending = sendReplies;
    sendReplies.clear();
    for (auto *reply : pending)
        reply->abort();
}
void Transport::upload(const QString &group, const QString &path, const QString &name, JsonCallback callback)
{
    auto *file = new QFile(path);
    if (!file->open(QIODevice::ReadOnly))
    {
        auto error = file->errorString();
        delete file;
        callback({}, error);
        return;
    }
    auto req = request("/api/media?group=" + QString::fromLatin1(QUrl::toPercentEncoding(group)) +
                       "&name=" + QString::fromLatin1(QUrl::toPercentEncoding(name)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
    auto *reply = network.post(req, file);
    trackSend(reply);
    file->setParent(reply);
    connect(reply, &QNetworkReply::uploadProgress, this,
            [this, reply](qint64 done, qint64 total)
            {
                if (sendReplies.contains(reply))
                    emit progress(done, total);
            });
    finishJson(reply, std::move(callback));
}
void Transport::download(const QJsonObject &media, const QString &root,
                         std::function<void(const QString &, const QString &)> callback)
{
    if (root.isEmpty() || !QDir(root).isAbsolute())
    {
        callback({}, "请选择完整的附件保存目录");
        return;
    }
    auto directory = QDir(root).filePath(QDate::currentDate().toString("yyyy-MM-dd"));
    if (!QDir().mkpath(directory))
    {
        callback({}, "无法创建保存目录");
        return;
    }
    auto partial =
        QDir(directory).filePath("." + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".part");
    auto file = std::make_shared<QFile>(partial);
    if (!file->open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        callback({}, file->errorString());
        return;
    }
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto failed = std::make_shared<QString>();
    auto *reply = network.get(request("/api/media/" + media["id"].toString()));
    reply->setReadBufferSize(64 * 1024);
    setupTls(reply, pin);
    auto drain = [reply, file, hash, failed]
    {
        while (reply->bytesAvailable() > 0)
        {
            auto chunk = reply->read(64 * 1024);
            if (chunk.isEmpty())
                break;
            if (file->write(chunk) != chunk.size())
            {
                *failed = "磁盘写入失败：" + file->errorString();
                reply->abort();
                return;
            }
            hash->addData(chunk);
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(reply, &QNetworkReply::downloadProgress, this, &Transport::progress);
    connect(reply, &QNetworkReply::finished, this,
            [reply, file, hash, failed, callback, media, directory, partial, drain]
            {
                drain();
                auto length = file->size();
                file->close();
                auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                QString error = *failed;
                if (error.isEmpty() && (reply->error() != QNetworkReply::NoError || status != 200))
                    error = reply->errorString();
                if (error.isEmpty() && (length != media["size"].toInteger() ||
                                        hash->result().toHex() != media["sha256"].toString().toLatin1()))
                    error = "下载内容校验失败，请重新下载";
                QString destination;
                if (error.isEmpty())
                {
                    auto name = AttachmentStore::safeName(media["name"].toString());
                    destination = QDir(directory).filePath(name);
                    int counter = 0;
                    while (QFileInfo::exists(destination))
                    {
                        ++counter;
                        auto info = QFileInfo(name);
                        destination = QDir(directory).filePath(
                            info.completeBaseName() + " (" + QString::number(counter) + ")" +
                            (info.suffix().isEmpty() ? "" : "." + info.suffix()));
                    }
                    if (!QFile::rename(partial, destination))
                        error = "无法保存下载文件";
                }
                if (!error.isEmpty())
                    QFile::remove(partial);
                callback(error.isEmpty() ? destination : QString(), error);
                reply->deleteLater();
            });
}
