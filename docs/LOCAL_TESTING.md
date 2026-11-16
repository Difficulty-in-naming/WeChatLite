# 助手构建与本机测试交付

文档入口见 [文档目录](README.md)。用户手工操作只维护在 [本机 Windows 测试步骤](TEST-SERVER-STEPS.md)。本文给修改代码和交付测试文件的助手使用。

## 先认清环境和用语

- 用户的“本地测试”“本机测试”“我的测试”始终指本机 Windows 虚拟机里的真实微信与服务端、配合真实客户端的手工测试。
- 助手运行的单测、fixture、demo、探测脚本称为“代码验证”，不能冒充用户的测试或解释用户为何没复现。
- 正式服务端在远处，由他人在其 Windows 电脑运行，连接的是对方微信。本机通过不代表远端通过。
- 本人手机发图后的同账号同步，与其他群成员发图后的接收须分别记录。2026-11-16 用户追加确认：本机旧版接收其他账号发来的图片也正常，两边微信版本一致。账号来源或微信版本差异不能解释远端故障，远端根因仍未确认。

## 每次交付必须完成

1. 核对分支和已有改动，阅读当前故障涉及的实现文档。
2. 完成改动及直接相关的代码验证。
3. 构建 Windows 适配器和 relay EXE；缩略图先显示、完整图后更新需要两者配合。
4. 运行交付脚本，更新用户已有的共享测试目录，保留旧文件。
5. 用 `--check` 核对共享目录与当前构建完全一致。
6. 回复“共享测试目录已更新”，给用户原来的启动路径、操作和这次应观察的结果。

**用户不要 ZIP。** 不交 ZIP，不把 `target/` 中的文件链接当成交付，不让用户自己找 EXE、复制文件。共享目录超出沙箱写范围时，为文件交付申请工具权限；不能省略这一步。

## 固定目录与启动入口

macOS 实际目录：

```text
/Volumes/MacData/cal_data/Downloads/WeChatLite-构建产物/WeChatLite-Server-测试版
```

Windows 虚拟机共享目录：

```text
\\Mac\Home\Downloads\WeChatLite-构建产物\WeChatLite-Server-测试版
```

用户在上述 Windows 目录双击 **`启动测试版.bat`**。不是 `~/Downloads`，不是仓库的 `target/`，也不是另一个旧便携包。

## 构建与交付命令

工作区使用 `.tools/` 内的 Rust 1.95，不用系统 Cargo。临时文件使用实际的 `/private/tmp`，不写真实 `.state`。

```bash
cd /Volumes/MacData/cal_data/projects/WeChatLite
export RUSTUP_HOME="$PWD/.tools/rustup"
export CARGO_HOME="$PWD/.tools/cargo"
export PATH="$PWD/.tools/cargo/bin:$PATH"
export TMPDIR=/private/tmp

.tools/cargo/bin/cargo build --release --locked -p wechat-adapter -p wechat-relay \
    --target x86_64-pc-windows-gnu
python3 scripts/stage-windows-test.py
python3 scripts/stage-windows-test.py --check
```

交付脚本只更新以下五个文件：

| 目标 | 来源 |
|---|---|
| `adapter/wechat-adapter.exe` | 当前 Windows release 构建 |
| `wechat-relay.exe` | 当前 Windows release 构建，支持缩略图消息保留重试并更新原消息 |
| `启动测试版.bat` | `scripts/windows-test-launcher.bat` |
| `使用说明.md` | `docs/TEST-SERVER-STEPS.md` 的原样副本 |
| `测试构建.json` | 上述文件的 SHA-256 和功能标记 |

旧文件移动到测试目录的 `测试文件备份/<时间-编号>/`，附归档清单；重复交付相同内容不会再建备份。脚本不读取配置，不接触数据或凭据，不连接、启动、停止微信或服务端。启动器由用户执行，检查哈希和已有服务进程；发现已有进程时提示用户正常退出，不强制结束。

当前交付同时更新适配器和 relay。原目录里的 Qt 管理端、FFmpeg 和其他依赖保留。若改动涉及其他组件，应按其构建流程准备并交付对应程序。

### 核对 GitHub 包与本机交付的原生组件

GitHub 的 `build-windows.ps1` 用 MSVC 从源码重新构建 `WeChatMedia.dll`，再更新 vendor DLL 及校验文件后嵌入适配器。Mac 交叉构建默认嵌入仓库 vendor 中已有的 DLL；相同 Rust 提交或相同 `image-preview` 标记不代表两者的原生组件相同。2026-11-16 用户提供的 GitHub 包 payload `01d71bfd…` 内嵌 Media SHA-256 为 `b6d743801c72b3a24bf7efa658b23d3cf67f01bce7a574548916682a26a15523`，原共享测试构建为 `e4f29d644be8fda53758420d1f06ce83431441ef8ac582692099b32b7f36196a`。这是已核实的构建差异，不代表已经确认运行失败的根因。

