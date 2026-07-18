# QuizPane 官网静态页

无框架的静态站：纯 HTML + CSS + 一个不到 300 行的原生 JS 文件。内容集中在
`src/content.json`，改文案不需要碰 HTML/JS。

## 目录结构

```text
website/
  src/                    # 静态页源码，可直接用浏览器或任意静态服务器打开调试
    index.html
    main.css
    main.js
    content.json          # 页面文案与下载平台配置（唯一数据源）
    assets/
      icons/               # SVG 图标（logo、favicon、平台图标）
      screenshots/         # 官网演示题库的产品截图与界面演示图
  scripts/
    build-site.mjs         # 构建脚本，见下文
  dist/                    # 构建产物，不提交到 Git（.gitignore 已排除）
```

## 本地预览

不需要安装依赖，任何静态文件服务器都可以：

```bash
cd website/src
python3 -m http.server 8080
# 浏览器打开 http://127.0.0.1:8080/index.html
```

页面会向 `/api/releases/latest` 发起请求获取最新版本信息；本地预览时该接口
通常不存在，下载区会回退显示"正在获取最新版本…"并把下载按钮指向
GitHub Release 页面，这是预期行为，不是 bug。

页面也支持部署在域名子路径（例如 `https://xutianyou.cc/quizpane/`）：资源、
Release API 和下载链接都会以当前页面目录为基准。仓库的 Nginx 模板已包含
`/quizpane/` 路由与对应代理；访问时务必保留末尾 `/`。

## 构建

```bash
node website/scripts/build-site.mjs
```

零依赖 Node 脚本（Node 18+ 即可），做的事情：

1. 把 `src/` 整个拷贝到 `dist/`；
2. 对 `main.css`、`main.js` 按内容生成哈希后缀（如 `main.170978eb8b.css`），
   并同步改写 `index.html` 里的引用，方便设置长缓存（Nginx 模板对 `/assets/`
   之外的 `/` 走 `no-cache`，但更新静态站时新旧文件名不同可以避免旧资源被
   浏览器缓存卡住）；
3. 不做打包、压缩、Sass/TS 编译——保持源码即产物、易于审查。

产物在 `website/dist/`，其中 `index.html` 是 `deploy/scripts/install-artifacts.sh
--site-dist` 要求的目录标志文件。

## 替换占位内容

- **文案与下载平台**：改 `src/content.json`。`downloads.platforms[].asset`
  必须与 GitHub Release 里实际的 asset 文件名一致（当前对应 README 里列出的
  `QuizPane-macos-arm64.dmg`、`QuizPane-macos-x86_64.dmg`、
  `QuizPane-windows-x64.exe`、`QuizPane-linux-x86_64.deb`）。
- **截图**：`src/assets/screenshots/` 已放入北京卷官网演示题库的导入截图，以及
  资料换行、公式选项和小窗答题演示图；替换文件后同步修改 `content.json` 的
  `shot` 路径和 `index.html` 的 hero 图路径。发布前请确认试卷内容的展示与
  传播授权。
- **图标**：导航、联系区和右下角赞赏浮窗使用产品图标
  `src/assets/icons/quizpane-app-icon.png`。

## 一键发布到服务器

仓库已经提供一键脚本 [`deploy/scripts/build-and-deploy-site.sh`](../deploy/scripts/build-and-deploy-site.sh)。
它会先构建 `website/dist`，再调用原子发布脚本切换站点版本；无需手动复制文件。

服务器需要先完成一次 `deploy/scripts/bootstrap-ubuntu.sh` 初始化（见
[`deploy/README.md`](../deploy/README.md)）。之后按实际部署位置选择一种方式：

### 在目标服务器上发布

```bash
./deploy/scripts/build-and-deploy-site.sh
```

可选地用 `--version site-20260718` 标记本次发布，或用 `--no-restart` 仅安装
文件、暂不重载 Nginx。

### 从本机或 CI 远程发布

远程服务器应已有本仓库检出目录，部署账号需能免密执行
`install-artifacts.sh` 的 sudo。执行一次即可构建、本地同步并在远端原子切换：

```bash
./deploy/scripts/build-and-deploy-site.sh \
  --remote-host deploy@quizpane.example.com \
  --remote-repo /srv/quizpane-src \
  --version site-20260718
```

发布完成后检查首页与下载元数据：

```bash
curl -I https://quizpane.example.com/
curl -s https://quizpane.example.com/api/releases/latest
```

脚本和网站构建均不上传题库、PDF 或用户数据。完整的 Ubuntu 初始化、HTTPS、
下载代理和故障排查见 [`deploy/README.md`](../deploy/README.md)。

## 依赖的后端接口

页面本身不依赖 Release 代理即可展示（会优雅降级），但要让下载区显示真实版本号
和大小，需要 `release-proxy` 服务提供 `/api/releases/latest` 与
`/download/:tag/:asset`，已实现，见
[`release-proxy/README.md`](../release-proxy/README.md)。
