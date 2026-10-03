# Windows AI 设置与字体实际 UI 回归（2026-10-03）

测试分支：`codex/windows-ai-settings-regression`。发布继续暂停，测试工作流只生成包。

环境：Windows App 的 SOHU 会话 → Windows 11 宿主 `XUTIANYOU-SOHU-` → Hyper-V `win764`（Win7 x64）。Win7 原安装路径为 `C:\Users\xty-64\Desktop\QuizPane\QuizPane.exe`。UI 用真实桌面鼠标操作，VM 查看器以 100% 缩放检查文字，50% 仅用于操作完整桌面；未改系统 DPI。

本轮使用独立 `Win7 AI UI` 题库，三道合成单选题及已有 AI 缓存。没有触发真实 AI API 请求，也没有编辑原题库。全局配置暂改测试模型后恢复默认；供应商与 API Key 不改。

| 构建 | 结果 |
|---|---|
| `8abaf9851ed203bf32574a525fc17edb8f15b8b7` | 首轮实际 UI，发现字体刷新回退及浅色文字对比度问题 |
| `ad64cba86591645fabb7342529bcec670b28b889` | 保留刷新时的字族 |
| `8ae49e29cee514a947214ba8163c14e650572537` | 移除解析题干、选项与原解析容器的固定浅灰内联颜色 |
| `13cd72c5084e288f6a5f85f43bb0234775685a9d` | Windows 离屏像素检查加载 Arial；Win7 浅色解析、复杂公式与配置跨重启保留已复验通过 |

