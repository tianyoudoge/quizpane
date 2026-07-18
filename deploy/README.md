# Ubuntu 部署脚本（官网与 Release 下载代理）

这套脚本准备并发布两部分制品：官网静态页（[`website/`](../website/README.md)）
和 Release 下载代理（[`release-proxy/`](../release-proxy/README.md)）。两者都
已实现，可以构建并部署。

## 文件职责

| 文件 | 作用 |
| --- | --- |
| `scripts/bootstrap-ubuntu.sh` | 安装 Ubuntu 依赖、创建最小权限用户/目录、写入 Nginx 与 systemd 配置；不部署真实页面或代理程序本身。 |
| `scripts/install-artifacts.sh` | 将 [`website/`](../website/README.md) 构建出的静态站、[`release-proxy/`](../release-proxy/README.md) 的可执行文件原子切换到 `current` 软链接，并重载相关服务。 |
| `scripts/build-and-deploy-site.sh` | 封装“本机构建 `website/dist` + 调用 `install-artifacts.sh`”，支持本机直接部署或通过 SSH 部署到远程服务器。 |
| `nginx/quizpane.conf.template` | 网站、API、下载的反向代理和限流规则。 |
| `nginx/quizpane-global.conf` | 必须在 Nginx `http` 级别加载的 upstream、元数据缓存和限流 zone。 |
| `systemd/quizpane-release-proxy.service` | 将代理作为非 root 的 `quizpane` 系统服务运行。 |
| `env/release-proxy.env.example` | 服务运行环境变量样例；部署后的真实文件仅保存在服务器。 |

代理服务只做定时轮询 GitHub Releases API，不监听 Webhook，因此不需要任何
HMAC secret，也没有对外暴露可被陌生人触发写操作的接口，见
[`release-proxy/README.md`](../release-proxy/README.md)。

## 首次初始化 Ubuntu 22.04 / 24.04

前提：域名 A/AAAA 记录已经解析到服务器，且 80/443 可从公网访问；**服务器本身
能够稳定访问 `api.github.com` 和 `github.com`**（代理服务器如果在被墙网络里，
轮询会一直失败，整个方案失去意义）。以下命令在本仓库目录执行：

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
/srv/quizpane/releases/            # Release 安装包磁盘缓存
/var/lib/quizpane/                 # 代理的元数据状态文件
/opt/quizpane/release-proxy/       # 代理可执行文件及 current 软链接
/etc/quizpane/release-proxy.env    # 仅服务器保存的运行配置，0600
```

不要把 `/etc/quizpane/release-proxy.env` 复制回仓库；部署后按需在服务器上
填写 `GITHUB_TOKEN`（公开仓库匿名也能读取 Release，填 token 只是为了提高
GitHub API 速率限额）。

如果域名或证书尚未就绪，可先不传 `--issue-cert`。Nginx 会提供一个最小的“正在准备”页面，待 DNS 就绪后再执行：

```bash
sudo certbot --nginx -d quizpane.example.com -m ops@example.com --agree-tos --redirect
```

## 制品发布

在服务器上执行（或由 CI 通过 SSH 调用）：

```bash
sudo ./deploy/scripts/install-artifacts.sh \
  --version v0.3.0 \
  --site-dist /tmp/quizpane-site-dist \
  --proxy-binary release-proxy/quizpane-release-proxy
```

`--site-dist` 必须含 `index.html`（用 `node website/scripts/build-site.mjs` 生成，见 [`website/README.md`](../website/README.md)）；`--proxy-binary` 必须是可执行文件（`release-proxy/quizpane-release-proxy` 本身就是可直接使用的单文件脚本，不需要额外构建）。两者可单独安装，例如只切换官网页面：

```bash
sudo ./deploy/scripts/install-artifacts.sh --version site-20260716 --site-dist /tmp/quizpane-site-dist
```

每次发布会先拷贝到新的不可变版本目录，然后才更新 `current` 软链接。静态页重载 Nginx，代理服务重启后会请求本机 `/healthz`；代理启动或健康检查失败会恢复到上一个代理软链接并返回非零，便于 CI 告警。

### 只发布静态站

在仓库根目录本机构建并直接安装（脚本内部会 `sudo` 调用 `install-artifacts.sh`）：

```bash
./deploy/scripts/build-and-deploy-site.sh
```

如果这台机器不是服务器本身，用 `--remote-host` 通过 SSH 构建产物同步 + 远程安装（需要该账户对 `install-artifacts.sh` 有免密 sudo）：

```bash
./deploy/scripts/build-and-deploy-site.sh \
  --remote-host deploy@quizpane.example.com \
  --remote-repo /srv/quizpane-src
```

两种方式都会先跑 `node website/scripts/build-site.mjs` 重新构建 `website/dist`，
版本号默认是 `site-<UTC 时间戳>`，可用 `--version` 覆盖。

### 只发布 Release 代理

```bash
sudo ./deploy/scripts/install-artifacts.sh \
  --version proxy-20260716 \
  --proxy-binary release-proxy/quizpane-release-proxy
```

首次启用后确认 `/etc/quizpane/release-proxy.env` 里 `GITHUB_OWNER`/
`GITHUB_REPOSITORY` 正确，再 `systemctl restart quizpane-release-proxy`。

## 运维检查

```bash
sudo nginx -t
sudo systemctl status nginx quizpane-release-proxy
sudo journalctl -u quizpane-release-proxy -n 100 --no-pager
curl -i http://127.0.0.1:8787/healthz
curl -s http://127.0.0.1:8787/api/releases/latest
sudo du -sh /srv/quizpane/releases
```

`/healthz` 返回 `status: "degraded"` 通常意味着服务器无法访问 GitHub，或
GitHub API 临时限流；代理会继续用上一次成功轮询的缓存数据服务用户，不会
下线。下载代理固定仓库为 `tianyoudoge/quizpane`，仅允许最近一次成功轮询的
metadata 中存在的 asset，完整下载及 SHA-256 校验通过后才公开缓存文件。Nginx
只缓存 `/api/releases/latest` 这类小 JSON，安装包的 Range 请求由服务自身
落盘处理，详见 [`release-proxy/README.md`](../release-proxy/README.md)。

## 挂载到个人域名的子路径

Nginx 模板同时提供 `https://域名/quizpane/` 路由，适合把官网挂在个人主页下。
静态资源、版本 API 和下载代理都会保留该路径前缀；`/quizpane` 会自动跳转到
`/quizpane/`，确保相对资源路径正确。只需把 `server_name` 改为真实域名并重载
Nginx，不需要新增 DNS 记录。
