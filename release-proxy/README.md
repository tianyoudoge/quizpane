# QuizPane Release 下载代理（release-proxy）

单文件、零依赖的 Node 脚本，定期从 GitHub Releases API 拉取最新版本元数据，
把用户请求的安装包下载并缓存到本地磁盘，之后直接从磁盘提供下载（支持 Range
续传）。用来解决中国大陆用户直连 GitHub 下载慢/不稳定的问题。

只有一个源文件：`quizpane-release-proxy`，本身就是部署产物，不需要编译/打包
步骤，只依赖 Node 18+ 内置模块（`http`、`fs`、`crypto`）和全局 `fetch`。

## 它不做什么

- **不监听 GitHub Webhook**。GitHub Release 本身是公开只读的 API，不需要认证，
  也就不需要验证任何"外部请求是不是真的来自 GitHub"——真正需要 HMAC 验签的
  场景是"GitHub 主动推消息给我们的服务器"，而这个服务只是定期主动去问 GitHub
  "有没有新版本"，完全不对外暴露任何可以被陌生人触发写操作的接口，攻击面更小。
- **不是通用反向代理**。仓库固定为 `GITHUB_OWNER/GITHUB_REPOSITORY`，能下载的
  文件名必须是最近一次成功轮询里真实存在的 asset，其余一律 404。

## 部署前提

**这台服务器本身必须能稳定访问 `api.github.com` 和 `github.com`**，否则代理
拉取不到任何数据，`/healthz` 会一直是 `degraded`，整个方案就没有意义。如果你
的服务器在被墙的网络里，需要先解决服务器自身的出网问题（比如换到墙外机房），
这个代理解决的是"终端用户不用直连 GitHub"，不是"服务器不用直连 GitHub"。

## 环境变量

与 [`deploy/env/release-proxy.env.example`](../deploy/env/release-proxy.env.example) 一致：

| 变量 | 必填 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `GITHUB_OWNER` | 是 | — | 仓库所有者，例如 `tianyoudoge` |
| `GITHUB_REPOSITORY` | 是 | — | 仓库名，例如 `quizpane` |
| `GITHUB_TOKEN` | 否 | 空 | 公开仓库匿名也能读 Release；填了只是提高 API 速率限额 |
| `LISTEN_HOST` | 否 | `127.0.0.1` | 只监听本机，由 Nginx 反代到公网 |
| `LISTEN_PORT` | 否 | `8787` | 与 Nginx upstream 配置一致 |
| `CACHE_DIR` | 否 | `./data/releases` | 安装包磁盘缓存目录，生产环境是 `/srv/quizpane/releases` |
| `STATE_DIR` | 否 | `./data/state` | 元数据状态文件目录，生产环境是 `/var/lib/quizpane` |
| `CACHE_MAX_BYTES` | 否 | 30 GiB | 缓存容量上限，超出后按 LRU 清理 |
| `KEEP_RELEASES` | 否 | `2` | 无论是否超容量，最新的 N 个版本永不清理 |
| `POLL_INTERVAL_SECONDS` | 否 | `1800` | 轮询间隔，最小 60 秒 |
| `PREHEAT_ASSETS` | 否 | 空 | 逗号分隔的 asset 文件名，新版本出现时立即预热下载 |
| `LOG_LEVEL` | 否 | `info` | `error`/`warn`/`info`/`debug` |

## 本地运行

```bash
mkdir -p /tmp/qp-cache /tmp/qp-state
GITHUB_OWNER=tianyoudoge \
GITHUB_REPOSITORY=quizpane \
CACHE_DIR=/tmp/qp-cache \
STATE_DIR=/tmp/qp-state \
LISTEN_PORT=8787 \
PREHEAT_ASSETS=SHA256SUMS-linux-x86_64.txt \
node release-proxy/quizpane-release-proxy
```

启动时会立即轮询一次真实的 GitHub API。用小文件（如 `SHA256SUMS-*.txt`）做
`PREHEAT_ASSETS` 可以避免本地测试时下载几十 MB 的安装包。

### 手动验证

```bash
curl -s http://127.0.0.1:8787/healthz
curl -s http://127.0.0.1:8787/api/releases/latest
curl -s -o /tmp/out.txt http://127.0.0.1:8787/download/<tag>/SHA256SUMS-linux-x86_64.txt
curl -i -r 0-9 http://127.0.0.1:8787/download/<tag>/SHA256SUMS-linux-x86_64.txt   # 206 Partial Content
curl -i http://127.0.0.1:8787/download/<tag>/not-a-real-asset                    # 404
curl -s -X POST http://127.0.0.1:8787/internal/refresh                          # 仅本机可调用，立即重新轮询
```

## 路由

| 路由 | 行为 |
| --- | --- |
| `GET /healthz` | 始终 200；`status` 为 `ok`/`degraded`，附带 `currentTag`、`lastPollAt`、`cacheBytes` 等。即使上次轮询失败也返回 200，避免部署脚本的健康检查因 GitHub 临时不可达而误判。 |
| `GET /api/releases/latest` | 返回最新版本的 `tag`/`publishedAt`/`htmlUrl`/`assets`（每个 asset 含 `size`、`sha256`、`cached`）。从未成功轮询过时返回 503。 |
| `GET`/`HEAD /download/:tag/:asset` | 命中缓存直接从磁盘提供（支持 `Range`）；未命中则回源下载、校验 SHA-256/字节数后落盘再提供。不在最近元数据里的 tag/asset 返回 404。 |
| `POST /internal/refresh` | 仅回环地址（127.0.0.1/::1）可调用，立即触发一次轮询，用于手动刷新。 |

## 安全与缓存规则

- 下载 allowlist 通过内存中的"最近一次成功轮询得到的 releases 元数据"做精确
  匹配实现，不会把请求路径拼接进文件系统路径，天然拒绝路径穿越和任意文件名。
- 每次下载写入磁盘前先计算 SHA-256，并与 GitHub Release API 返回的
  `assets[].digest` 字段核对，同时核对字节数；任一不匹配都会删除临时文件、
  绝不会把不完整或被篡改的文件暴露给用户。
- 磁盘缓存永久保留最新 `KEEP_RELEASES` 个版本；其余版本仅保留最近 7 天内被
  访问过的；超出 `CACHE_MAX_BYTES` 时按最久未访问优先清理（LRU）。
- 元数据轮询失败不会清空已有缓存或让服务下线——继续用上一次成功轮询的数据
  服务用户，这是本代理存在的核心目的。

## 部署

跟 [`deploy/README.md`](../deploy/README.md) 里的通用流程一致：

```bash
sudo ./deploy/scripts/install-artifacts.sh \
  --version v0.3.0 \
  --proxy-binary release-proxy/quizpane-release-proxy
```

`install-artifacts.sh` 会把这个文件拷贝到
`/opt/quizpane/release-proxy/releases/<version>/quizpane-release-proxy`，切换
`current` 软链接，重启 `quizpane-release-proxy.service`（见
[`deploy/systemd/quizpane-release-proxy.service`](../deploy/systemd/quizpane-release-proxy.service)），
并请求本机 `/healthz` 做健康检查，失败会自动回滚到上一个版本。真实环境变量
写在服务器上的 `/etc/quizpane/release-proxy.env`（由
`deploy/scripts/bootstrap-ubuntu.sh` 首次创建），不要把这个文件提交回仓库。
