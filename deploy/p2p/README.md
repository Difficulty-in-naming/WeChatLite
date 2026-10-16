# 自建公网协商和中继入口

入口运行独立 Rust `iroh-relay` 1.3.0，不安装微信、不保存聊天数据库。这里只是 Linux 公网基础设施；Windows/macOS 客户端和本机微信服务端使用同一代 Rust 连接协议。

`install.py` 在 Debian/Ubuntu SSH 主机上以 root 执行，使用已核对 SHA-256 的官方 Linux x86_64 发行包。版本、下载地址及摘要分别为：

- [上游 1.3.0 发行](https://github.com/n0-computer/iroh/releases/tag/v1.3.0)
- `iroh-relay-v1.3.0-x86_64-unknown-linux-gnu.tar.gz`
- `50e0158f6b9aefc95b45b515485d3b94d786fb1bfa4155d25937c71b81905f8f`

将该发行包与本目录三个安装文件上传到已授权 SSH 主机的独立目录，在主机执行：

```sh
sudo python3 install.py --archive ./iroh-relay-v1.3.0-x86_64-unknown-linux-gnu.tar.gz --public-ip YOUR_PUBLIC_IPV4
```

安装创建专用无登录账户 `wechat-p2p`、`/opt/wechat-p2p/1.3.0`、`/etc/wechat-p2p` 和独立 `wechat-p2p-relay.service`。现有 SSH、反代、站点和防火墙配置不修改。启动默认不开放 metrics，仅监听 TCP/UDP 54443；HTTP 辅助监听只在 `127.0.0.1:55440`。需要在云安全组/轻量服务器防火墙中另外放行 54443 的 TCP 和 UDP。

入口有单独随机接入凭据、连接速率和每端点 2 MiB/s 入站上限。systemd 使用只读系统目录、无额外权限、192 MiB 内存上限和半个 CPU 核心的配额；这是资源上限，不是实测消耗。已安装的身份及凭据不会在重复运行安装器时重置。若改变版本或 IP，安装器停止并要求检查升级方案。

入口完整连接信息在 `/etc/wechat-p2p/endpoint.json`，包含基础设施接入凭据，应通过 SSH 安全取回后导入本机服务端 GUI。它不进入源码或客户端/服务端通用运行包。私有 CA 私钥留在云主机；证书公有部分与客户端邀请同行，应用仍进行正常证书、名称和设备身份校验。当前叶证书有效期三年，CA 十年；在到期前用相同 CA 更新叶证书并重启本服务，CA 本身轮换则需要更新所有客户端入口信息。

管理命令：

```sh
sudo systemctl status wechat-p2p-relay.service
sudo systemctl restart wechat-p2p-relay.service
sudo journalctl -u wechat-p2p-relay.service -n 30
```

查看服务是否 active 只证明主机内进程状态。还需从外部使用配置中的公有 CA 验证 HTTPS `/healthz`，再运行真实客户端确认 QUIC 建联与中继，不能关闭 TLS 验证代替此检查。跨网直连成功率和中继吞吐随网络与云带宽而变。
