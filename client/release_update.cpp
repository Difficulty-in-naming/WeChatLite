#include "release_update.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QSysInfo>
#include <QThreadPool>
#include <QUuid>
#include <QVersionNumber>
#include <cmath>

namespace
{
constexpr qint64 metadataLimit = 1024 * 1024;
constexpr qint64 chunkSize = 64 * 1024;
const QString apiRepository = QStringLiteral("https://api.github.com/repos/Difficulty-in-naming/WeChatLite");
const QString browserRepository = QStringLiteral("https://github.com/Difficulty-in-naming/WeChatLite");

QVersionNumber stableVersion(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\Av?(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\z"));
    const auto match = pattern.match(text);
    if (!match.hasMatch()) return {};
    return QVersionNumber(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
}
bool secure(const QUrl &url)
{
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() &&
           (url.port(-1) == -1 || url.port() == 443) && !url.authority(QUrl::FullyEncoded).contains('@') && !url.hasFragment() &&
           url.toEncoded().size() <= 8192;
}
bool exactUrl(const QJsonValue &value, const QString &expected)
{
    if (!value.isString()) return false;
    QUrl url(value.toString(), QUrl::StrictMode);
    if (url.port() == 443) url.setPort(-1);
    return secure(url) && !url.hasQuery() && url == QUrl(expected);
}
bool assetRedirect(const QUrl &url, const QUrl &original)
{
    if (!secure(url)) return false;
    if (url.host() == "github.com")
    {
        auto normalized = url;
        if (normalized.port() == 443) normalized.setPort(-1);
        return !normalized.hasQuery() && normalized == original;
    }
    return url.host() == "release-assets.githubusercontent.com" || url.host() == "objects.githubusercontent.com";
}
QString identifier(const QJsonValue &value)
{
    if (!value.isDouble()) return {};
    const auto number = value.toDouble();
    if (number < 1 || number > 9007199254740991.0 || std::floor(number) != number) return {};
    return QString::number(value.toInteger());
}
bool safeDirectory(const QString &path)
{
    if (!QDir::isAbsolutePath(path)) return false;
    auto current = QDir::cleanPath(path);
    while (!current.isEmpty())
    {
        const QFileInfo entry(current);
        if (entry.isSymLink() || entry.isJunction()) return false;
        const auto parent = entry.absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
}

ReleaseUpdate::ReleaseUpdate(Role selectedRole, const QString &currentVersion, QObject *parent)
    : QObject(parent), role(selectedRole), installedVersion(currentVersion),
      architecture(QSysInfo::buildCpuArchitecture()), network(new QNetworkAccessManager(this))
{
#ifdef Q_OS_WIN
    system = "Windows";
#elif defined(Q_OS_MACOS)
    system = "macOS";
#endif
    // The update connection has no relationship to the chat connection or its credentials.
    network->setProxy(QNetworkProxy::NoProxy);
    cacheRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("updates");
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this]
    {
        if (busy()) fail(currentState == State::Checking ? "检查更新超时，请重试" : "下载更新超时，请重试");
    });
}
ReleaseUpdate::~ReleaseUpdate() { cancel(); }
void ReleaseUpdate::setState(State next)
{
    if (currentState == next) return;
    currentState = next;
    emit stateChanged(next);
}
void ReleaseUpdate::resetAttempt()
{
    ++generation;
    deadline.stop();
    if (verificationCancelled) verificationCancelled->store(true);
    verificationCancelled.reset();
    if (reply)
    {
        auto *old = reply.data();
        reply.clear();
        disconnect(old, nullptr, this, nullptr);
        old->abort();
        old->deleteLater();
    }
    if (output) output->close();
    output.reset();
    hash.reset();
    if (!partialPath.isEmpty()) QFile::remove(partialPath);
    if (!workDirectory.isEmpty()) QDir().rmdir(workDirectory);
    partialPath.clear();
    workDirectory.clear();
    metadataBytes.clear();
    received = discarded = 0;
    redirects = 0;
}
void ReleaseUpdate::fail(const QString &message)
{
    resetAttempt();
    readyPath.clear();
    failure = message;
    const auto attempt = generation;
    setState(State::Failed);
    if (generation == attempt && currentState == State::Failed) emit failed(message);
}
void ReleaseUpdate::cancel()
{
    if (!busy()) return;
    resetAttempt();
    readyPath.clear();
    failure.clear();
    setState(State::Cancelled);
}
void ReleaseUpdate::check()
{
    if (busy()) return;
    resetAttempt();
    failure.clear();
    readyPath.clear();
    latestVersion.clear();
    asset = {};
    const auto installed = stableVersion(installedVersion);
    if (installed.isNull()) { fail("当前程序版本无效，无法比较更新"); return; }
    const auto selectedRole = role == Role::Client ? QString("Client") : role == Role::Server ? QString("Server") : QString();
    if (selectedRole.isEmpty()) { fail("无法识别当前应用类型"); return; }
    if (system == "Windows" && architecture == "x86_64")
        asset.name = "WeChatLite-" + selectedRole + "-Windows-x64.exe";
    else if (system == "macOS" && architecture == "arm64")
        asset.name = "WeChatLite-" + selectedRole + "-macOS-arm64.dmg";
    else { fail("当前系统架构暂无可用更新包"); return; }
    const auto attempt = generation;
    setState(State::Checking);
    if (generation != attempt || currentState != State::Checking) return;
    deadline.start(checkTimeoutMs);
    request(QUrl(apiRepository + "/releases/latest"), true);
}
void ReleaseUpdate::request(const QUrl &url, bool metadata)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "WeChatLite-Update/1");
    request.setRawHeader("Accept", metadata ? "application/vnd.github+json" : "application/octet-stream");
    request.setRawHeader("Accept-Encoding", "identity");
    if (metadata) request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    auto tls = QSslConfiguration::defaultConfiguration();
    tls.setLocalCertificateChain({});
    tls.setPrivateKey(QSslKey());
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(tls);
    request.setTransferTimeout(30000);
    auto *response = network->get(request);
    reply = response;
    response->setReadBufferSize(256 * 1024);
    const auto attempt = generation;
    connect(response, &QNetworkReply::readyRead, this, [this, response, metadata, attempt]
    {
        if (generation == attempt && reply == response) consume(response, metadata);
    });
    connect(response, &QNetworkReply::finished, this, [this, response, metadata, attempt]
    {
        if (generation == attempt && reply == response) finish(response, metadata);
    });
}
void ReleaseUpdate::consume(QNetworkReply *response, bool metadata)
{
    const auto status = response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    while (response->bytesAvailable() > 0)
    {
        const auto chunk = response->read(chunkSize);
        if (chunk.isEmpty()) break;
        if (status != 200)
        {
            discarded += chunk.size();
            if (discarded > metadataLimit) { fail("更新响应过大，已停止请求"); return; }
            continue;
        }
        if (metadata)
        {
            if (metadataBytes.size() + chunk.size() > metadataLimit)
                { fail("更新信息过大，已停止请求"); return; }
            metadataBytes.append(chunk);
        }
        else
        {
            const auto encoding = response->rawHeader("Content-Encoding").trimmed().toLower();
            bool lengthValid = true;
            const auto length = response->hasRawHeader("Content-Length")
                ? response->rawHeader("Content-Length").toLongLong(&lengthValid) : asset.size;
            if ((!encoding.isEmpty() && encoding != "identity") || !lengthValid || length != asset.size ||
                received + chunk.size() > asset.size || received + chunk.size() > maximumAssetSize)
                { fail("更新包大小或传输格式不匹配"); return; }
            if (!output || output->write(chunk) != chunk.size())
                { fail("无法写入更新缓存，请检查磁盘空间和目录权限"); return; }
            hash->addData(chunk);
            received += chunk.size();
            emit downloadProgress(received, asset.size);
            if (reply != response || currentState != State::Downloading) return;
        }
    }
}
void ReleaseUpdate::finish(QNetworkReply *response, bool metadata)
{
    consume(response, metadata);
    if (reply != response) return;
    const auto status = response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto target = response->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (status >= 300 && status < 400)
    {
        const auto redirected = response->url().resolved(target);
        if (metadata || target.isEmpty() || !assetRedirect(redirected, asset.url) || ++redirects > 5)
            { fail("更新下载地址不受信任或跳转次数过多"); return; }
        reply.clear();
        response->deleteLater();
        discarded = 0;
        request(redirected, false);
        return;
    }
    if (response->error() != QNetworkReply::NoError || status != 200)
    {
        fail(status > 0 && status != 200 ? QString("GitHub 更新请求失败（HTTP %1），请稍后重试").arg(status)
                        : QString("无法连接 GitHub，请检查网络后重试"));
        return;
    }
    reply.clear();
    response->deleteLater();
    if (metadata) selectRelease();
    else completeDownload();
}
void ReleaseUpdate::selectRelease()
{
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(metadataBytes, &parse);
    metadataBytes.clear();
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        { fail("GitHub 返回的更新信息无效"); return; }
    const auto release = document.object();
    const auto tag = release["tag_name"].toString();
    const auto version = stableVersion(tag);
    const auto releaseId = identifier(release["id"]);
    if (releaseId.isEmpty() || !exactUrl(release["url"], apiRepository + "/releases/" + releaseId) ||
        !exactUrl(release["html_url"], browserRepository + "/releases/tag/" + tag))
        { fail("更新发布信息与指定仓库不匹配"); return; }
    if (version.isNull() || !release["draft"].isBool() || release["draft"].toBool() ||
        !release["prerelease"].isBool() || release["prerelease"].toBool())
        { fail("GitHub 未提供有效的正式版本"); return; }
    latestVersion = version.toString();
    if (QVersionNumber::compare(version, stableVersion(installedVersion)) <= 0)
    {
        deadline.stop();
        const auto attempt = generation;
        setState(State::UpToDate);
        if (generation == attempt && currentState == State::UpToDate) emit upToDate();
        return;
    }
    int matches = 0;
    for (const auto &value : release["assets"].toArray())
    {
        const auto candidate = value.toObject();
        if (candidate["name"] != asset.name) continue;
        ++matches;
        const auto assetId = identifier(candidate["id"]);
        static const QRegularExpression sha(QStringLiteral("\\Asha256:([0-9a-fA-F]{64})\\z"));
        const auto digest = sha.match(candidate["digest"].toString());
        const auto size = candidate["size"].toDouble(-1);
        if (assetId.isEmpty() || candidate["state"] != "uploaded" || !digest.hasMatch() ||
            size < 1 || size > maximumAssetSize || std::floor(size) != size ||
            !exactUrl(candidate["url"], apiRepository + "/releases/assets/" + assetId) ||
            !exactUrl(candidate["browser_download_url"], browserRepository + "/releases/download/" + tag + '/' + asset.name))
            { fail("对应更新包的信息、大小或 SHA-256 校验值无效"); return; }
        asset.url = QUrl(candidate["browser_download_url"].toString(), QUrl::StrictMode);
        asset.size = qint64(size);
        asset.digest = QByteArray::fromHex(digest.captured(1).toLatin1());
    }
    if (matches != 1) { fail("此版本没有唯一匹配的系统与应用更新包"); return; }
    deadline.start(downloadTimeoutMs);
    const auto attempt = generation;
    setState(State::Downloading);
    if (generation != attempt || currentState != State::Downloading) return;
    prepareDownload();
}
void ReleaseUpdate::prepareDownload()
{
    if (cachedVersion != latestVersion || cachedDigest != asset.digest || cachedSize != asset.size || cachedPath.isEmpty())
        { startDownload(); return; }
    verificationCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = verificationCancelled;
    const auto path = cachedPath;
    const auto expected = asset;
    const auto attempt = generation;
    QPointer<ReleaseUpdate> self(this);
    QThreadPool::globalInstance()->start([self, cancelled, path, expected, attempt]
    {
        QFile input(path);
        const QFileInfo info(path);
        bool valid = !info.isSymLink() && !info.isJunction() && info.isFile() && info.size() == expected.size &&
                     input.open(QIODevice::ReadOnly);
        QCryptographicHash digest(QCryptographicHash::Sha256);
        qint64 total = 0;
        while (valid && !cancelled->load() && !input.atEnd())
        {
            const auto chunk = input.read(chunkSize);
            if (chunk.isEmpty()) { valid = false; break; }
            total += chunk.size();
            if (total > expected.size) { valid = false; break; }
            digest.addData(chunk);
        }
        valid = valid && input.error() == QFile::NoError && total == expected.size && digest.result() == expected.digest;
        input.close();
        if (cancelled->load()) return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, path, valid, attempt]
        {
            if (!self || self->generation != attempt || self->currentState != State::Downloading) return;
            self->verificationCancelled.reset();
            if (!valid) { self->startDownload(); return; }
            self->readyPath = path;
            self->deadline.stop();
            emit self->downloadProgress(self->asset.size, self->asset.size);
            if (self->generation != attempt || self->currentState != State::Downloading) return;
            const auto version = self->latestVersion;
            self->setState(State::Ready);
            if (self->generation == attempt && self->currentState == State::Ready) emit self->ready(version, path);
        });
    });
}
void ReleaseUpdate::startDownload()
{
    if (!safeDirectory(cacheRoot) || !QDir().mkpath(cacheRoot))
        { fail("无法创建安全的更新缓存目录"); return; }
    workDirectory = QDir(cacheRoot).filePath(latestVersion + '-' + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkdir(workDirectory)) { fail("无法创建更新下载目录"); return; }
    QFile::setPermissions(workDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    partialPath = QDir(workDirectory).filePath(asset.name + ".partial");
    output = std::make_unique<QFile>(partialPath);
    if (!output->open(QIODevice::WriteOnly | QIODevice::NewOnly))
        { fail("无法创建更新临时文件"); return; }
    output->setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    received = discarded = 0;
    redirects = 0;
    const auto attempt = generation;
    emit downloadProgress(0, asset.size);
    if (generation != attempt || currentState != State::Downloading) return;
    request(asset.url, false);
}
void ReleaseUpdate::completeDownload()
{
    if (!output || received != asset.size || hash->result() != asset.digest)
        { fail("更新包 SHA-256 或大小校验失败，请重新检查更新"); return; }
    if (!output->flush()) { fail("无法保存更新包，请检查磁盘空间"); return; }
    output->close();
    const auto target = QDir(workDirectory).filePath(asset.name);
    if (!QFile::rename(partialPath, target)) { fail("无法完成更新包保存"); return; }
    partialPath.clear();
    workDirectory.clear();
    output.reset();
    hash.reset();
    readyPath = cachedPath = target;
    cachedVersion = latestVersion;
    cachedDigest = asset.digest;
    cachedSize = asset.size;
    deadline.stop();
    const auto attempt = generation;
    const auto version = latestVersion;
    setState(State::Ready);
    if (generation == attempt && currentState == State::Ready) emit ready(version, target);
}
