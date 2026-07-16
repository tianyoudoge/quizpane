# QuizPane 官网静态页与 Release 下载代理方案

> 状态：官网静态页与 Release 下载代理均已实现（见 [`website/README.md`](../website/README.md)、[`release-proxy/README.md`](../release-proxy/README.md)）。官网截图和图标仍是占位 SVG，发布前需替换。下载代理只做定时轮询，不监听 Webhook。Ubuntu 部署脚本已准备，见 [`deploy/README.md`](../deploy/README.md)。

## 1. 目标与边界

在一台 Ubuntu 服务器上部署 `quizpane.example.com`（实际域名确认后替换），提供：

1. 一个响应式中文产品静态页，介绍小窗刷题与本地 PDF 题库整理能力；
2. 基于真实产品操作录制的功能轮播和宣传截图；
3. 面向中国用户的 Release 下载入口，服务器从 GitHub Release 拉取、落盘缓存后分发；
4. 定时轮询 GitHub 检查新 Release，自动刷新下载元数据并预热最新版核心安装包。

不在官网上传用户 PDF，不把用户资料传到第三方，也不把题库生成服务暴露成公网 API。

## 2. 页面信息架构

页面采用单页静态站，桌面端有顶栏锚点，移动端折叠菜单。整体视觉建议沿用 QuizPane 的深色桌面端调性：深石墨背景、低饱和蓝色强调、产品截图作为视觉中心，不做夸张的 AI 概念图。

| 区块 | 页面内容 | 主要动作 |
| --- | --- | --- |
| 首屏 Hero | 标题“把 PDF 真题变成能刷的题库”、一句本地优先说明、产品主截图 | 下载最新版 / 查看功能 |
| 可信能力条 | 本地处理、PDF 图表保留、多选题支持、小窗刷题 | 跳转功能区 |
| 功能轮播 Banner | 4 张真实产品截图自动播放，可手动切换 | 查看对应说明 |
| 工作流 | 导入题目 PDF + 答案 PDF → 自动整理 → 复核 → 小窗刷题 | 了解题库制作器 |
| 下载区 | 按 macOS Apple Silicon、macOS Intel、Windows 分卡片展示版本、大小、发布日期、SHA-256 | 通过本站下载代理下载 |
| 隐私与开源 | 本地规则模式不上传资料、GitHub 仓库与 Release 链接 | 查看仓库 |
| 页脚 | 版本、更新日期、GitHub、备案/联系信息（如有） | 外链 |

### 功能轮播内容

1. **PDF 导入与答案配对**：题目 PDF、答案 PDF 被添加到题库制作器；强调“规则整理可离线完成”。
2. **资料题与图表保留**：116–120 题的材料卡展示跨页文字和统计图；强调“图表不再丢失”。
3. **图片/公式选项**：76 题展示 A/B/C/D 选项图；强调“选项是图片也可直接作答”。
4. **小窗刷题**：题库自动载入后在小窗中作答，展示答题状态与复盘入口。

轮播使用 CSS `scroll-snap` + 少量原生 JavaScript 实现：默认每 6 秒切换、鼠标悬停/键盘焦点时暂停、支持左右按钮、圆点与 `prefers-reduced-motion`。不依赖大型前端框架。

## 3. 宣传截图采集方案

### 输入与操作

使用以下本地文件创建演示题库：

- `/Volumes/macmini_data/downloads/2011北京卷-试题.pdf`
- `/Volumes/macmini_data/downloads/2011北京卷-答案.pdf`

采集流程：

1. 在题库制作器选择“离线整理”，添加题目与配对答案；
2. 命名为“2011 北京卷 · 演示题库”，完成安装；
3. 小窗刷题中打开该题库；
4. 截取上表定义的四个状态，统一裁为 16:10，并输出 `web/assets/screenshots/*.webp`（同时保留 PNG 原图）；
5. 页面只使用截图，不上传/托管原 PDF 或完整题库文件。

发布前应确认试卷内容的展示与传播授权；如无法确认，截图应只保留产品框架、局部脱敏的题干与图表，避免将整份试题作为官网可下载内容。

## 4. 工程结构（已实现部分见下方标注）

