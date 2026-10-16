#include "release_update.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QQueue>
#include <QTemporaryDir>
#include <QtTest>
#include <cstring>

namespace
{
const QString api = "https://api.github.com/repos/Difficulty-in-naming/WeChatLite";
const QString web = "https://github.com/Difficulty-in-naming/WeChatLite";
const QString clientName = "WeChatLite-Client-Windows-x64.exe";
const QByteArray payload = QByteArray("offline synthetic updater fixture ").repeated(6000);
struct Plan
{
    QUrl url;
    QByteArray body;
    int status = 200;
    QUrl redirect;
    bool stall = false;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    qint64 contentLength = -1;
};
struct ReadStats { qint64 maximumRead = 0, bufferSize = 0; bool aborted = false; };
class FakeReply final : public QNetworkReply
{
  public:
    FakeReply(const QNetworkRequest &request, Plan response, std::shared_ptr<ReadStats> stats, QObject *parent)
        : QNetworkReply(parent), plan(std::move(response)), stats(std::move(stats))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, plan.status);
        if (!plan.redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute, plan.redirect);
        if (plan.contentLength != -2)
            setRawHeader("Content-Length", QByteArray::number(plan.contentLength >= 0 ? plan.contentLength : plan.body.size()));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] { emit metaDataChanged(); pump(); });
    }
    void abort() override
    {
        if (isFinished()) return;
        stats->aborted = true;
        buffer.clear();
        setError(OperationCanceledError, "offline cancellation");
        setFinished(true);
        emit errorOccurred(OperationCanceledError);
        emit finished();
    }
    qint64 bytesAvailable() const override { return buffer.size() + QIODevice::bytesAvailable(); }
  protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        stats->maximumRead = qMax(stats->maximumRead, maximum);
        stats->bufferSize = readBufferSize();
        const auto count = qMin(maximum, qint64(buffer.size()));
        if (!count) return isFinished() ? -1 : 0;
        std::memcpy(data, buffer.constData(), size_t(count));
        buffer.remove(0, count);
        return count;
    }
  private:
    void pump()
    {
        if (isFinished() || plan.stall) return;
        if (offset < plan.body.size())
        {
            const auto chunk = plan.body.mid(offset, 16384);
            offset += chunk.size();
            buffer.append(chunk);
            emit readyRead();
            if (!isFinished()) QTimer::singleShot(0, this, [this] { pump(); });
            return;
        }
        if (plan.error != NoError)
        {
            setError(plan.error, "offline transport error");
            emit errorOccurred(plan.error);
        }
        setFinished(true);
        emit finished();
    }
    Plan plan;
    std::shared_ptr<ReadStats> stats;
    QByteArray buffer;
    qsizetype offset = 0;
};
class FakeNetwork final : public QNetworkAccessManager
{
  public:
    explicit FakeNetwork(QObject *parent) : QNetworkAccessManager(parent) {}
    QQueue<Plan> plans;
    QList<QNetworkRequest> requests;
    QList<std::shared_ptr<ReadStats>> stats;
    bool expectedRequests = true;
  protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override
    {
        requests.append(request);
        Plan plan;
        if (plans.isEmpty()) { expectedRequests = false; plan.status = 500; }
        else plan = plans.dequeue();
        expectedRequests &= operation == GetOperation && (plan.url.isEmpty() || plan.url == request.url());
        auto read = std::make_shared<ReadStats>();
        stats.append(read);
        return new FakeReply(request, std::move(plan), read, this);
    }
};
QJsonObject release(const QString &tag = "v0.3.2", const QString &name = clientName, const QByteArray &bytes = payload)
{
    return {{"id", 123}, {"url", api + "/releases/123"}, {"html_url", web + "/releases/tag/" + tag},
        {"tag_name", tag}, {"draft", false}, {"prerelease", false},
        {"assets", QJsonArray{QJsonObject{{"id", 456}, {"name", name}, {"state", "uploaded"},
            {"url", api + "/releases/assets/456"}, {"browser_download_url", web + "/releases/download/" + tag + '/' + name},
            {"size", bytes.size()},
            {"digest", "sha256:" + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}}}};
}
Plan metadata(const QJsonObject &value) { return {QUrl(api + "/releases/latest"), QJsonDocument(value).toJson(QJsonDocument::Compact)}; }
Plan download(const QJsonObject &value, const QByteArray &body = payload)
{
    return {QUrl(value["assets"].toArray().first().toObject()["browser_download_url"].toString()), body};
}
QStringList partials(const QString &directory)
{
    QStringList result;
    QDirIterator files(directory, {"*.partial"}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) result.append(files.next());
    return result;
}
}

class UpdateTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<ReleaseUpdate> make(const QString &directory, ReleaseUpdate::Role role = ReleaseUpdate::Role::Client,
                                      const QString &current = "0.3.1", const QString &system = "Windows",
                                      const QString &architecture = "x86_64")
    {
        auto updater = std::make_unique<ReleaseUpdate>(role, current);
        delete updater->network;
        updater->network = new FakeNetwork(updater.get());
        updater->system = system;
        updater->architecture = architecture;
        updater->cacheRoot = QDir(directory).filePath("updates");
        return updater;
    }
    FakeNetwork *network(const std::unique_ptr<ReleaseUpdate> &updater)
        { return static_cast<FakeNetwork *>(updater->network); }
  private slots:
    void updateProxyUsesEachUrlPreservesCredentialsAndBypassesLoopback()
    {
        const QNetworkProxy http(QNetworkProxy::HttpProxy, "system-http.invalid", 8123,
            "system-http-user", "synthetic-http-password");
        const QNetworkProxy socks(QNetworkProxy::Socks5Proxy, "system-socks.invalid", 9123,
            "system-socks-user", "synthetic-socks-password");
        QList<QNetworkProxy> policy{http, QNetworkProxy::NoProxy, socks};
        QList<QUrl> queries;
        std::unique_ptr<QNetworkProxyFactory> factory(ReleaseUpdate::updateProxyFactory(
            [&](const QNetworkProxyQuery &query)
            {
                queries.append(query.url());
                return policy;
            }));
        const QList<QUrl> urls{QUrl(api + "/releases/latest"),
            QUrl(web + "/releases/download/v0.3.2/" + clientName),
            QUrl("https://release-assets.githubusercontent.com/file?signature=fixture")};
        for (const auto &url : urls)
            QCOMPARE(factory->queryProxy(QNetworkProxyQuery(url)), policy);
        QCOMPARE(queries, urls);
        for (const auto &url : {"https://localhost/fixture", "https://LOCALHOST./fixture",
                 "https://127.0.0.2/fixture", "https://[::1]/fixture"})
            QCOMPARE(factory->queryProxy(QNetworkProxyQuery(QUrl(url))),
                QList<QNetworkProxy>{QNetworkProxy::NoProxy});
        QCOMPARE(queries, urls); // Bypassing a fixture must not invoke system PAC discovery.
        QCOMPARE(factory->queryProxy(QNetworkProxyQuery(QUrl("https://localhost.example/fixture"))), policy);
        QCOMPARE(queries.size(), urls.size() + 1);
        policy = {QNetworkProxy::DefaultProxy, http, QNetworkProxy::NoProxy};
        QCOMPARE(factory->queryProxy(QNetworkProxyQuery(urls.first())),
            (QList<QNetworkProxy>{QNetworkProxy::NoProxy, http, QNetworkProxy::NoProxy}));
        policy.clear();
        QCOMPARE(factory->queryProxy(QNetworkProxyQuery(urls.last())),
            QList<QNetworkProxy>{QNetworkProxy::NoProxy});
        ReleaseUpdate updater(ReleaseUpdate::Role::Client, "0.3.1");
        QVERIFY(updater.network->proxyFactory());
        QCOMPARE(updater.network->proxyFactory()->queryProxy(QNetworkProxyQuery(QUrl("https://127.0.0.1/fixture"))),
            QList<QNetworkProxy>{QNetworkProxy::NoProxy});
    }
    void proxyAuthenticationNeverUsesOriginOrChatCredentials_data()
    {
        QTest::addColumn<bool>("credentials");
        QTest::newRow("system-proxy-credentials") << true;
        QTest::newRow("no-proxy-credentials") << false;
    }
    void proxyAuthenticationNeverUsesOriginOrChatCredentials()
    {
        QFETCH(bool, credentials);
        QTemporaryDir directory;
        QTcpServer proxy;
        QVERIFY(proxy.listen(QHostAddress::LocalHost, 0));
        QList<QByteArray> exchanges;
        const QByteArray expected = "Basic " + QByteArray("system-proxy-user:synthetic-proxy-password").toBase64();
        bool authenticated = false;
        connect(&proxy, &QTcpServer::newConnection, this, [&]
        {
            while (auto *socket = proxy.nextPendingConnection())
            {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer]
                {
                    buffer->append(socket->readAll());
                    if (!buffer->contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                    socket->setProperty("handled", true);
                    exchanges.append(*buffer);
                    QByteArray authorization;
                    for (const auto &line : buffer->split('\n'))
                        if (line.toLower().startsWith("proxy-authorization:"))
                            authorization = line.mid(line.indexOf(':') + 1).trimmed();
                    authenticated |= authorization == expected;
                    const auto response = authenticated
                        ? QByteArray("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                        : QByteArray("HTTP/1.1 407 Proxy Authentication Required\r\n"
                            "Proxy-Authenticate: Basic realm=\"offline updater proxy\"\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->write(response);
                    socket->disconnectFromHost(); // No origin connection or external traffic is made.
                });
            }
        });
        QNetworkAccessManager unrelated;
        const QNetworkProxy chatProxy(QNetworkProxy::HttpProxy, "chat-proxy.invalid", 9321,
            "chat-only-user", "chat-token-never-for-updates");
        unrelated.setProxy(chatProxy);
        ReleaseUpdate updater(ReleaseUpdate::Role::Client, "0.3.1");
        updater.system = "Windows";
        updater.architecture = "x86_64";
        updater.cacheRoot = directory.filePath("updates");
        updater.checkTimeoutMs = 2500;
        QNetworkProxy selected(QNetworkProxy::HttpProxy, "127.0.0.1", proxy.serverPort(),
            credentials ? "system-proxy-user" : QString(), credentials ? "synthetic-proxy-password" : QString());
        updater.network->setProxyFactory(ReleaseUpdate::updateProxyFactory(
            [selected](const QNetworkProxyQuery &) { return QList<QNetworkProxy>{selected}; }));
        updater.check();
        QTRY_COMPARE_WITH_TIMEOUT(updater.state(), ReleaseUpdate::State::Failed, 4000);
        QCOMPARE(authenticated, credentials);
        QVERIFY(!exchanges.isEmpty());
        for (const auto &headers : exchanges)
        {
            QVERIFY(headers.startsWith("CONNECT api.github.com:443 "));
            QVERIFY(!headers.toLower().contains("\r\nauthorization:"));
            QVERIFY(!headers.toLower().contains("\r\ncookie:"));
            QVERIFY(!headers.contains("chat-token-never-for-updates"));
            QVERIFY(!headers.contains("chat-only-user"));
        }
        QCOMPARE(unrelated.proxy(), chatProxy);
        QVERIFY(!unrelated.proxyFactory());
        QVERIFY(updater.filePath().isEmpty());
        QVERIFY(partials(directory.path()).isEmpty());
    }
    void strictVersionComparison_data()
    {
        QTest::addColumn<QString>("current");
        QTest::addColumn<QString>("tag");
        QTest::addColumn<ReleaseUpdate::State>("expected");
        QTest::newRow("patch-numeric") << "0.3.9" << "v0.3.10" << ReleaseUpdate::State::Ready;
        QTest::newRow("minor-numeric") << "0.9.9" << "v0.10.0" << ReleaseUpdate::State::Ready;
        QTest::newRow("major-numeric") << "1.99.99" << "v2.0.0" << ReleaseUpdate::State::Ready;
        QTest::newRow("equal") << "0.3.1" << "v0.3.1" << ReleaseUpdate::State::UpToDate;
        QTest::newRow("older") << "2.0.0" << "v1.99.99" << ReleaseUpdate::State::UpToDate;
        for (const auto &tag : {"v1.2.3-beta", "v1.2.3+build", "v01.2.3", "v1.2", "v1.2.3junk", "v1000000000.2.3"})
            QTest::newRow(tag) << "0.3.1" << tag << ReleaseUpdate::State::Failed;
        QTest::newRow("tag-newline") << "0.3.1" << "v1.2.3\n" << ReleaseUpdate::State::Failed;
        QTest::newRow("bad-installed") << "0.3.1-local" << "v0.3.2" << ReleaseUpdate::State::Failed;
    }
    void strictVersionComparison()
    {
        QFETCH(QString, current); QFETCH(QString, tag); QFETCH(ReleaseUpdate::State, expected);
        QTemporaryDir directory;
        auto updater = make(directory.path(), ReleaseUpdate::Role::Client, current);
        auto *fake = network(updater);
        const auto value = release(tag);
        fake->plans.enqueue(metadata(value));
        fake->plans.enqueue(download(value));
        updater->check();
        QTRY_COMPARE(updater->state(), expected);
        if (expected == ReleaseUpdate::State::Ready)
        {
            QCOMPARE(fake->requests.size(), 2);
            QCOMPARE(QFileInfo(updater->filePath()).fileName(), clientName);
            QFile file(updater->filePath());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), payload);
        }
        else if (expected == ReleaseUpdate::State::UpToDate) QCOMPARE(fake->requests.size(), 1);
        QVERIFY(fake->expectedRequests);
        QVERIFY(partials(directory.path()).isEmpty());
    }
    void roleAndArchitectureSelectOnlyExactPackage_data()
    {
        QTest::addColumn<ReleaseUpdate::Role>("role");
        QTest::addColumn<QString>("system"); QTest::addColumn<QString>("architecture"); QTest::addColumn<QString>("name");
        QTest::newRow("win-client") << ReleaseUpdate::Role::Client << "Windows" << "x86_64" << clientName;
        QTest::newRow("win-server") << ReleaseUpdate::Role::Server << "Windows" << "x86_64" << "WeChatLite-Server-Windows-x64.exe";
        QTest::newRow("mac-client") << ReleaseUpdate::Role::Client << "macOS" << "arm64" << "WeChatLite-Client-macOS-arm64.dmg";
        QTest::newRow("mac-server") << ReleaseUpdate::Role::Server << "macOS" << "arm64" << "WeChatLite-Server-macOS-arm64.dmg";
        QTest::newRow("unsupported-mac-intel") << ReleaseUpdate::Role::Client << "macOS" << "x86_64" << "";
        QTest::newRow("unsupported-win-arm") << ReleaseUpdate::Role::Client << "Windows" << "arm64" << "";
    }
    void roleAndArchitectureSelectOnlyExactPackage()
    {
        QFETCH(ReleaseUpdate::Role, role); QFETCH(QString, system); QFETCH(QString, architecture); QFETCH(QString, name);
        QTemporaryDir directory;
        auto updater = make(directory.path(), role, "0.3.1", system, architecture);
        auto *fake = network(updater);
        const auto value = release("v0.3.2", name);
        fake->plans.enqueue(metadata(value)); fake->plans.enqueue(download(value));
        updater->check();
        QTRY_COMPARE(updater->state(), name.isEmpty() ? ReleaseUpdate::State::Failed : ReleaseUpdate::State::Ready);
        if (name.isEmpty()) QVERIFY(fake->requests.isEmpty());
        else QCOMPARE(QFileInfo(updater->filePath()).fileName(), name);
        QVERIFY(fake->expectedRequests);
    }
    void invalidReleaseMetadataNeverDownloads_data()
    {
        QTest::addColumn<QString>("mutation");
        for (const auto &key : {"draft", "prerelease", "wrong-repo", "wrong-html", "asset-repo", "asset-http", "asset-name",
                               "missing-digest", "bad-digest", "digest-newline", "oversize", "fractional-size", "duplicate", "not-uploaded"})
            QTest::newRow(key) << QString::fromLatin1(key);
    }
    void invalidReleaseMetadataNeverDownloads()
    {
        QFETCH(QString, mutation);
        QTemporaryDir directory;
        auto updater = make(directory.path());
        auto *fake = network(updater);
        auto value = release();
        auto asset = value["assets"].toArray().first().toObject();
        if (mutation == "draft" || mutation == "prerelease") value[mutation] = true;
        else if (mutation == "wrong-repo") value["url"] = "https://api.github.com/repos/other/WeChatLite/releases/123";
        else if (mutation == "wrong-html") value["html_url"] = "https://github.com/other/WeChatLite/releases/tag/v0.3.2";
        else if (mutation == "asset-repo") asset["url"] = "https://api.github.com/repos/other/WeChatLite/releases/assets/456";
        else if (mutation == "asset-http") asset["browser_download_url"] = "http://github.com/Difficulty-in-naming/WeChatLite/releases/download/v0.3.2/" + clientName;
        else if (mutation == "asset-name") asset["name"] = "../" + clientName;
        else if (mutation == "missing-digest") asset.remove("digest");
        else if (mutation == "bad-digest") asset["digest"] = "sha256:abcd";
        else if (mutation == "digest-newline") asset["digest"] = asset["digest"].toString() + '\n';
        else if (mutation == "oversize") asset["size"] = ReleaseUpdate::maximumAssetSize + 1;
        else if (mutation == "fractional-size") asset["size"] = 1.5;
        else if (mutation == "not-uploaded") asset["state"] = "new";
        value["assets"] = mutation == "duplicate" ? QJsonArray{asset, asset} : QJsonArray{asset};
        fake->plans.enqueue(metadata(value));
        updater->check();
        QTRY_COMPARE(updater->state(), ReleaseUpdate::State::Failed);
        QCOMPARE(fake->requests.size(), 1);
        QVERIFY(!updater->errorString().isEmpty());
        QVERIFY(updater->filePath().isEmpty());
        QVERIFY(partials(directory.path()).isEmpty());
    }
    void officialRedirectsStreamAndReuseVerifiedPackage()
    {
        QTemporaryDir directory;
        auto updater = make(directory.path());
        auto *fake = network(updater);
        const auto value = release();
        auto redirected = download(value);
        redirected.status = 302; redirected.body.clear();
        redirected.redirect = QUrl("https://release-assets.githubusercontent.com/offline-fixture?signature=synthetic");
        fake->plans.enqueue(metadata(value)); fake->plans.enqueue(redirected);
        fake->plans.enqueue(Plan{redirected.redirect, payload});
        QSignalSpy ready(updater.get(), &ReleaseUpdate::ready);
        updater->check(); updater->check(); // Busy checks do not start parallel requests.
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(fake->requests.size(), 3);
        const auto path = updater->filePath();
        for (const auto &request : fake->requests)
        {
            QVERIFY(!request.hasRawHeader("Authorization"));
            QVERIFY(!request.hasRawHeader("Proxy-Authorization"));
            QVERIFY(!request.hasRawHeader("Cookie"));
            QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
            QCOMPARE(request.attribute(QNetworkRequest::AuthenticationReuseAttribute).toInt(), int(QNetworkRequest::Manual));
            QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
            QCOMPARE(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), int(QNetworkRequest::Manual));
            QVERIFY(request.sslConfiguration().localCertificateChain().isEmpty());
            QCOMPARE(request.transferTimeout(), 30000);
        }
        QVERIFY(fake->stats.last()->maximumRead <= 64 * 1024);
        QCOMPARE(fake->stats.last()->bufferSize, 256 * 1024);
        fake->plans.enqueue(metadata(value));
        updater->check();
        QTRY_COMPARE(ready.size(), 2); // Recheck latest, asynchronously hash our existing file, no second asset GET.
        QCOMPARE(updater->filePath(), path);
        QCOMPARE(fake->requests.size(), 4);
        QFile modified(path);
        QVERIFY(modified.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(modified.write(QByteArray(payload.size(), 'x')), payload.size()); modified.close();
        fake->plans.enqueue(metadata(value)); fake->plans.enqueue(download(value));
        updater->check();
        QTRY_COMPARE(ready.size(), 3);
        QVERIFY(updater->filePath() != path); // Never overwrite an existing/running package.
        QCOMPARE(fake->requests.size(), 6);
        const auto cancelReuse = connect(updater.get(), &ReleaseUpdate::downloadProgress, this,
            [&](qint64, qint64) { updater->cancel(); });
        fake->plans.enqueue(metadata(value));
        updater->check();
        QTRY_COMPARE(updater->state(), ReleaseUpdate::State::Cancelled);
        QCOMPARE(ready.size(), 3); // Cancellation during the async reuse check cannot publish Ready later.
        disconnect(cancelReuse);
        fake->plans.enqueue(metadata(value));
        updater->check();
        QTRY_COMPARE(ready.size(), 4);
        QCOMPARE(fake->requests.size(), 8); // Both reuse checks fetched metadata only.
        QVERIFY(fake->expectedRequests);
        QVERIFY(partials(directory.path()).isEmpty());
    }
    void unsafeRedirectsAreRejected_data()
    {
        QTest::addColumn<QString>("target");
        for (const auto &url : {"http://release-assets.githubusercontent.com/file", "https://evil.example/file",
             "https://github.com/other/WeChatLite/releases/download/v0.3.2/file.exe",
             "https://user@release-assets.githubusercontent.com/file", "https://@release-assets.githubusercontent.com/file",
             "https://objects.githubusercontent.com:8443/file",
             "https://release-assets.githubusercontent.com/file#fragment"})
            QTest::newRow(url) << QString::fromLatin1(url);
    }
    void unsafeRedirectsAreRejected()
    {
        QFETCH(QString, target);
        QTemporaryDir directory;
        auto updater = make(directory.path());
        auto *fake = network(updater);
        const auto value = release();
        auto redirect = download(value); redirect.body.clear(); redirect.status = 302; redirect.redirect = QUrl(target);
        fake->plans.enqueue(metadata(value)); fake->plans.enqueue(redirect);
        updater->check();
        QTRY_COMPARE(updater->state(), ReleaseUpdate::State::Failed);
        QCOMPARE(fake->requests.size(), 2);
        QVERIFY(partials(directory.path()).isEmpty());
    }
    void boundedMetadataRedirectsAndFailedIntegrity()
    {
        for (const auto &scenario : {"metadata-limit", "metadata-redirect", "too-many-redirects", "hash", "short", "oversize-body", "tls"})
        {
            QTemporaryDir directory;
            auto updater = make(directory.path());
            auto *fake = network(updater);
            auto value = release();
            auto info = metadata(value);
            if (QByteArray(scenario) == "metadata-limit") info.body = QByteArray(1024 * 1024 + 1, 'x');
            else if (QByteArray(scenario) == "metadata-redirect") { info.status = 302; info.redirect = QUrl(api + "/releases/123"); }
            fake->plans.enqueue(info);
            auto file = download(value);
            if (QByteArray(scenario) == "too-many-redirects")
            {
                for (int i = 0; i < 6; ++i)
                {
                    file.status = 302; file.body.clear();
                    file.redirect = QUrl("https://objects.githubusercontent.com/offline-" + QString::number(i));
                    fake->plans.enqueue(file); file.url = file.redirect;
                }
            }
            else
            {
                if (QByteArray(scenario) == "hash") file.body.fill('x');
                else if (QByteArray(scenario) == "short") file.body.chop(1);
                else if (QByteArray(scenario) == "oversize-body") { file.body.append('x'); file.contentLength = payload.size(); }
                else if (QByteArray(scenario) == "tls") { file.error = QNetworkReply::SslHandshakeFailedError; file.status = 0; file.body.clear(); }
                fake->plans.enqueue(file);
            }
            updater->check();
            QTRY_COMPARE(updater->state(), ReleaseUpdate::State::Failed);
            QVERIFY2(updater->filePath().isEmpty(), scenario);
            QVERIFY2(partials(directory.path()).isEmpty(), scenario);
            QVERIFY2(fake->expectedRequests, scenario);
        }
    }
    void cancelRetryTimeoutAndDestructionCleanPartialFiles()
    {
        QTemporaryDir directory;
        auto updater = make(directory.path());
        auto *fake = network(updater);
        const auto value = release();
        fake->plans.enqueue(metadata(value)); fake->plans.enqueue(download(value));
        QSignalSpy ready(updater.get(), &ReleaseUpdate::ready);
        bool cancelled = false;
        connect(updater.get(), &ReleaseUpdate::downloadProgress, this, [&](qint64 done, qint64)
        {
            if (!cancelled && done > 0) { cancelled = true; updater->cancel(); }
        });
        updater->check();
        QTRY_COMPARE(updater->state(), ReleaseUpdate::State::Cancelled);
        QVERIFY(partials(directory.path()).isEmpty());
        QCOMPARE(ready.size(), 0);
        fake->plans.enqueue(metadata(value)); fake->plans.enqueue(download(value));
        updater->check();
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(updater->state(), ReleaseUpdate::State::Ready);
        QVERIFY(fake->expectedRequests);
        for (bool duringDownload : {false, true})
        {
            auto timed = make(directory.path());
            timed->checkTimeoutMs = timed->downloadTimeoutMs = 40;
            auto *fixture = network(timed);
            auto stalled = duringDownload ? download(value) : metadata(value);
            stalled.stall = true;
            if (duringDownload) fixture->plans.enqueue(metadata(value));
            fixture->plans.enqueue(stalled);
            timed->check();
            QTRY_COMPARE(timed->state(), ReleaseUpdate::State::Failed);
            QVERIFY(timed->errorString().contains("超时"));
            QVERIFY(partials(directory.path()).isEmpty());
        }
        auto destroyed = make(directory.path());
        auto *fixture = network(destroyed);
        auto stalled = download(value); stalled.stall = true;
        fixture->plans.enqueue(metadata(value)); fixture->plans.enqueue(stalled);
        destroyed->check();
        QTRY_COMPARE(destroyed->state(), ReleaseUpdate::State::Downloading);
        QTRY_COMPARE(partials(directory.path()).size(), 1);
        destroyed.reset();
        QVERIFY(partials(directory.path()).isEmpty());
    }
};
QTEST_GUILESS_MAIN(UpdateTests)
#include "update_tests.moc"
