# 本地测试与验证

这份文档写给「改了代码要验证」的人和 AI 助手。它记录的是**踩过的坑**，不是理想流程。

核心原则：**真实微信群不可操作**。所有自动化验证必须在隔离环境完成；凡是必须碰真实数据的验证，一律由用户本人执行，助手只提供可复现的步骤和判断依据。

---

## 一、先分清「能自动验证」和「必须用户验证」

| 改动位置 | 自动化验证 | 需要真实环境 |
|---|---|---|
| 纯函数、序列化、解析 | 单元测试 | 否 |
| 客户端 UI 逻辑 | `desktop-tests`（mock HTTP） | 否 |
| 适配器数据库查询 | 内存 SQLite fixture | 否 |
| 服务端 HTTP 行为 | `wechat-relay` 测试 | 否 |
| 真实微信库的字段含义 | — | **是** |
| 真实群的收发 | — | **是** |
| TLS/证书/局域网连接 | — | **是** |
| WXGF/SILK 媒体解码 | 需要 FFmpeg 的 fixture | 部分 |

**判断标准**：如果验证过程会读取真实微信数据库、发消息、或连接用户正在用的服务端，就不能自动化。

---

## 二、各套件的跑法

项目自带的 Rust 工具链在 `.tools/`，不要用系统 cargo（Homebrew 的 1.88 编不了 edition 2024）。

```bash
cd /Volumes/MacData/cal_data/projects/WeChatLite
export RUSTUP_HOME="$PWD/.tools/rustup"
export CARGO_HOME="$PWD/.tools/cargo"
export TMPDIR="$PWD/.state/tmp-rs"   # 见下方「TMPDIR 陷阱」
```

### 适配器（Rust）

```bash
.tools/cargo/bin/cargo test -p wechat-adapter --bin wechat-adapter -- \
    --skip real_static_hevc_color --skip text_send_builds_stable_id
```

**两个必须跳过的测试**（环境原因，不是代码问题）：

- `real_static_hevc_color_*` — 需要真实 FFmpeg 二进制
- `text_send_builds_stable_id_*` — 需要真实微信进程

### 服务端（Rust）

```bash
.tools/cargo/bin/cargo test -p wechat-relay --bin wechat-relay
.tools/cargo/bin/cargo test -p wechat-relay --test protocol
```

### 客户端（Qt/C++）

构建：

```bash
cmake --build build-macos --target wechat-lite desktop-tests client-tests -j 8
```

运行（**必须设置隔离的 HOME**，否则测试会写真实配置目录）：

```bash
export CFFIXED_USER_HOME="$PWD/.state/test-home"
export HOME="$PWD/.state/test-home"
export QT_SSL_USE_TEMPORARY_KEYCHAIN=1
mkdir -p .state/test-home

./build-macos/client/desktop-tests -platform offscreen
./build-macos/client/client-tests -platform offscreen
./build-macos/client/transport-tests -platform offscreen
./build-macos/client/preferences-tests -platform offscreen
./build-macos/client/notifier-tests -platform offscreen
```

用 `-platform offscreen` 可以无窗口运行。系统通知在 offscreen 下由 `SystemNotifier` 自动抑制。

---

## 三、踩过的坑（照着做能省半小时）

### 1. `TMPDIR` 是符号链接

macOS 的 `$TMPDIR` 指向 `/var/folders/...`，是个符号链接。适配器的媒体测试里有「媒体目录不能是链接」的检查，会因此失败：

```
媒体目录不能是链接
```

**解法**：`export TMPDIR="$PWD/.state/tmp-rs"`，用仓库内的真实目录。

### 2. Qt 只认 `CFFIXED_USER_HOME`

macOS 上 `QStandardPaths` **不看 `HOME`**，必须设 `CFFIXED_USER_HOME`。只设 `HOME` 时测试仍然会写 `~/Library/Application Support`，在不允许写入的环境里表现为：

```
attachments.addImage 返回 false
```

**解法**：两个都设，并且 `mkdir -p` 目标目录。

### 3. 中文在二进制里是 UTF-8，不是 UTF-16

用 Python 在编译产物里搜字符串时，`QStringLiteral` 的中文是 UTF-8 编码存储的，不是 UTF-16LE。搜不到不代表没有：

```python
open(binary,'rb').read().find('已定位到原文'.encode())        # 对
open(binary,'rb').read().find('已定位到原文'.encode('utf-16-le'))  # 错
```

### 4. `hdiutil` 在沙箱里会被拒

创建和校验 DMG 需要在 workspace-write 之外的权限，否则报：

```
hdiutil: create failed - 操作不被允许
```

### 5. `cargo` 的 registry 缓存不能在 `/tmp`

`/tmp` 下的 cargo registry 会被沙箱拦住。用 `.state/cargo-home` 或 `.tools/cargo`。

