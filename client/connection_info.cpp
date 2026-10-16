#include "connection_info.h"
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QUrl>

namespace
{
bool boundedString(const QJsonObject &object, const char *key, qsizetype maximum, bool required = true)
{
    const auto value = object.value(key);
    return (!required && value.isUndefined()) ||
           (value.isString() && value.toString().toUtf8().size() <= maximum &&
            (!required || !value.toString().isEmpty()));
}

bool objectFromBytes(const QByteArray &bytes, QJsonObject &object)
{
    if (bytes.size() > ConnectionInfo::maximumBytes)
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    object = document.object();
    return true;
}

bool pinValid(QString pin)
{
    pin = pin.remove(':').remove(' ').trimmed();
    return QRegularExpression("^[0-9a-fA-F]{64}$").match(pin).hasMatch();
}
} // namespace

bool ConnectionInfo::relaySettings(const QJsonObject &settings, QString &error, bool requireServerId)
{
    error = "公网入口信息无效，请复制管理员提供的完整 JSON。";
    if (!boundedString(settings, "relay_url", 2048) ||
        !boundedString(settings, "relay_token", 512) ||
        !boundedString(settings, "relay_ca", 16 * 1024) ||
        !settings.value("relay_quic_port").isDouble())
        return false;
    const auto port = settings.value("relay_quic_port").toDouble();
    if (port < 1 || port > 65535 || port != int(port))
        return false;
    const auto rawUrl = settings.value("relay_url").toString();
    const QUrl url(rawUrl, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || url.port() == 0 ||
        url.authority(QUrl::FullyEncoded).contains('@') || url.hasQuery() || url.hasFragment() ||
        (!url.path().isEmpty() && url.path() != "/") ||
        (url.scheme() != "https" && !(url.scheme() == "http" && QHostAddress(url.host()).isLoopback())))
        return false;
    if (rawUrl != rawUrl.trimmed() || rawUrl.contains(QRegularExpression("[\\x00-\\x20\\x7f]")) ||
        settings.value("relay_token").toString().contains(QRegularExpression("[\\x00-\\x20\\x7f]")))
        return false;
    if (QSslCertificate::fromData(settings.value("relay_ca").toString().toUtf8()).isEmpty())
        return false;
    if (requireServerId &&
        (!boundedString(settings, "server_id", 128) ||
         !QRegularExpression("^[a-z0-9]{1,128}$").match(settings.value("server_id").toString()).hasMatch()))
        return false;
    error.clear();
    return true;
}

bool ConnectionInfo::parse(const QByteArray &bytes, QJsonObject &connection, QString &error)
{
    error = "连接信息无效，请复制服务端提供的 JSON（直接连接最多 8 KiB，自动连接最多 64 KiB）。";
    QJsonObject input;
    if (!objectFromBytes(bytes, input) || !boundedString(input, "serverUrl", 2048) ||
        !boundedString(input, "mode", 16, false) ||
        !boundedString(input, "token", 512) || !boundedString(input, "certificatePin", 256, false) ||
        input.value("token").toString().trimmed().size() < 24)
        return false;
    const auto mode = input.value("mode").toString("direct");
    if (mode == "p2p")
    {
        if (!input.value("version").isDouble() || input.value("version").toDouble() != 1 ||
            !input.value("p2p").isObject() ||
            !pinValid(input.value("certificatePin").toString()))
            return false;
        QString detail;
        if (!relaySettings(input.value("p2p").toObject(), detail, true))
            return false;
        const auto id = input.value("p2p").toObject().value("server_id").toString();
        if (input.value("serverUrl").toString() != "p2p://" + id)
            return false;
    }
    else if (mode == "direct")
    {
        if (bytes.size() > 8192 || (input.contains("version") &&
            (!input.value("version").isDouble() || input.value("version").toDouble() != 1)))
            return false;
        const QUrl url(input.value("serverUrl").toString().trimmed(), QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty() || url.authority(QUrl::FullyEncoded).contains('@') ||
            url.hasQuery() || url.hasFragment() ||
            (url.scheme() != "https" && url.scheme() != "http") ||
            (url.scheme() == "http" && url.host() != "localhost" && !QHostAddress(url.host()).isLoopback()))
            return false;
    }
    else
        return false;
    connection = QJsonObject{{"version", 1}, {"mode", mode},
        {"serverUrl", input.value("serverUrl").toString().trimmed()},
        {"token", input.value("token").toString().trimmed()},
        {"certificatePin", input.value("certificatePin").toString().trimmed()}};
    if (mode == "p2p")
        connection.insert("p2p", input.value("p2p"));
    error.clear();
    return true;
}

bool ConnectionInfo::publicEntry(const QByteArray &bytes, QJsonObject &settings, QString &error)
{
    error = "公网入口信息无效，请复制完整 JSON（最多 64 KiB）。";
    QJsonObject input;
    if (!objectFromBytes(bytes, input))
        return false;
    if (input.value("p2p").isObject())
        input = input.value("p2p").toObject();
    if (!relaySettings(input, error))
        return false;
    settings = QJsonObject{{"relay_url", input.value("relay_url")},
        {"relay_token", input.value("relay_token")}, {"relay_ca", input.value("relay_ca")},
        {"relay_quic_port", input.value("relay_quic_port")}};
    return true;
}