[13cd72c 测试构建](https://github.com/tianyoudoge/quizpane/actions/runs/37097588299)：Win7 x64/x86 构建、CTest、包检查通过。Win10/11 构建、CTest、包检查也通过。

## 首轮实机结果

`8abaf98` 包 ZIP SHA256：`883d5c93bea2afea79616cd88046cd50c47fd457a9a782f07df7056b0fd4e397`。两段复制后均逐项验证 86 个文件哈希，再正常退出旧程序、备份用户数据与设置、整目录替换。部署 EXE SHA256：`c1b157c3f04f729084a90ade4c3dee1f8cc4ddde74169096cc41be33e21a489d`。旧目录保留为 `QuizPane.backup-20261003-122856`，用户数据备份留在 guest，不上传设置内容。

| 检查项 | 实际观察 | 结果／证据 |
|---|---|---|
| 主菜单全局设置 | 未进入解析即可打开 AI 配置，高级设置显示服务地址和模型 | PASS，05-global-ai-settings.png |
| 配置共享 | 全局保存 `ui-regression-model` 后，解析页齿轮显示同一模型 | PASS，06-shared-model.png |
| 取消编辑 | 修改模型后取消，再打开仍为先前配置 | PASS |
| 托盘入口 | 托盘菜单直接打开同一配置弹窗 | PASS |
| 原数据保留 | 新包启动仍显示旧练习／草稿恢复提示 | PASS，04-deployed.png |
| 答题流程 | 三题选 A、切题、提交，显示 3/3 正确 | PASS |
| Markdown | 粗体、段落、两项列表、行内代码与 cpp 围栏代码实际可读 | PASS，08-large-markdown.png |
| 公式 | 行内上下标、希腊字母；嵌套分数、根号、求和及括号未见裁切，未知命令保留 | PASS，09-large-formulas.png |
| 小／中／大字号 | 窗口正文和公式随模式变化，本 fixture 未见横向裁切 | PASS；仅覆盖这三道合成题 |
| 字体刷新 | 调背景可见度后正文出现明显字形回退 | FAIL，07-small-font-refresh.png；已在 13cd72c 包复验通过 |
| 浅色主题 | AI 面板文字正常，题干与选项太淡 | FAIL，10-light-contrast-bug.png；已在 13cd72c 包复验通过 |

首轮截图目录：[qa/8abaf98-win7](qa/8abaf98-win7/)。

本地验证：全局设置修改后 37/37 CTest 通过；字体和颜色修改后相关三项 CTest 通过；离屏字体修正后 solution_page_controller_test 通过。CI 通过不替代桌面 UI 结论。

## 修复复验

`13cd72c` Win7 ZIP SHA256：`71f04e8031e8ab1574625eab5ec34143c78805342ed0baeb941f4616ef518ca3`；EXE SHA256：`bb9639194954f0938390e97c96502e94b2249ee1879a668254e4cda7fc8e476a`。两段 86 文件检查通过后整包替换。重启前保存测试模型 `qa`，重启后全局配置仍显示 `qa`，随后恢复默认。

实际浅色解析页题干、选项、原解析已清晰；100% 缩放下 Markdown、代码和复杂公式正常。背景可见度从 100% 改回 69%、切换深色与中号后，正文字形保持一致，未再出现首轮刷新回退。截图：[qa/13cd72c-win7](qa/13cd72c-win7/)。

Win11 使用同一提交的 Qt6 包，独立目录启动，没有替换宿主原包或关闭其它实例。ZIP SHA256：`a5394afdf5a06a6ed14cc093753800ac0b1a064ffb930e6530b8e04accb24c49`；刷题 EXE SHA256：`f03084c17261ffc7c92d2fba98f3c74c528a43f2d99d15a1c995fedeb4216bf9`。38 文件哈希核对通过。

Win11 实际通过：主菜单 AI 设置入口、高级地址与模型布局、取消关闭、三题切换作答与 3/3 提交结果、Markdown/代码滚动、复杂公式与未知命令保留。界面高度受限时正文可滚动，底部操作按钮保留。只查看配置，没有更改模型或 API Key。截图：[qa/13cd72c-win11](qa/13cd72c-win11/)。Win11 浅色与字号切换尚未覆盖。

继续实测发现练习页题干也固定浅色，已在 `9fe32d46775d7162f7b803fcfc8ddbcfc90e16c4` 移除内联颜色。[9fe32d4 测试构建](https://github.com/tianyoudoge/quizpane/actions/runs/37098480112) Win7 x64/x86 与 Win10/11 全部通过，发布任务跳过。练习页已用 Win7 原生 100% 视图复验：浅色题干及选项使用深色文字，作答、翻题与 3/3 交卷正常。截图见 `qa/9fe32d4-win7/03-light-practice-native.png`、`04-light-practice-opaque-icons-before.png`。

## 追加问题：浅色图标

9fe32d4 的 Win7 浅色 100% 背景实机发现菜单、翻题、提交图标过淡。已将固定浅灰像素图改为按主题动态绘制的 QIconEngine，禁用态降低透明度；同一图标深浅切换和禁用态像素测试通过，主程序构建通过。修复提交 `e1384b3791c8b38eebd039114dfde1a6f01a1155` 的 [Windows 测试构建](https://github.com/tianyoudoge/quizpane/actions/runs/37106433326) Win7 x64/x86、Win10/11 构建、测试与包检查全部通过，Release 跳过；本地相关三项 CTest 通过。新包 Win7 实机复验通过，详细结果如下。


`e1384b3` Win7 ZIP SHA256：`2bd05dde93025d2fa2e0382dfccb5235fdd075c2484763a6ff14d2217022321c`；部署 EXE SHA256：`e2be584978b87e7b650388293c7aa6b05cfa35dd87aaeb0bf8a15ae818457e08`。宿主与 guest 两段 86 文件检查通过，正常退出旧主程序、备份数据与设置后整包替换；启动仍显示原题库与草稿恢复提示，选择稍后处理。然后载入独立合成题库 `Win7 AI UI e1384b3`。

| 检查项 | 步骤与实际结果 | 结果／证据 |
|---|---|---|
| 浅色练习图标 | 中号、100% 背景、VM 100% 原生视图；菜单、下一题、题目列表、提交显示深色线条，上一题禁用态较淡 | PASS，04-light-practice-native-icons.png |
| 操作与提交 | 三题选择 A，通过下一题按钮翻题，再提交并确认，显示 3/3 正确 | PASS，05-light-solution-native-icons.png |
| 浅色解析 | 题干、选项、原解析、缓存 AI Markdown 中文和行内公式清晰；底部设置与列表图标清晰 | PASS，05-light-solution-native-icons.png |
| 同一窗口主题刷新 | 托盘将浅色切换为深色，不重启；已有菜单、导航、设置、列表图标更新为浅色线条，正文正常 | PASS，06-dark-theme-icons-refreshed.png |
| 设置恢复 | 深色、中号；背景可见度从 100% 恢复至 69%，字形保持一致 | PASS，07-restored-dark-medium-opacity69.png |
| 模型恢复读回 | 再打开解析设置，高级栏显示默认 `deepseek-chat` 与 `https://api.deepseek.com/v1`，取消关闭 | PASS，08-restored-model-default.png |

截图目录：[qa/e1384b3-win7](qa/e1384b3-win7/)。Win7 原安装保留此修复包。最新图标修复未做 Win11 实机复验，Win11 实机结论仍限于上述 `13cd72c` 深色冒烟测试。AI 验证使用已有缓存，没有验证真实服务请求。

master 推送、打标签及小版本发布继续暂停；本轮仅推送回测分支。
