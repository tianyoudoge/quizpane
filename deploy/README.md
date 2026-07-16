# Ubuntu 部署脚本（官网与 Release 下载代理）

这套脚本只准备运行环境与未来制品的发布路径：Nginx、HTTPS、systemd、静态站目录、Release 下载缓存目录。当前仓库尚未实现官网静态页和 `quizpane-release-proxy` 服务，因此不要把它当成可立即上线的完整网站。

## 文件职责

| 文件 | 作用 |
| --- | --- |
| `scripts/bootstrap-ubuntu.sh` | 安装 Ubuntu 依赖、创建最小权限用户/目录、写入 Nginx 与 systemd 配置；不部署真实页面或代理程序。 |
| `scripts/install-artifacts.sh` | 将未来构建出的静态站和代理二进制原子切换到 `current` 软链接，并重载相关服务。 |
| `nginx/quizpane.conf.template` | 网站、API、下载、Webhook 的反向代理和限流规则。 |
| `nginx/quizpane-global.conf` | 必须在 Nginx `http` 级别加载的 upstream、元数据缓存和限流 zone。 |
| `systemd/quizpane-release-proxy.service` | 将未来代理作为非 root 的 `quizpane` 系统服务运行。 |
| `env/release-proxy.env.example` | 服务运行环境变量样例；部署后的真实文件仅保存在服务器。 |

## 首次初始化 Ubuntu 22.04 / 24.04

前提：域名 A/AAAA 记录已经解析到服务器，且 80/443 可从公网访问。以下命令在本仓库目录执行：

```bash
chmod +x deploy/scripts/*.sh
sudo ./deploy/scripts/bootstrap-ubuntu.sh \
  --domain quizpane.example.com \
  --email ops@example.com \
  --configure-firewall \
  --issue-cert
```

脚本会创建：

```text
/srv/quizpane/site/releases/       # 每次静态站发布的不可变目录
/srv/quizpane/site/current         # Nginx 当前站点软链接
/srv/quizpane/releases/            # 未来 Release 安装包磁盘缓存
/var/lib/quizpane/                 # 元数据、去重及运行状态
/opt/quizpane/release-proxy/       # 未来代理二进制及 current 软链接
/etc/quizpane/release-proxy.env    # 仅服务器保存的密钥与运行配置，0600
```

首次运行时会生成 `WEBHOOK_SECRET`。不要把 `/etc/quizpane/release-proxy.env` 复制回仓库。上线 GitHub Webhook 前，先读取该值并在仓库 **Settings → Webhooks** 中设置为同一个 secret；事件只勾选 **Releases**。

如果域名或证书尚未就绪，可先不传 `--issue-cert`。Nginx 会提供一个最小的“正在准备”页面，待 DNS 就绪后再执行：

```bash
sudo certbot --nginx -d quizpane.example.com -m ops@example.com --agree-tos --redirect
```

## 未来制品发布

静态页面和代理服务完成后，在服务器上执行（或由 CI 通过 SSH 调用）：

```bash
sudo ./deploy/scripts/install-artifacts.sh \
  --version v0.3.0 \
  --site-dist /tmp/quizpane-site-dist \
  --proxy-binary /tmp/quizpane-release-proxy
```

`--site-dist` 必须含 `index.html`；`--proxy-binary` 必须是可执行文件。两者可单独安装，例如先切换官网页面：

```bash
sudo ./deploy/scripts/install-artifacts.sh --version site-20260716 --site-dist /tmp/quizpane-site-dist
```

每次发布会先拷贝到新的不可变版本目录，然后才更新 `current` 软链接。静态页重载 Nginx，代理服务重启后会请求本机 `/healthz`；代理启动或健康检查失败会恢复到上一个代理软链接并返回非零，便于 CI 告警。

## 运维检查

```bash
sudo nginx -t
sudo systemctl status nginx quizpane-release-proxy
sudo journalctl -u quizpane-release-proxy -n 100 --no-pager
curl -i http://127.0.0.1:8787/healthz
sudo du -sh /srv/quizpane/releases
```

下载代理服务本身仍待实现。实现时必须固定仓库为 `tianyoudoge/quizpane`，仅允许 metadata 中存在的 asset，验证 GitHub `X-Hub-Signature-256`，并在完整下载及 hash 校验后才公开缓存文件。Nginx 只缓存 `/api/releases/latest` 这类小 JSON，安装包的 Range 请求由服务自身落盘处理。
