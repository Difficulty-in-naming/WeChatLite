#include "system_notifier.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <notificationactivationcallback.h>
#include <propkey.h>
#include <propvarutil.h>
#include <roapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>
#include <wrl.h>

namespace
{
// Public, stable application identity; it is neither an account identifier nor a credential.
constexpr wchar_t appId[] = L"WeChatLite.Client";
constexpr wchar_t activatorText[] = L"{5EC8E92B-BA31-4CC9-9A44-9370811FF548}";
constexpr CLSID activatorId{0x5ec8e92b, 0xba31, 0x4cc9, {0x9a, 0x44, 0x93, 0x70, 0x81, 0x1f, 0xf5, 0x48}};
using Microsoft::WRL::ComPtr;

struct Apartment
{
    bool initialized = false;
    Apartment()
    {
        const auto result = RoInitialize(RO_INIT_SINGLETHREADED);
        initialized = SUCCEEDED(result);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            winrt::throw_hresult(result);
    }
    ~Apartment()
    {
        if (initialized)
            RoUninitialize();
    }
};

QString encodeGroup(const QString &group)
{
    return "group:" + QString::fromLatin1(group.toUtf8().toBase64(QByteArray::Base64UrlEncoding |
                                                                  QByteArray::OmitTrailingEquals));
}

QString decodeGroup(const QString &argument)
{
    if (!argument.startsWith("group:") || argument.size() > 8192)
        return {};
    const auto decoded = QByteArray::fromBase64Encoding(
        argument.mid(6).toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return {};
    const auto group = QString::fromUtf8(decoded.decoded);
    if (group.isEmpty() || group.size() > 1024 || group.toUtf8() != decoded.decoded)
        return {};
    for (auto character : group)
        if (character.isNull() || character.category() == QChar::Other_Control)
            return {};
    return group;
}

struct Dispatch
{
    QMutex mutex;
    SystemNotifier *owner = nullptr;
    void activate(const QString &group)
    {
        if (group.isEmpty())
            return;
        QMutexLocker lock(&mutex);
        if (owner)
            QMetaObject::invokeMethod(
                owner, [target = owner, group] { emit target->activated(group); }, Qt::QueuedConnection);
    }
};

class Activator final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          INotificationActivationCallback, Microsoft::WRL::FtmBase>
{
  public:
    explicit Activator(std::shared_ptr<Dispatch> dispatcher) : dispatch(std::move(dispatcher)) {}
    HRESULT STDMETHODCALLTYPE Activate(LPCWSTR identity, LPCWSTR arguments,
                                       const NOTIFICATION_USER_INPUT_DATA *, ULONG) override
    {
        try
        {
            if (!identity || wcscmp(identity, appId) != 0 || !arguments)
                return E_INVALIDARG;
            const auto length = wcsnlen_s(arguments, 8193);
            if (length > 8192)
                return E_INVALIDARG;
            dispatch->activate(decodeGroup(QString::fromWCharArray(arguments, int(length))));
            return S_OK;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

  private:
    std::shared_ptr<Dispatch> dispatch;
};

class ActivatorFactory final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IClassFactory, Microsoft::WRL::FtmBase>
{
  public:
    explicit ActivatorFactory(std::shared_ptr<Dispatch> dispatcher) : dispatch(std::move(dispatcher)) {}
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid, void **result) override
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        auto callback = Microsoft::WRL::Make<Activator>(dispatch);
        return callback ? callback->QueryInterface(iid, result) : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }

  private:
    std::shared_ptr<Dispatch> dispatch;
};

void registryString(const std::wstring &path, const wchar_t *name, const std::wstring &value)
{
    HKEY key = nullptr;
    winrt::check_win32(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                       &key, nullptr));
    const auto status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                                       DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    winrt::check_win32(status);
}

void installIdentity()
{
    auto executable = QCoreApplication::instance()->property("notificationLaunchExecutable").toString();
    if (!QFileInfo(executable).isAbsolute() || !QFileInfo(executable).isFile())
        executable = QCoreApplication::applicationFilePath();
    const auto target = QDir::toNativeSeparators(executable).toStdWString();
    // Quote the complete executable path. No arguments, chat content or credentials enter the registry.
    registryString(std::wstring(L"Software\\Classes\\CLSID\\") + activatorText + L"\\LocalServer32", nullptr,
                   L"\"" + target + L"\" --notification-activation");
    const auto identity = std::wstring(L"Software\\Classes\\AppUserModelId\\") + appId;
    registryString(identity, L"DisplayName", L"微信轻聊");
    registryString(identity, L"CustomActivator", activatorText);

    PWSTR programs = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programs));
    const auto directory = QString::fromWCharArray(programs) + QStringLiteral("/微信轻聊");
    CoTaskMemFree(programs);
    if (!QDir().mkpath(directory))
        winrt::throw_hresult(E_ACCESSDENIED);
    const auto shortcut =
        QDir::toNativeSeparators(directory + QStringLiteral("/微信轻聊.lnk")).toStdWString();
    ComPtr<IShellLinkW> link;
    winrt::check_hresult(
        CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)));
    winrt::check_hresult(link->SetPath(target.c_str()));
    const auto working = QDir::toNativeSeparators(QFileInfo(executable).absolutePath()).toStdWString();
    winrt::check_hresult(link->SetWorkingDirectory(working.c_str()));
    winrt::check_hresult(link->SetDescription(L"微信轻聊客户端"));
    ComPtr<IPropertyStore> properties;
    winrt::check_hresult(link.As(&properties));
    PROPVARIANT value;
    PropVariantInit(&value);
    winrt::check_hresult(InitPropVariantFromString(appId, &value));
    auto result = properties->SetValue(PKEY_AppUserModel_ID, value);
    PropVariantClear(&value);
    winrt::check_hresult(result);
    winrt::check_hresult(InitPropVariantFromCLSID(activatorId, &value));
    result = properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, value);
    PropVariantClear(&value);
    winrt::check_hresult(result);
    winrt::check_hresult(properties->Commit());
    ComPtr<IPersistFile> persist;
    winrt::check_hresult(link.As(&persist));
    winrt::check_hresult(persist->Save(shortcut.c_str(), TRUE));
    winrt::check_hresult(SetCurrentProcessExplicitAppUserModelID(appId));
}
} // namespace

