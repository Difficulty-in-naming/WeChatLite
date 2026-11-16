# 测试版服务端：完整操作步骤

目标：用**已替换好适配器**的测试目录跑一遍，确认图片能正常收下来，同时不影响你现在用的正式便携包。

> **先读第 0 步。** 这份文档重写的原因，就是上一版漏了它——改了代码、换了 exe，
> 结果测试时加载的还是旧文件，日志全是旧文案，白测一轮。

---

## 第 0 步（最重要）：确认跑的是哪一份适配器

### 为什么必须单独确认

`adapter/wechat-adapter.exe` 换掉了，**不代表它会被加载**。原因在
`server_gui/server_window.cpp`：

```cpp
const auto configured = config.value("bridge_program").toString();
if (configured.isEmpty() || QFileInfo(configured).fileName() == "wechat-adapter.exe")
{
    config["bridge_program"] = QCoreApplication::applicationDirPath() + "/adapter/wechat-adapter.exe";
}
```

管理程序**每次启动都会把 `server.json` 里的 `bridge_program` 改写成「我自己所在目录/adapter/wechat-adapter.exe」**。

推论（这就是踩过的坑）：

- 你从 `D:\旧便携包\wechat-server.exe` 启动 → 加载的是 `D:\旧便携包\adapter\` 里的适配器；
- 你把新 exe 覆盖到了测试目录，但启动的是旧便携包 → **跑的仍然是旧适配器**；
- 托盘里点开的管理窗口，也是它自己那个目录。

所以「覆盖了哪个 adapter 目录」不决定结果，「双击了哪个 `wechat-server.exe`」才决定。

### 逐条对照：旧版 vs 修复版

两版的日志文案**不同**，可以直接用来判断。旧版已经不存在于新二进制里：

| 日志文案 | 旧版（有 bug） | 修复版 |
|---|---|---|
| `adapter build=... exe=... wxgf=start-code-v2` | 没有 | **启动第一行就有** |
| `[media] wxgf decode failed, using thumbnail: ...` | 有 | **不存在这个字符串** |
| `[media] wxgf decode failed: ...` | 没有 | 有 |
| `[media] picture not displayable (...), using a placeholder` | 有 | 删除 |
| `[media] no displayable picture, using a placeholder` | 没有 | 有 |
| 错误里出现 `WXGF 分片超出数据范围（声明 386096384 字节...）` | 有 | **不会再有** |
| 错误里出现 `WXGF 码流未以参数集开始` / `WXGF 图像码流缺失` | 没有 | 有 |

一句话判断：**控制台第一行有 `wxgf=start-code-v2` = 修复版在跑。**

### 启动前先核对文件（3 秒）

在测试目录里执行（PowerShell）：

```powershell
Get-Item .\adapter\wechat-adapter.exe | Select-Object Length, LastWriteTime
Select-String -LiteralPath .\adapter\wechat-adapter.exe -Pattern 'wxgf=start-code-v2' -SimpleMatch -Quiet
```

预期：

- `Length` ≈ **8306688**（7.9 MB）
- 第二条输出 **True**

`False` 就说明这个文件不是修复版，别往下走，先把它覆盖掉。

> 直接双击 `启动测试版.bat` 也会做这项检查：不匹配就直接退出并给出提示。

---

## 第 1 步：停掉原来的服务端

**为什么必须停**——三个硬冲突：

1. **端口冲突**：测试版复制你的配置，用同一个 `10.211.55.8:8765`。原来的还在跑，新的启动就会报「无法监听配置的服务地址」。
2. **数据目录锁**：`data\.relay.lock` 是排他锁，两个进程指向同一份数据时第二个抢不到。
3. **适配器抢微信库**：两个 `wechat-adapter.exe` 同时读微信数据库会互相干扰，读到的可能是残缺快照。

**怎么停**：

1. Windows 托盘 → 微信轻聊服务端图标 → 右键
2. 选 **「停止服务并退出」**

不要只关窗口，那只是收起到托盘。

**确认停干净**：任务管理器（Ctrl+Shift+Esc）→ 详细信息 → 搜 `wechat-relay`、`wechat-adapter`、`wechat-server`，三个都不应该出现。

---

## 第 2 步：备份配置（建议）

配置默认在这里：

```
%LOCALAPPDATA%\WeChatLite\WeChatLiteServer\server.json
```

要确认准确路径：打开原来的服务端管理窗口 → **「服务状态」页** → 看「配置」那一行。

把这个 `server.json` 复制到桌面，顺便看一眼里面 `data` 字段指向哪个目录。

> 不想保留测试期间的消息可以跳过备份——原目录不会被测试版写坏，但会**追加**测试期的收发记录。

---

## 第 3 步：打开测试目录

共享目录里已经准备好，**不用解压**：

```
\\Mac\Home\Downloads\WeChatLite-构建产物\WeChatLite-Server-测试版\
```

对应 macOS 侧的真实路径：

```
/Volumes/MacData/cal_data/Downloads/WeChatLite-构建产物/WeChatLite-Server-测试版/
```

> 注意：不是 `~/Downloads`。`~` 下面只有一个按日期归档的 `微信轻聊/`，
> 共享出来的是 `/Volumes/MacData/cal_data/Downloads/`。
> 找错目录会让「我明明覆盖了」变成一场空。

---

## 第 4 步：启动（推荐用 `启动测试版.bat`）

### 方式 A：双击 `启动测试版.bat`（推荐）

脚本会依次做：

1. 检查 `wechat-relay` / `wechat-adapter` / `wechat-server` 是否还在跑，会给强制结束选项；
2. 检查 `wechat-server.exe`、`adapter\wechat-adapter.exe`、`adapter\ffmpeg.exe` 是否齐全；
3. **核对适配器是不是修复版**（不含 `wxgf=start-code-v2` 就直接退出）；
4. 用原配置 `--config "%LOCALAPPDATA%\WeChatLite\WeChatLiteServer\server.json" --start` 启动。

### 方式 B：命令行指定配置

在测试目录里 `Shift` + 右键 → 「在此处打开 PowerShell 窗口」：

```powershell
.\wechat-server.exe --config "$env:LOCALAPPDATA\WeChatLite\WeChatLiteServer\server.json" --start
```

> 路径两边**必须**有引号，里面有空格。

### 方式 C：独立配置（完全隔离）

不碰原配置，但要重新填密钥、重新选群：

1. 先把 `%LOCALAPPDATA%\WeChatLite\WeChatLiteServer\server.json` 改名；
2. 直接双击 `wechat-server.exe`。

**验证适配器行为建议用 A。** 隔离没有必要。

---

## 第 5 步：确认起来了（关键是第一行）

启动后应该有：

1. **一个黑色控制台窗口**（正式便携包没有，因为它是 GUI 子系统程序）——**别关它**
2. 管理窗口打开，「服务状态」页
3. 状态变成绿色「运行中」

控制台**第一段**输出应该是：

```
[media] adapter build=0.2.0 exe=C:\...\WeChatLite-Server-测试版\adapter\wechat-adapter.exe wxgf=start-code-v2
[media] account_dir=C:\Users\...\xwechat_files\wxid_... account=wxid_... wxid=wxid_... images=true
```

**逐字核对 `exe=` 那一段**：它必须指向你刚才核对过的那个 `adapter\wechat-adapter.exe`。
指向别处（例如旧便携包），说明启动的还是旧服务端，回第 0 步。

后面会持续出现：

```
[timing] inbox poll groups=... due=... converted=... took=...ms send_job=false
```

---

## 第 6 步：测图片（本次改动的核心）

1. 手机微信往测试群（`56266522281@chatroom`「测试群」）发一张**照片**（不要发截图/GIF，先测普通照片）；
2. 等 5–10 秒；
3. 看客户端里图片有没有直接显示出来，同时盯控制台。

### 结果对照

**A. 图片正常显示，控制台没有 `wxgf decode failed`**

→ 修复生效。

**B. 图片显示但控制台有 `[media] wxgf decode failed: ...`**

→ 解码失败后退到了缩略图。**把这一行原文贴出来**，括号里有具体原因：

| 括号里的内容 | 含义 |
|---|---|
| `缺少独立 FFmpeg 解码器` | `adapter\ffmpeg.exe` 不在，或被杀毒隔离 |
| `WXGF 码流未以参数集开始` / `图像码流缺失` | 容器布局又变了，需要新样本 |
| `WXGF 图像未完整解码：<ffmpeg stderr>` | HEVC 码流本身被 FFmpeg 拒了 |
| `WXGF 分片超出数据范围（声明 ...）` | **修复版不该再出现**；出现说明跑的仍是旧 exe |

**C. 图片位置是「图片 · 点击下载」**

→ 占位图或缩略图都没拿到。看控制台有没有：

```
[media] no displayable picture, using a placeholder
[media] picture unavailable, sending a placeholder: ...
```

把后面的错误原文贴出来。

**D. 控制台完全没有 `[media]` 开头的新行**

→ 服务端没连上微信，或没进群同步。看是否有 `微信新分片密钥尚不可用`。

---

## 第 7 步：测引用定位

新消息收发正常后再做。

1. 群里找一条**引用消息**
2. 点引用块 → **「定位到原文位置」**

| 看到什么 | 含义 |
|---|---|
| 「已定位到原文」并跳过去 | 成功 |
| 「已定位到原文（按发送者和内容匹配）」 | 客户端本地匹配成功 |
| 「已加载的消息里没有找到这条引用的原文」 | 本地没有，需要翻历史 |
| 「…（no stored row has server_id N）」 | 微信库里没这个编号 |
| 「…（not unique in this group）」 | 同编号多行，拒绝猜 |
| 「…（was recalled）」 | 原消息已撤回 |
| **「原消息尚未同步或不可用」** | **跑的还是旧适配器** |

---

## 第 8 步：有问题怎么办

### 「无法监听配置的服务地址」

原服务端没停干净。回第 1 步，用任务管理器确认三个进程都消失。

### 「此配置的服务端管理窗口已经在运行」

原管理窗口还开着（`QLockFile` 拦住）。从托盘图标打开原来那个，正常退出。

### 「微信新分片密钥尚不可用」

适配器读不到微信数据库密钥。等 30 秒，或重启一次测试服务端。也可能是微信没登录。

### 图片一直「微信附件暂未就绪」

1. 检查 `adapter\ffmpeg.exe` 在不在（约 87 MB）；
2. 说明：**只有「原消息已保留、会自动重试」才会重试**，解码彻底失败会给占位图，不会卡队列。

### 换了 exe 但日志一点没变

99% 是第 0 步的问题：启动的是别处的 `wechat-server.exe`。
用任务管理器看 `wechat-adapter.exe` 的「命令行/所在目录」，或看控制台第一行 `exe=`。

---

## 第 9 步：测完怎么恢复

1. 停掉测试服务端（关控制台窗口，或托盘 → 停止服务并退出）；
2. 双击 `\\Mac\Home\Downloads\WeChatLite-构建产物\WeChatLite-Server-Windows-x64-patched.exe`。

**测试目录随便删**，不影响任何东西。

> 便携 EXE 是自校验包，**不能手工替换内部文件**（会哈希不匹配、拒绝启动）。
> 要更新它只能在 Windows 上重新打包，或者解压后替换再运行解压目录里的 `wechat-server.exe`。
> 见 `docs/LOCAL_TESTING.md` 第六节。

> 测试期间的收发记录写在 `server.json` 的 `data` 指向的目录里。用方式 A 会**进入你原来的数据目录**——这是好事，切回去消息还在。

---

## 第 10 步：贴给我的信息

按重要程度：

1. **控制台从启动到出问题的完整输出**，尤其第一行 `adapter build=... exe=...`
2. 客户端上的**具体提示文字**
3. 出问题的**时间点**
4. 「服务状态」页显示的**配置**和**数据目录**

第 1 条里的 `exe=` 能直接排除「跑错文件」这一类问题，省掉一整轮来回。
