#include "system_notifier.h"
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <QCryptographicHash>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
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
          state->invoke(
              [status](SystemNotifier *owner)
              {
                  if (auto *backend = static_cast<MacNotificationBackend *>(owner->native.get()))
                      backend->authorization(status);
              });
        }];
    }

  private:
    void authorization(UNAuthorizationStatus status)
    {
        if (status == UNAuthorizationStatusNotDetermined)
        {
            const auto state = dispatch;
            // This is reached only by the first actual showMessage, never by construction/tests.
            [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                                  completionHandler:^(BOOL granted, NSError *error) {
                                    Q_UNUSED(error);
                                    state->invoke(
                                        [granted](SystemNotifier *owner)
                                        {
                                            if (auto *backend = static_cast<MacNotificationBackend *>(
                                                    owner->native.get()))
                                            {
                                                backend->checking = false;
                                                if (granted)
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
            const auto identifier =
                "wechat-lite." +
                QCryptographicHash::hash(entry.group.toUtf8(), QCryptographicHash::Sha256).toHex();
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
    if (!disabled()) NSBeep();
}