### 6. 测试 fixture 必须写成真实格式

写引用卡片的 fixture 时**必须带 `<type>57</type>`**：

```xml
<msg><appmsg><type>57</type><title>…</title><refermsg>…</refermsg></appmsg></msg>
```

漏了它，`reference()` 直接返回 `None`，测试会表现为「convert 没有认出这是引用」，很容易误判成代码 bug。

### 7. 系统有 `HTTPS_PROXY` 时 curl 会走代理

`curl` 读环境变量里的 `HTTPS_PROXY`，Qt 的 `QWebSocket` **不读**。所以 curl 成功不代表客户端能连上：

```
* Connected to 127.0.0.1 (127.0.0.1) port 7897   ← 走了代理
```

验证真实连通性时加 `--noproxy '*'` 强制直连。

---

## 四、验证网络连接（只读）

确认服务端可达、TLS 正常、token 有效。**全部是 GET，不写入任何数据**。

```bash
# 1. 端口是否开放
nc -zv -w 3 <host> <port>

# 2. 直连（绕过代理）是否返回 200
curl -sk --noproxy '*' --max-time 8 -o /dev/null -w "%{http_code}\n" https://<host>:<port>/health

# 3. 证书指纹是否和客户端配置一致
openssl s_client -connect <host>:<port> -servername <host> </dev/null 2>/dev/null \
  | openssl x509 -fingerprint -sha256 -noout

# 4. 用客户端自己的 token 请求状态
curl -sk --noproxy '*' -H "Authorization: Bearer <token>" https://<host>:<port>/api/status
```

第 4 步返回的关键字段：

| 字段 | 含义 | 注意 |
|---|---|---|
| `ready` | 服务端认为适配器可用 | **只在启动时设置，之后不更新**，不可作为健康指标 |
| `self_id` | 适配器读到的微信账号 | 空说明适配器没读到微信 |
| `text` | 人类可读的状态 | 同样是启动时写的 |
| `permissions.groups` | 允许读取的群 | 空说明没选群 |

**重要**：`ready: true` 不代表现在能收到新消息。适配器卡死时它仍然是 `true`。判断"能不能收到"要看 `/api/messages` 里最新一条的时间戳是不是在推进。

### WebSocket 握手测试

`curl` 不能测 WebSocket。用 Python 发原始握手：

```python
import base64, os, socket, ssl
ctx = ssl.create_default_context(); ctx.check_hostname=False; ctx.verify_mode=ssl.CERT_NONE
key = base64.b64encode(os.urandom(16)).decode()
req = (f"GET /api/events?after=0 HTTP/1.1\r\nHost: {host}:{port}\r\n"
       f"Authorization: Bearer {token}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
with socket.create_connection((host, port), timeout=8) as s:
    with ctx.wrap_socket(s) as t:
        t.sendall(req.encode())
        print(t.recv(4096).decode('utf-8','replace').split('\r\n\r\n')[0])
```

期望 `HTTP/1.1 101 Switching Protocols`。

注意：`urllib` 会把 header 名规范成 `Upgrade` 之外的写法，服务端返回 `400 Connection header did not include 'upgrade'`，这是**测试方法的问题**，不是服务端的问题。

---

## 五、隔离测试客户端

不要改动 `~/Applications/微信轻聊.app`。用独立副本：

```bash
bash scripts/make-test-client.sh          # 复制当前构建
build-macos/test-client/run.sh            # 隔离启动（默认 --demo）
```

隔离的三样东西：

1. **Bundle ID** `local.wechatlite.client.test` —— 通知授权、TCC 独立
2. **`CFFIXED_USER_HOME` / `WECHAT_LITE_PROFILE`** —— 配置、缓存、下载都在 `.state/test-client`
3. **门禁** —— `xattr -dr com.apple.quarantine` 避免 ad-hoc 签名被拦

**必须用 `run.sh` 启动**，直接双击 app 仍会读正式版配置。

### `--demo` 的局限（重要）

demo 模式提供 800 条历史消息，可以测滚动、翻页、键盘操作。但：

- **没有任何引用消息** —— 无法验证引用定位
- **不连接真实微信** —— 状态永远是「演示模式 · 未连接真实微信」
- **数据是假的** —— 不能用它判断「真实群能不能收到」

需要验证引用、媒体解码、真实收发时，**必须用户在自己的服务端上手动执行**。

---

## 六、跨平台构建 Windows 产物（在 macOS 上）

```bash
export RUSTUP_HOME="$PWD/.tools/rustup" CARGO_HOME="$PWD/.tools/cargo"
cargo build --release --locked -p wechat-relay -p wechat-adapter \
    --target x86_64-pc-windows-gnu
```

