# macOS Dock 未读角标排查

日期：2026-10-17。状态：已定位并修复。用户真机确认新消息显示 Dock 红色数字，阅读消息后角标消失。

根因：旧安装仅登记过横幅和声音授权，Badge 仍为 `NotSupported`；代码只在通知授权 `NotDetermined` 时申请选项，导致已授权的旧安装一直跳过 Badge 补申请。

## 操作边界

- 不读取或操作微信轻聊界面，不输入或发送任何消息。
- 应用的打开、退出、会话选择和测试消息发送均由用户操作。
- 诊断只读取相关代码、Dock/通知授权元数据，不记录聊天内容。

## 已确认的证据

用户提供的前序记录：`/tmp/notdiag/badge-problem.md`。

2026-10-17 12:42，用户用手机在测试群发送消息。临时观测库记录到：

```text
bundle=local.wechatlite.client
path=/Users/cal/Applications/微信轻聊.app
class=QNSApplication policy=0
tile.size={128, 128} tile.contentView=nil
authorization=2 badge=0 alert=2 sound=2 center=2
WRITE badge old=nil new=1 main=1
STATE 1s after badge write label=1
STATE 4s after badge write label=1
NOTIFICATION content.badge=nil
NOTIFICATION delivered error=nil
```

以上枚举按 SDK 定义解释为：通知 `Authorized`，Badge `NotSupported`，横幅、声音和通知中心均 `Enabled`。用户也确认通知和声音正常。

这次角标写入在主线程执行，之后 4 秒内没有再次写入、清空或替换 Dock tile 的记录。源码中只有一处应用角标写入，窗口图标只在初始化时设置。当前证据不支持“业务代码紧接着覆盖角标”的解释。

`contentView=nil` 是这次已安装主应用的观测值，不应沿用前序其他构建的非空结果。它本身不能证明异常。

原始观测文件：`/tmp/notdiag/badge-session/trace.log`。观测库仅临时加载，未写入应用包或仓库。

## 根因与修复后对照

此前“零授权或拒绝授权的探针仍能画角标”，并没有覆盖主应用的具体状态：**通知已授权，但从未登记 Badge 选项**。

源码中的 `authorization()` 只在 `NotDetermined` 时申请 `Alert | Sound | Badge`。已获横幅和声音授权的旧安装会跳过该分支，即使 `badgeSetting` 仍为 `NotSupported`。

2026-10-17 12:55，用户自行通过诊断脚本启动原安装路径下的修复版，并用手机发送测试消息。日志记录：

```text
PERMISSIONS authorization=2 badge=0 alert=2 sound=2 center=2
WRITE badge old=nil new=1
AUTH request options=7
AUTH completion granted=1 error=nil
WRITE badge old=1 new=1
AUTH settings authorization=2 badge=2 alert=2 sound=2
NOTIFICATION delivered error=nil
```

`options=7` 为 `Badge | Sound | Alert`。补申请使 Badge 从 `NotSupported` 变为 `Enabled`，用户确认 Dock 出现红色数字；随后用户自行阅读测试群消息，确认角标消失。

前后保持相同 bundle ID、签名身份和安装路径。该对照确认了上述授权升级缺口导致的显示问题。`lsappinfo` 和 `badgeLabel` 读回值均不能替代 Dock 的实际显示结果。

## 修复内容

`client/system_notifier.mm`：

- 同时检查通知授权状态和 `badgeSetting`。
- 对已授权且 Badge 为 `NotSupported` 的旧安装，每次运行最多补申请一次选项。
- 不对用户明确关闭的 `Disabled` 状态补申请。
- 授权成功后重新应用最新未读数，包括等待期间变为零的情况。
- 补申请失败时，已有通知授权仍可继续投递横幅和声音。
- 移除 `NSApp.dockTile` 上无效的 `showsApplicationBadge` 赋值和错误注释。

验证情况：`wechat-lite` 目标编译成功；修复应用沿用原 bundle ID 和 Apple Development 签名，`codesign --verify --deep --strict` 成功。用户确认自行退出后，修复版已替换到原安装路径；由用户自行启动，并完成新消息显示角标及已读清零的真机验证。

原安装包归档：`/tmp/notdiag/badge-session/archive/微信轻聊.app`，归档条目 `ASD-20261017-045335-e9eb7abe`。本轮使用的用户手动启动脚本为 `/tmp/notdiag/badge-session/手动启动诊断.command`；脚本只加载临时观测库，不读取界面、不输入或发送消息。

## 后续使用与边界

- 日常使用直接打开 `~/Applications/微信轻聊.app` 即可，修复不依赖临时观测库。
- 诊断脚本仅用于本次观测；未将观测代码加入正式应用包。
- 本轮确认了旧授权升级、角标出现和已读消失；全新安装、用户手动关闭 Badge、申请失败等其他系统状态未另行做真机测试。