比对用户指定的便携包时，只读解析并验证 payload 和目标条目的 SHA-256，不启动程序，不修改原包或真实解包缓存。需要复用其中媒体 DLL 做对照时，先校验从适配器提取的完整 DLL 哈希与内嵌哈希一致，然后同时指定 `WECHAT_MEDIA_DLL=<已验证 DLL 的绝对路径>` 与 `WECHAT_MEDIA_DLL_SHA256=<完整 SHA-256>` 构建。两者缺一或指纹不符都会拒绝构建；默认构建仍校验 vendor 的固定指纹。无需覆盖仓库中的 DLL。

启动日志新增 `native-health=split-v1 native-media-sha256=<完整 SHA-256>`，交付清单也记录 `native_media_sha256`。比较 GitHub 与共享测试版时必须核对这个值；MSVC 与 GNU 的适配器 EXE 哈希不同本身不能证明功能不同。切换不同媒体 DLL 时，按用户步骤由本人正常完全退出微信后重新登录，避免未知旧钩子链；不得关闭内存指纹检查或由助手操作真实微信。

## 怎么证明用户拿到本次构建

`--check` 必须成功。启动器按 `测试构建.json` 检查实际文件 SHA-256，随后用户核对服务端日志中 `exe=` 是当前测试目录。

当前图片兼容改动的启动标记是：

```text
wxgf=start-code-v2 image-download=full-and-mid-v1 image-preview=cdn-footer-v3 media-wait=delayed-clear-v1 inbox-poll=snapshot-v1
```

前三个标记分别表示 WXGF 解码版本、原图与标准图下载字段兼容、图片预览与恢复流程版本。`cdn-footer-v3` 补齐 CDN 图片的短尾部处理，用户已用两张带 24 字节尾部的 JPEG 确认本机前台、后台接收成功。`media-wait=delayed-clear-v1` 表示媒体等待超过 30 秒才告警一次，客户端根据同群、同来源的完整消息自动清除；缩略图不算完成。relay 也输出 `image-preview=cdn-footer-v3 media-wait=delayed-clear-v1`。功能标记不能替代文件哈希。没有现场证据时，不得因为对方失败就断言启动了旧版。

`inbox-poll=snapshot-v1` 是本轮适配器的扫描修正：读取本轮消息分片版本时，同时失效与该版本不符的旧快照。隔离的加密数据库/WAL 夹具已复现旧代码“检测到新版本却扫描旧快照”的问题，修正后无需下一条消息或重启即可读到新图片；不变分片仍复用快照。用户反馈的约 10 秒指从另一账号发图到服务端首条图片日志，且第二次可能很快；文字是否也延迟尚不确定。代码缺陷已复现，现场延迟是否由它造成仍待用户手测，不能提前归因微信后台或宣布已修好。

新增 `[timing] inbox discovered`（首次入队）、`scan`、慢 `snapshot`/`signature`/`cycle` 和原图片处理日志中的本机 `at_ms`。只记阶段、来源定位、数量与耗时，不记消息正文或下载凭据。用同一来源的日志区分扫描前与图片处理后的等待；`at_ms` 是服务端本机时间，不能直接与另一台设备的时间相减。用户步骤仍只维护在 [本机测试步骤](TEST-SERVER-STEPS.md)。本轮未改客户端，无需重新交付客户端；上轮清除警告所需的客户端继续使用。

## 媒体警告所需客户端

自动清除提示需要已交付的新版客户端配合；仅修改服务端扫描逻辑时不必重复构建客户端。涉及客户端改动时，沿用原 Mac 测试客户端和启动入口，不另建测试配置：

```text
/Volumes/MacData/cal_data/Downloads/WeChatLite-构建产物/微信轻聊-测试版.app
/Volumes/MacData/cal_data/Downloads/WeChatLite-构建产物/启动测试版客户端.command
```

构建、封装和交付命令：

```bash
cmake --build build-macos --target wechat-lite desktop-tests --parallel 3
python3 scripts/stage-macos-test-client.py --package-only
# 使用上一条输出的已封装 app 路径；封装、签名校验完成后才交付。
python3 scripts/stage-macos-test-client.py --app '<已封装 app 的绝对路径>'
python3 scripts/stage-macos-test-client.py --check
```

封装补齐 Qt 与现有辅助程序，保留测试应用的 Bundle ID，使用临时签名。程序包和 `测试客户端构建.json` 放回原构建产物目录，旧应用移入同目录的 `测试文件备份/`；原 `.command` 启动器保持不变。脚本只处理应用程序，不读取配置、媒体或消息数据，也不启动或退出客户端。交付到沙箱外目录须申请工具权限。