```text
website/                      # 已实现，见 website/README.md
  src/                        # 静态 HTML、CSS、原生 JS，无框架
    index.html
    main.css
    main.js
    content.json              # 页面文案与下载平台配置
    assets/icons/              # 图标，当前为占位 SVG
    assets/screenshots/        # 轮播截图，当前为占位 SVG，发布前需替换真实截图
  scripts/build-site.mjs      # 已实现：拷贝 src → dist，对 CSS/JS 做内容哈希
  dist/                       # 构建产物，不提交
release-proxy/                 # 已实现，见 release-proxy/README.md
  quizpane-release-proxy       # 单文件 Node 脚本，本身即部署产物
  README.md
deploy/                        # 已实现
  nginx/quizpane.conf.template
  nginx/quizpane-global.conf
  scripts/bootstrap-ubuntu.sh
  scripts/install-artifacts.sh
  scripts/build-and-deploy-site.sh  # 本机构建 + 本地/远程安装静态站
```

静态页目前是完全无框架的原生 HTML/CSS/JS；页面内容存放在本地 `content.json`，
版本信息通过 `/api/releases/latest` 动态读取，接口不存在或返回非 2xx 时降级为
跳转 GitHub Release 页面。

Release 代理是单文件、零依赖的 Node 脚本（只用内置模块和全局 `fetch`），本身
就是部署产物，不需要编译/打包步骤，与 `deploy/scripts/install-artifacts.sh
--proxy-binary` 期望的"单个可执行文件"契约直接匹配。

仓库现已提供部署基础设施：`deploy/scripts/bootstrap-ubuntu.sh` 初始化 Ubuntu、Nginx、systemd 和运行配置文件；`deploy/scripts/install-artifacts.sh` 原子发布静态站与代理制品；`deploy/scripts/build-and-deploy-site.sh` 封装静态站的构建与发布。三者均不导入 PDF 或采集宣传图。

## 5. Release 代理缓存架构

```text
浏览器
  │  GET /download/v0.2.12/QuizPane-macos-arm64.dmg
  ▼
Nginx（TLS、限流、静态站、缓存响应头）
  │
  ▼
release-proxy（只允许白名单仓库与白名单 asset，定时轮询，不监听外部 Webhook）
  ├── state.json：Release、asset、SHA-256、缓存状态
  ├── /srv/quizpane/releases：完整安装包磁盘缓存
  └── GitHub API + Release asset 下载
          │
          ▼
      github.com/tianyoudoge/quizpane Releases
```

### 路由与行为

| 路由 | 行为 |
| --- | --- |
| `GET /api/releases/latest` | 返回本站缓存的最新版 metadata；从未成功轮询过时返回 503。 |
| `GET`/`HEAD /download/:tag/:asset` | 仅允许 metadata 中存在的 asset；命中本地文件直接支持 Range 下载；未命中时回源 GitHub、流式写盘、SHA-256/字节数校验通过后才提供下载。 |
| `POST /internal/refresh` | 仅本机（127.0.0.1/::1）可调用，用于手动触发立即轮询。 |

### 安全和缓存规则

- **不是通用代理**：仓库固定为 `tianyoudoge/quizpane`，文件名必须来自最近一次成功轮询得到的 Release metadata，拒绝任意 URL、重定向目标和路径穿越。
- **无 Webhook，仅定时轮询**：不监听任何外部触发请求，服务器主动、周期性地向 GitHub 发起只读查询；不需要验证任何入站请求的来源，天然没有伪造 Webhook 请求触发缓存刷新的攻击面。
- 下载接口允许 `GET`/`HEAD`，限制单 IP 并发与带宽；`/internal/refresh` 仅回环地址可调用。
- 缓存容量默认 30 GiB，保留“当前正式版 + 上一个正式版 + 最近 7 天访问版本”；LRU 清理前不删除当前版本。
- 所有下载在写盘前用 GitHub 提供的 asset digest（SHA-256）和字节数校验，校验通过才对外暴露；网页显示 SHA-256，客户端可自行核验。
- Nginx 可额外缓存小型 JSON 响应；大安装包由应用落盘缓存，避免 Nginx 对 Range/回源中断造成不完整缓存。

GitHub 的 Release API 可取得最新正式 Release 与 asset 元数据；公开仓库的公开 Release 可以不带认证读取，每个 asset 自带 `digest` 字段（`sha256:<hex>`），不需要额外解析 checksum 文件。相关官方文档：