class WindowsNotificationBackend final : public QObject, public SystemNotifier::Native
{
  public:
    explicit WindowsNotificationBackend(SystemNotifier *owner)
        : QObject(owner), dispatch(std::make_shared<Dispatch>())
    {
        dispatch->owner = owner;
        const auto name =
            "WeChatLite.Notifications." +
            QString::fromLatin1(
                QCryptographicHash::hash(
                    QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toUtf8(),
                    QCryptographicHash::Sha256)
                    .toHex()
                    .left(20));
        ipcName = name;
        QLocalSocket probe;
        probe.connectToServer(name, QIODevice::WriteOnly);
        const bool existing = probe.waitForConnected(120);
        probe.abort();
        relay = existing && SystemNotifier::activationLaunch();
        if (!existing)
        {
            server.setSocketOptions(QLocalServer::UserAccessOption);
            server.setMaxPendingConnections(4);
            connect(&server, &QLocalServer::newConnection, this, [this] { acceptActivations(); });
            server.listen(name);
        }
        if (!existing || relay)
        {
            auto factory = Microsoft::WRL::Make<ActivatorFactory>(dispatch);
            if (!factory)
                winrt::throw_hresult(E_OUTOFMEMORY);
            winrt::check_hresult(CoRegisterClassObject(activatorId, static_cast<IClassFactory *>(factory.Get()),
                                                       CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &registration));
        }
        if (relay)
        {
            connect(owner, &SystemNotifier::activated, this,
                    [this](const QString &group)
                    {
                        QLocalSocket socket;
                        socket.connectToServer(ipcName);
                        if (socket.waitForConnected(500))
                        {
                            socket.write(encodeGroup(group).toUtf8() + '\n');
                            socket.waitForBytesWritten(500);
                            socket.waitForReadyRead(500);
                        }
                        QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
                    });
            QTimer::singleShot(15000, this, [] { QCoreApplication::quit(); });
        }
    }

    ~WindowsNotificationBackend() override
    {
        {
            QMutexLocker lock(&dispatch->mutex);
            dispatch->owner = nullptr;
        }
        if (registration)
            CoRevokeClassObject(registration);
        server.close();
        notifier = nullptr;
    }

    bool isActivationRelay() const override { return relay; }

    void show(const QString &group, const QString &title, const QString &body, bool sound) override
    {
        if (relay)
            return;
        try
        {
            if (!notifier)
            {
                installIdentity();
                notifier =
                    winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(appId);
            }
            winrt::Windows::Data::Xml::Dom::XmlDocument document;
            const auto xml = QStringLiteral("<toast launch=\"%1\"><visual><binding template=\"ToastGeneric\">"
                                            "<text/><text/></binding></visual>%2</toast>")
                                 .arg(encodeGroup(group), sound ? QString() : QStringLiteral("<audio silent=\"true\"/>"));
            document.LoadXml(winrt::hstring(xml.toStdWString()));
            const auto text = document.GetElementsByTagName(L"text");
            text.Item(0).AppendChild(document.CreateTextNode(winrt::hstring(title.toStdWString())));
            text.Item(1).AppendChild(document.CreateTextNode(winrt::hstring(body.toStdWString())));
            winrt::Windows::UI::Notifications::ToastNotification toast(document);
            toast.Tag(winrt::hstring(
                QString::fromLatin1(
                    QCryptographicHash::hash(group.toUtf8(), QCryptographicHash::Sha256).toHex().left(16))
                    .toStdWString()));
            toast.Group(L"wechat-lite");
            toast.ExpirationTime(winrt::clock::now() + std::chrono::hours(24));
            notifier.Show(toast);
        }
        catch (...)
        { /* Notification policy must never interrupt chat or expose message text in logs. */
        }
    }

  private:
    void acceptActivations()
    {
        while (auto *socket = server.nextPendingConnection())
        {
            if (server.findChildren<QLocalSocket *>(QString(), Qt::FindDirectChildrenOnly).size() > 4)
            {
                delete socket;
                continue;
            }
            socket->setReadBufferSize(8193);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            const auto consume = [this, socket]
            {
                if (socket->bytesAvailable() > 8192)
                {
                    socket->abort();
                    return;
                }
                if (!socket->canReadLine())
                    return;
                const auto group = decodeGroup(QString::fromUtf8(socket->readLine(8193)).trimmed());
                if (!group.isEmpty())
                    dispatch->activate(group);
                socket->write("ok\n");
                socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, this, consume);
            if (socket->bytesAvailable())
                consume();
            QTimer::singleShot(3000, socket, [socket] { socket->abort(); });
        }
    }

    Apartment apartment;
    std::shared_ptr<Dispatch> dispatch;
    QLocalServer server;
    QString ipcName;
    bool relay = false;
    DWORD registration = 0;
    winrt::Windows::UI::Notifications::ToastNotifier notifier{nullptr};
};

std::unique_ptr<SystemNotifier::Native> SystemNotifier::createNative(SystemNotifier *owner)
{
    try
    {
        return std::make_unique<WindowsNotificationBackend>(owner);
    }
    catch (...)
    {
        return {};
    }
}

void SystemNotifier::previewSound()
{
    if (!disabled()) MessageBeep(MB_OK);
}