管理端会把适配器路径设置为自身目录的 `adapter/wechat-adapter.exe`，所以实际启动的 `wechat-server.exe` 所在目录决定加载哪份适配器。

## 助手能做什么，用户要做什么

| 动作 | 执行者 |
|---|---|
| 改代码、构建、隔离的代码验证 | 助手 |
| 更新上述共享测试目录的程序、说明与哈希清单 | 助手，保留备份 |
| 停止旧服务、双击启动器、操作真实微信、观察手测结果 | 用户亲自完成 |
| 更新远处他人的正式服务端 | 对方亲自完成 |
| 读取真实服务端或微信数据 | 先取得本次明确的只读范围授权 |

不能把“用户亲自停服和启动”误解成“助手不能把编译产物放进共享测试目录”。不改真实 `server.json`、`.state`、data、TLS、密钥、草稿、数据库或媒体。真实微信操作遵循根目录 `AGENTS.md`。

## 代码验证参考

Windows CI 的临时目录可能带 `RUNNER~1` 等短路径，而下载缓存使用规范长路径。回执路径不同文本但指向同一个私有文件时应通过；指向其他文件时仍须拒绝。原先失败的 `ordinary_reencoded_pictures_require_a_complete_resource_bound_receipt_and_decodable_bytes`、`record_picture_footer_is_normalized_after_verifying_the_original_download` 已在 Windows 短临时路径复现；验证这类修正时保留群下载授权检查，不能给夹具授权真实群来绕过错误。新增 `completed_picture_receipt_accepts_canonical_alias_only_for_the_expected_private_file` 覆盖同文件别名与相同字节的其他文件，跨平台运行。

只运行与改动直接相关的验证；这些结果单独标为代码验证。本次图片改动可用：

```bash
.tools/cargo/bin/cargo test --locked -p wechat-adapter --bin wechat-adapter media::tests::
.tools/cargo/bin/cargo test --locked -p wechat-adapter -p wechat-relay --bins thumbnail
.tools/cargo/bin/cargo test --locked -p wechat-relay --bin wechat-relay picture_upgrade_preserves
git diff --check
```

媒体和解析测试使用临时夹具，不连接真实微信；不要运行 live probe 或依赖真实微信的测试。需要验证真实 Windows 行为时，用用户手工流程，不能用 demo 的假数据替代。

其他组件按改动范围选用现有套件：Rust 服务端核心使用 `cargo test -p wechat-relay --bin wechat-relay`；Qt UI 使用 `build-macos/client/desktop-tests -platform offscreen` 等对应套件。Qt 在 macOS 的标准路径需要单独的 `CFFIXED_USER_HOME` 和测试 profile；测试目录必须在工作区外的临时目录，避免写入真实配置。不要直接照搬可能创建客户端、发送消息或触发真实适配器的集成命令，先按 `AGENTS.md` 检查隔离条件。

协议 CI 的启动验证可单独运行 `cargo test --locked -p wechat-relay --test protocol startup_seeds_history_without_live_replay`。它只用临时目录、随机回环端口、空 `bridge_program` 的 `--demo` 实例，检查首次启动和重启后仍有 804 条消息、其中 800 条历史不进入实时回放；不发送消息、不连接微信。存储批量导入的持久化、去重与失败回滚用 `cargo test --locked -p wechat-relay --bin wechat-relay store::tests::` 验证。

2026-11-16 Windows CI 的 5 项协议失败包括 3 项演示实例启动超时和 2 项整份 stderr 比较失败。演示历史现改为单事务持久化，保持 `synchronous=FULL`；适配器退出诊断单独按内容、次数和顺序校验，允许 relay 自身输出启动标记。启动超时会附模式、耗时、最后状态及 stdout/stderr，不能只凭超时认定现场微信有故障，也不能把本次演示代码验证当作用户本机测试。

保留的环境注意事项：macOS `/var` 的符号链接会触发媒体目录的链接检查，使用实际的 `/private/tmp`；系统代理能影响 curl 而 Qt WebSocket 行为不同，网络诊断先确认直连范围；生成 DMG 的 `hdiutil` 可能需要沙箱外权限。中文产物标记按 UTF-8 字节核对，不能用 UTF-16 搜不到来判定版本。

真实只读网络和微信库诊断仍需用户明确授权本次范围。`ready: true` 只能说明已报告的状态，不能证明新消息正在推进；无新日志也不足以单独判定失联。不要把早期提交的“尚未验证”列表或历史 handoff 结论照抄为当前事实。

## 正式便携 EXE

正式便携 EXE 对内部文件有整体校验，不能直接改其内部适配器。正式打包见 [发布](RELEASE.md) 和 [便携包格式](../packaging/windows-portable/FORMAT.md)。用户本机测试使用既有的展开目录，助手直接更新其中的 EXE，不能把“正式包需要重打包”作为交 ZIP 的理由。