- [GitHub Releases REST API](https://docs.github.com/en/rest/releases/releases)

## 6. Ubuntu 22.04/24.04 部署指南

### 6.1 前置条件

- 一个已解析到服务器公网 IPv4/IPv6 的域名，例如 `quizpane.example.com`；
- Ubuntu 22.04 或 24.04，建议 2 vCPU / 4 GiB RAM / 50 GiB SSD 起步；
- 开放 80/443；SSH 仅限管理员 IP 或密钥登录；
- **服务器本身能稳定访问 `api.github.com` 和 `github.com`**：下载代理只做定时轮询，不依赖 GitHub 主动通知，但代理仍需要主动出网才能拉取数据；如果服务器在被墙网络里，需要先解决出网问题；
- 站点域名、邮箱地址（用于 Let's Encrypt 证书）。

### 6.2 基础初始化

```bash
sudo apt update && sudo apt -y upgrade
sudo apt install -y nginx certbot python3-certbot-nginx ufw curl ca-certificates
sudo adduser --system --group --home /srv/quizpane quizpane
sudo install -d -o quizpane -g quizpane /srv/quizpane/releases /var/lib/quizpane
sudo ufw allow OpenSSH
sudo ufw allow 'Nginx Full'
sudo ufw enable
```

应用服务以 `quizpane` 用户运行；GitHub token（可选）通过 `/etc/quizpane/release-proxy.env`（权限 `0600`）注入。

### 6.3 Nginx 与 HTTPS

Nginx 负责：

- `/` 提供 `website/dist` 的静态文件并回退至 `index.html`；
- `/api/` 反向代理至 `127.0.0.1:8787`；
- `/download/` 反向代理至同一服务，但关闭缓冲、超时设置得更长、允许 Range；
- `Strict-Transport-Security`、CSP、`X-Content-Type-Options` 等安全响应头；
- 对下载设置限流规则。

首次部署后：

```bash
sudo ln -s /etc/nginx/sites-available/quizpane.conf /etc/nginx/sites-enabled/quizpane.conf
sudo nginx -t && sudo systemctl reload nginx
sudo certbot --nginx -d quizpane.example.com -m ops@example.com --agree-tos --redirect
```

Nginx 可用 `proxy_cache_path`、`proxy_cache_lock` 和过期后台刷新能力缓存 API 小响应；官方模块说明见 [ngx_http_proxy_module](https://nginx.org/en/docs/http/ngx_http_proxy_module.html)。

### 6.4 Release Proxy 服务

已实现为单文件、零依赖的 Node 脚本（`release-proxy/quizpane-release-proxy`），
只用 Node 18+ 内置模块和全局 `fetch`，本身就是部署产物，见
[`release-proxy/README.md`](../release-proxy/README.md)。服务实现了：

1. 启动时立即轮询一次 GitHub Releases API，之后每 `POLL_INTERVAL_SECONDS`
   （默认 1800 秒）轮询一次；轮询失败只记录警告，继续用上次成功的缓存数据
   服务用户，不清空缓存、不崩溃进程；
2. 发现新版本后异步预热 `PREHEAT_ASSETS` 列出的安装包；
3. 只有完整下载且字节数、SHA-256 都与 GitHub 返回的 digest 一致后，才把文件
   标记为可用；
4. 以 systemd 运行、重启策略 `on-failure`，日志写 stdout/stderr（journald 采集）；
5. 对外健康检查 `GET /healthz`，返回 GitHub 最近同步时间、缓存容量和当前版本；即使轮询失败也返回 200（状态标记为 `degraded`），避免部署健康检查误判。

示例 systemd 运维命令：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now quizpane-release-proxy
sudo systemctl status quizpane-release-proxy
journalctl -u quizpane-release-proxy -f
```

## 7. 验收清单

1. 手机与桌面端首屏、功能轮播、下载卡片均正常；减少动态效果系统设置下不自动滚动。
2. 宣传截图来自实际导入的演示题库，且不公开原始 PDF。
3. `/api/releases/latest` 显示 tag、日期、大小、SHA-256、平台下载地址。
4. 新 Release 发布后，下一次轮询周期内（默认最长 30 分钟）官网版本信息更新；`POST /internal/refresh`（仅本机）可立即触发一次轮询用于验证。
5. 同一安装包第二次下载从服务器缓存提供；断点续传可用；缓存未完成文件不对外暴露。
6. 代理请求不能访问任何非 QuizPane Release 的 URL/文件。
7. HTTPS、限流、磁盘容量告警和 systemd 自动恢复均通过；代理服务器自身能稳定访问 GitHub。

## 8. 需要确认的决策

1. 官网域名与备案主体；
2. 是否只提供 macOS arm64 / Windows x64，还是保留 macOS Intel；
3. 宣传页面的产品语气：偏“效率工具”还是偏“公考/考试刷题”；
4. 服务器地区与带宽、是否接入对象存储/CDN（缓存量增大后建议）；
5. 下载代理是否需要保留历史版本，默认建议保留 2 个正式版；
6. 是否允许官网收集匿名下载统计（默认不收集）。