产物在 `target/x86_64-pc-windows-gnu/release/`。

检查依赖只用了系统库（应为空输出）：

```bash
x86_64-w64-mingw32-objdump -p wechat-adapter.exe | awk '/DLL Name:/{print $3}' \
  | grep -viE '^(api-ms-win-|ext-ms-)|^(kernel32|ntdll|advapi32|bcrypt|bcryptprimitives|ws2_32|wsock32|combase|crypt32|iphlpapi|ole32|oleaut32|user32|shell32|shlwapi|secur32|wintrust|userenv|version|psapi|dbghelp|powrprof)\.dll$'
```

### 构建完直接替换到测试目录（不要丢给用户自己复制）

macOS 上编出来的 `.exe` 要落到**用户实际启动的那个目录**里。这个目录是共享目录，
不在仓库内：

```bash
TESTDIR="/Volumes/MacData/cal_data/Downloads/WeChatLite-构建产物/WeChatLite-Server-测试版"
# Windows 侧看到的是 \\Mac\Home\Downloads\WeChatLite-构建产物\WeChatLite-Server-测试版\
```

**注意不是 `~/Downloads`。** `~` 下面只有一个按日期归档的 `微信轻聊/`；
共享出去的是 `/Volumes/MacData/cal_data/Downloads/`。找错目录会以为「我明明覆盖了」，
实际改的是别处——这个坑已经踩过，白测一轮。

替换（保留一份旧版用于回滚）：

```bash
cp "$TESTDIR/adapter/wechat-adapter.exe" "$TESTDIR/adapter/wechat-adapter.exe.bak-old"
cp target/x86_64-pc-windows-gnu/release/wechat-adapter.exe "$TESTDIR/adapter/wechat-adapter.exe"
```

替换后**必须**验证落地的是不是新二进制，而不是靠时间戳猜：

```bash
python3 - "$TESTDIR/adapter/wechat-adapter.exe" <<'EOF'
import pathlib, sys
d = pathlib.Path(sys.argv[1]).read_bytes()
print("size", len(d))
for name, needle in {
    "旧 using thumbnail": "using thumbnail",
    "旧 分片超出数据范围": "WXGF 分片超出数据范围",
    "新 码流未以参数集开始": "WXGF 码流未以参数集开始",
    "新 placeholder.png": "placeholder.png",
    "新 启动戳": "wxgf=start-code-v2",
}.items():
    print(f"  {name:24}", "有" if needle.encode() in d else "无")
EOF
```

预期：`旧 *` 两项都是「无」，`新 *` 三项都是「有」。

### 换完文件 ≠ 跑的是新文件

`server_gui/server_window.cpp` 每次启动都会**改写 `bridge_program`**：

```cpp
config["bridge_program"] = QCoreApplication::applicationDirPath() + "/adapter/wechat-adapter.exe";
```

也就是说加载哪个适配器，取决于**你双击的那个 `wechat-server.exe` 在哪个目录**，
而不是你把新 exe 覆盖到了哪里。从旧便携包启动就会拉起旧便携包旁边的适配器。

所以适配器启动第一行会打印身份（`main.rs::announce_build`）：

```
[media] adapter build=0.2.0 exe=<完整路径> wxgf=start-code-v2
```

用户报「换了没变化」时，先要这一行。`exe=` 指向别处就说明启动错服务端了。

### 便携 EXE 不能手工替换内部文件

`WeChatLite-Server-Windows-x64-*.exe` 是自校验的便携包（格式见 `packaging/windows-portable/FORMAT.md`）：

```
[启动器 PE] [WLPKG01\n] [u64 清单长度] [JSON 清单] [各文件字节...] [WLPEND1\n] [u64 offset] [u64 size]
```

启动器把**整个 payload 的 SHA-256 编译在自己里面**。改动包内任何文件都会导致哈希不匹配、拒绝启动。

**所以更新适配器有两条路**：

1. **在 Windows 上重新打包**（推荐）：`scripts/package-portable-windows.ps1`
2. **解压后直接替换**（快速验证）：先 `unpack_portable.py unpack`，替换 `adapter/wechat-adapter.exe` 和 `wechat-relay.exe`，直接运行解压目录里的 `wechat-server.exe`

只读查看包内容：

```bash
python3 scripts/unpack_portable.py list <portable.exe>
```

---

## 七、必须由用户完成的验证

以下操作助手**一律不能代做**，只能给出步骤：

| 操作 | 为什么 |
|---|---|
| 在真实群发任何内容 | 不可撤回、影响他人 |
| 点「定位原文」验证引用 | 会读真实微信库对应行 |
| 替换 Windows 服务端文件 | 影响用户正在用的环境 |
| 重启真实服务端 | 会中断用户使用 |
| 修改真实配置文件 | 可能丢失凭据 |

