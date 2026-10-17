#include "system_notifier.h"
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <QCryptographicHash>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#import <UserNotifications/UserNotifications.h>
#include <algorithm>
#include <functional>
#include <utility>

namespace
{
NSString *text(const QString &value)
{
    return [NSString stringWithCharacters:reinterpret_cast<const unichar *>(value.utf16())
                                   length:NSUInteger(value.size())];
}

struct Dispatch
{
    QMutex mutex;
    SystemNotifier *owner = nullptr;
    void invoke(std::function<void(SystemNotifier *)> action)
    {
        QMutexLocker lock(&mutex);
        if (owner)
            QMetaObject::invokeMethod(
                owner, [target = owner, action = std::move(action)] { action(target); },
                Qt::QueuedConnection);
    }
};
} // namespace

@interface WeChatLiteNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
{
  @public
    std::weak_ptr<Dispatch> dispatch;
}
@end

@implementation WeChatLiteNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    Q_UNUSED(center);
    if ([notification.request.identifier hasPrefix:@"wechat-lite."])
        completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList |
                          (notification.request.content.sound ? UNNotificationPresentationOptionSound : 0));
    else
        completionHandler(UNNotificationPresentationOptionNone);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    Q_UNUSED(center);
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier] &&
        [response.notification.request.identifier hasPrefix:@"wechat-lite."])
    {
        id value = response.notification.request.content.userInfo[@"group"];
        if ([value isKindOfClass:[NSString class]] && [value length] > 0 && [value length] <= 1024)
        {
            const auto group = QString::fromUtf8([(NSString *)value UTF8String]);
            if (auto target = dispatch.lock())
                target->invoke([group](SystemNotifier *owner) { emit owner->activated(group); });
        }
    }
    completionHandler();
}
@end

class MacNotificationBackend final : public SystemNotifier::Native
{
  public:
    explicit MacNotificationBackend(SystemNotifier *owner) : dispatch(std::make_shared<Dispatch>())
    {
        dispatch->owner = owner;
        center = UNUserNotificationCenter.currentNotificationCenter;
        previous = center.delegate;
        delegate = [[WeChatLiteNotificationDelegate alloc] init];
        delegate->dispatch = dispatch;
        center.delegate = delegate;
    }

    ~MacNotificationBackend() override
    {
        {
            QMutexLocker lock(&dispatch->mutex);
            dispatch->owner = nullptr;
        }
        if (center.delegate == delegate)
            center.delegate = previous;
    }