给用户写步骤时，必须包含：

1. **前置检查**：先确认什么状态（例如 `/api/status` 返回什么）
2. **具体命令或点击位置**
3. **预期结果**：正常时看到什么
4. **失败时看什么**：哪个日志、哪个字段、什么错误文案
5. **回滚方式**：旧文件在哪、怎么恢复

---

## 八、诊断日志现状

**客户端目前没有通用日志。** 连接失败、WebSocket 断开、pin 校验失败都只能从界面看到一句话。

已有的只有一个：引用定位会写 `quote-locate.log` 到 `QStandardPaths::AppDataLocation`，内容是匹配过程（点了几条、有没有近似候选、最终命中哪条）。

适配器有 `[timing] poll/submit/confirmed` 输出到控制台（`59d4685` 加入），只在有控制台时可见。

**这是当前最大的可观测性缺口**：适配器卡死时，服务端 `ready` 仍然是 `true`，客户端显示已连接，界面上没有任何提示。

---

## 九、已知未验证项

截至 `dd4ebb3`：

| 项目 | 状态 | 原因 |
|---|---|---|
| 引用按 `svrid` 定位 | 仅内存 SQLite 验证 | 真实微信库的 `svrid` 语义未在真机确认 |
| WXGF 图片解码 | 单测覆盖 + 真机曾验证 | fixture 走真实 FFmpeg；真机布局见下节 |
| 连接圆点的四态 | mock HTTP 验证 | 真实断线恢复未验证 |
| 适配器快照复用 | **存在回归风险** | `release_changed()` 在文件被锁时静默跳过，可能永久卡在旧快照 |
| Windows 便携包重打包 | 未验证 | 需要 Windows + MSVC |

最后一项的细节：`db.rs` 里 `release_changed()` 对 `crypto::version()` 的错误是 `continue`（跳过），而微信写入时会对数据库持锁，可能导致这个分片的快照永不失效，表现为「服务端说连上了，但收不到新消息」。

---

## 十、WXGF 图片：已知布局与判断依据

### 容器布局

真实 1080x2400 照片的头部（`wxgf` 之后）：

```text
33..38   00 00 01 3a 31        记录标记（NAL type 29，分区号 '1'）
38..42   00 01 3a 24           长度 0x13a24 = 80420
42..46   00 00 00 01           四字节起始码
46..     40 01 ...             VPS，之后 SPS / PPS / slice
```

长度写在**起始码之前**。读错位置就会把头部字节当成长度：线上日志出现过
`声明 386096384 字节，剩余 88838 字节，偏移 12`，而 `0x17035d00` 是 `wxgf` 文件头，
不是任何长度。这类数字**不能**当成「图片还在下载」的证据。

### 现行解析策略

`media/wxgf.rs::partitions` 不遍历头部：

1. 扫描所有起始码；
2. 取起始码前 4 字节为长度，必须落在数据范围内；
3. 覆盖的字节必须「以参数集开头 + 只含一帧静图」（`picture_stream` 校验）；
4. 不合格就跳过、继续找下一个起始码，**不整张判死**；
5. 全部不合格且容器无长度前缀时，回退为扫 annex-B 码流。

颜色分片和透明遮罩分片都能拿到（`parts.len() == 2` 时按 chroma 区分）。

### 回退顺序

| 顺序 | 来源 | 失败后 |
|---|---|---|
| 1 | 完整 WXGF 解码 | → 2 |
| 2 | 同目录 `<md5>.dat` 缩略图（跳过刚失败的那个文件） | → 3 |
| 3 | 内置占位图 `placeholder.png`（写盘前自校验） | 结束 |

**只有「原消息已保留、会自动重试」才重试**；解码彻底失败给占位图，不会卡队列。

### 占位图必须本身可解码

`PLACEHOLDER_PNG` 曾经是 585 字节的截断 PNG（IDAT 声明 634 字节、CRC 错、zlib 空流），
于是占位图自己也被 `picture::validate` 拒掉，客户端只画「图片 · 点击下载」。
现在它是真 PNG，并且：

- 走 `Media::placeholder_picture()`，写盘前 `picture::validate` 一次；
- 文件名 `placeholder.png`，**不占用 `image.*`**——那个名字是后续成功解码的首查位置，
  放占位图会把真图永久挡住。

### 单元测试（FFmpeg fixture）

```bash
export TMPDIR="$PWD/.state/tmp-rs"
.tools/cargo/bin/cargo test -p wechat-adapter --bin wechat-adapter -- media::wxgf
```

包含：真实布局重放、颜色+遮罩合成、截断码流判为「仍在下载」、占位图可解码且不进 `image.*`。
`real_static_hevc_color_*` 需要系统里有 `ffmpeg`；没有时按第二节跳过。