    void show(const QString &group, const QString &title, const QString &body, bool sound) override
    {
        // Permission UI may stay open while messages arrive. Keep only the latest per group,
        // with a fixed cap, rather than retaining an unbounded backlog of private content.
        auto existing = std::find_if(pending.begin(), pending.end(),
                                     [&group](const auto &entry) { return entry.group == group; });
        if (existing != pending.end())
            *existing = {group, title, body, sound};
        else
        {
            if (pending.size() == 32)
                pending.removeFirst();
            pending.append({group, title, body, sound});
        }
        if (checking)
            return;
        checking = true;
        const auto state = dispatch;
        [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
          const auto status = settings.authorizationStatus;
          const auto badge = settings.badgeSetting;
          state->invoke(
              [status, badge](SystemNotifier *owner)
              {
                  if (auto *backend = static_cast<MacNotificationBackend *>(owner->native.get()))
                      backend->authorization(status, badge);
              });
        }];
    }

    void setBadge(int count) override
    {
        // Keep the unread label independent of banner delivery. The Dock also consults the
        // registered application's notification settings when deciding whether to render it.
        NSDockTile *tile = NSApp.dockTile;
        if (!tile)
            return;
        tile.badgeLabel = count > 0 ? text(QString::number(count)) : nil;
        [tile display];
    }

  private:
    void authorization(UNAuthorizationStatus status, UNNotificationSetting badge)
    {
        const bool authorized = status == UNAuthorizationStatusAuthorized ||
                                status == UNAuthorizationStatusProvisional;
        // Older versions requested only alert/sound. An authorized application can therefore
        // still have no Badge option in its notification settings. Request the missing
        // option once per run, without overriding a badge the user explicitly disabled.
        const bool missingBadge = authorized && badge == UNNotificationSettingNotSupported &&
                                  !requestedBadgeAuthorization;
        if (status == UNAuthorizationStatusNotDetermined || missingBadge)
        {
            requestedBadgeAuthorization = true;
            const auto state = dispatch;
            // This is reached only by the first actual showMessage, never by construction/tests.
            [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound |
                                                    UNAuthorizationOptionBadge)
                                  completionHandler:^(BOOL granted, NSError *error) {
                                    Q_UNUSED(error);
                                    state->invoke(
                                        [granted, authorized](SystemNotifier *owner)
                                        {
                                            if (auto *backend = static_cast<MacNotificationBackend *>(
                                                    owner->native.get()))
                                            {
                                                backend->checking = false;
                                                // The unread count may have changed while the
                                                // permission request was pending.
                                                if (granted)
                                                    backend->setBadge(owner->unreadTotal);
                                                if (granted || authorized)
                                                    backend->deliverPending();
                                                else
                                                    backend->pending.clear();
                                            }
                                        });
                                  }];
            return;
        }
        checking = false;
        if (status == UNAuthorizationStatusAuthorized || status == UNAuthorizationStatusProvisional)
            deliverPending();
        else
            pending.clear();
    }

    void deliverPending()
    {
        const auto ready = std::exchange(pending, {});
        for (const auto &entry : ready)
        {
            UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
            content.title = text(entry.title);
            content.body = text(entry.body);
            content.sound = entry.sound ? UNNotificationSound.defaultSound : nil;
            content.threadIdentifier = text(entry.group);
            content.userInfo = @{@"group" : text(entry.group)};
            // The identifier must be unique per message. Reusing one identifier per group makes
            // macOS treat each new message as an update to an already-delivered notification, so it
            // reorders the existing entry instead of alerting: no banner, no sound, one visible row.
            // threadIdentifier keeps the per-group stack that identifier uniqueness would otherwise
            // cost us, because macOS groups by thread rather than by identifier.
            const auto identifier =
                "wechat-lite." +
                QCryptographicHash::hash(entry.group.toUtf8(), QCryptographicHash::Sha256).toHex() +
                "." + QByteArray::number(sequence++);
            UNNotificationRequest *request =
                [UNNotificationRequest requestWithIdentifier:text(QString::fromLatin1(identifier))
                                                     content:content
                                                     trigger:nil];
            [center addNotificationRequest:request withCompletionHandler:nil];
        }
    }

    struct Pending
    {
        QString group, title, body;
        bool sound = true;
    };
    std::shared_ptr<Dispatch> dispatch;
    UNUserNotificationCenter *center = nil;
    WeChatLiteNotificationDelegate *delegate = nil;
    __weak id<UNUserNotificationCenterDelegate> previous = nil;
    QList<Pending> pending;
    bool checking = false;
    bool requestedBadgeAuthorization = false;
    // Makes every delivered request identifier unique. Uniqueness is what forces a fresh alert,
    // because macOS treats a repeat identifier as an update to an already-delivered notification
    // and updates do not re-alert. Seeded randomly so a relaunch cannot collide with entries that
    // an earlier run left visible in Notification Center.
    quint64 sequence = QRandomGenerator::global()->generate64();
};

std::unique_ptr<SystemNotifier::Native> SystemNotifier::createNative(SystemNotifier *owner)
{
    // UserNotifications requires an application bundle identity. CLI fixtures must remain inert.
    if (NSBundle.mainBundle.bundleIdentifier.length == 0)
        return {};
    @try
    {
        return std::make_unique<MacNotificationBackend>(owner);
    }
    @catch (NSException *exception)
    {
        Q_UNUSED(exception);
        return {};
    }
}

void SystemNotifier::previewSound()
{
    // 试听 should sound like a real message. A real message uses UNNotificationSound.defaultSound,
    // which resolves to the ToneLibrary "Note" tone rather than to anything under /System/Library/
    // Sounds, so resolve that tone first and keep the generic alert and NSBeep only as fallbacks.
    if (disabled())
        return;
    static NSSound *alert = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSBundle *tones = [NSBundle bundleWithIdentifier:@"com.apple.ToneLibrary"];
        NSString *path = [tones pathForResource:@"Note" ofType:@"m4r"
                                   inDirectory:@"AlertTones/Modern"];
        if (!path)
            path = @"/System/Library/PrivateFrameworks/ToneLibrary.framework/Versions/A/"
                   @"Resources/AlertTones/Modern/Note.m4r";
        alert = [[NSSound alloc] initWithContentsOfFile:path byReference:YES];
        if (!alert)
            alert = [NSSound soundNamed:@"Ping"];
    });
    if (alert)
    {
        [alert stop];
        [alert play];
    }
    else
        NSBeep();
}
