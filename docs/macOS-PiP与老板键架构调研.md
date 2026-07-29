# macOS：无可见源窗、稳定视频与老板键的架构边界

> 2026-07-29。仅依据 Chrome 官方文档和 Web 规范；不把未验证的浏览器行为写成保证。

## 结论先行

**没有一条公开的 Chrome 扩展 / Web PiP 路线，能同时保证：**

1. 用户看不到源页面；
2. 视频由浏览器稳定渲染；
3. QuizPane 的原生全局老板键可以任意时刻“隐藏 → 自动恢复”浮窗，且恢复时无需用户再点网页。

Document PiP 和普通视频 PiP 可以消灭当前 ScreenCaptureKit 的“镜像依赖可见源窗”问题；PiP 本身由 Chrome 渲染，原页面只需继续存活。但它们不能满足第 3 条：**首次进入或关闭后重新进入 PiP 均需要该页面的新 transient user activation（临时用户手势）**。来自桌面程序的 WebSocket / Native Messaging 命令不是页面里的点击手势，因此不能可靠地让 PiP 在老板键恢复时重新打开。

老板键可以可靠地执行“关闭 PiP + 暂停视频”，但随后的“自动重新打开 PiP”不在公开 API 保证内。把 PiP 关闭当作“隐藏”会因此造成不可接受的恢复断点。

## 路线对比

| 路线 | 源页面可不作为独立可见窗口 | 视频稳定性 | 桌面老板键隐藏 | 桌面老板键自动恢复 | 结论 |
| --- | --- | --- | --- | --- | --- |
| 现有 SCK → 原生 `NSPanel` | 否；当前实测中源窗离屏会断帧 | 受源窗可见性影响 | 可以，`NSPanel` 属于 QuizPane | 可以 | 交互权在桌面端，但做不到完全隐藏源窗且保持可靠帧流。 |
| HTML Video PiP | 可以；源文档必须继续存活 | 由 Chrome 直接渲染 PiP | 可调用 `exitPictureInPicture()` 关闭 | **不可靠**；重新 `requestPictureInPicture()` 需页面用户手势 | 适合作为用户主动开启的播放器，不是老板键替代品。 |
| Document PiP | 可以；源文档必须继续存活 | 由 Chrome 直接渲染、任意 HTML 可放入窗口 | 可调用 PiP `window.close()` | **不可靠**；`requestWindow()` 需页面用户手势 | 控件表现最好，但同样不能无感恢复。 |
| `chrome.tabCapture` + 扩展页面 / offscreen document | 可以不创建独立源 popup | 捕获的是当前标签页的合成媒体流，不是独立的原视频解码通路 | 扩展可停止流 | 没有通向 QuizPane 原生 panel 的公开媒体流交接；若再放进 PiP，仍受 PiP 用户手势限制 | 不能作为当前问题的替代后端。 |

## 普通 HTML Video PiP

`HTMLVideoElement.requestPictureInPicture()` 只能在具有 transient user activation 时成功；规范要求否则以 `NotAllowedError` 拒绝并消费该 activation。Chrome 也明确建议直接从点击处理函数调用，而不要从 `video.play()` 的 Promise 后调用。[W3C Picture-in-Picture](https://w3c.github.io/picture-in-picture/#request-picture-in-picture) · [Chrome guide](https://developer.chrome.com/blog/watch-video-using-picture-in-picture/)

进入后，PiP 窗口在源 document 的 `visibilityState` 为 `hidden` 时仍必须可见，故它确实可避免“源页面不可见就没画面”的 SCK 问题；但源 document 不能被销毁。[W3C PiP visibility](https://w3c.github.io/picture-in-picture/#picture-in-picture)

退出可由 `document.exitPictureInPicture()` 以脚本方式执行，无用户手势要求；随后会触发 `leavepictureinpicture`。但是下一次进入又回到上面的手势要求。[W3C PiP exit](https://w3c.github.io/picture-in-picture/#exit-picture-in-picture)

**硬结论：** 它能负责“播放表面”，不能单独负责“原生老板键的隐藏后自动恢复”。Chrome / 操作系统可能把 PiP 限制为一个窗口；新的请求可关闭、拒绝或替代已有 PiP，具体由实现决定。[W3C PiP one-window behavior](https://w3c.github.io/picture-in-picture/#one-picture-in-picture-window)

## Document Picture-in-Picture

Document PiP 是一个由 Chrome 保持置顶的同源空白 document，可搬入自定义播放器 DOM 与进度条；它不会超出打开它的页面、不可导航，而且网站不能设置其屏幕位置。[Chrome Document PiP](https://developer.chrome.com/docs/web-platform/document-picture-in-picture/) · [WICG specification](https://wicg.github.io/document-picture-in-picture/#introduction)

`documentPictureInPicture.requestWindow()` 同样要求 transient user activation；无手势会抛 `NotAllowedError`。`pipWindow.close()` 可以被脚本调用，关闭时触发 `pagehide`。因此“老板键隐藏”可做成关闭 PiP；“老板键恢复”无法由 QuizPane 命令可靠地 reopen。[WICG request algorithm](https://wicg.github.io/document-picture-in-picture/#request-window) · [WICG close example](https://wicg.github.io/document-picture-in-picture/#exiting-pip)

它不是桌面原生窗口 API：网站不能通过 `moveTo` / `moveBy` 改 PiP 位置；调整大小也需要 PiP 窗自身的用户手势。它天生置顶，但无法纳入 QuizPane 对 `NSPanel` 的同一套精确层级、显示或隐藏控制。[WICG positioning](https://wicg.github.io/document-picture-in-picture/#moving-the-pip-window) · [Chrome resize/focus constraints](https://developer.chrome.com/docs/web-platform/document-picture-in-picture/)

**硬结论：** Document PiP 是“用户显式启用的、更稳定的课程小窗”的可行 POC；不是当前严格老板键语义的替代。

## 扩展注入、跨域与权限边界

PiP API 是页面 DOM API，而不是 `chrome.*` 扩展 API。MV3 service worker 没有 DOM；须由 content script 在实际持有 `<video>` 的 document 中调用，service worker 只能转发消息。content script 虽能读写该 document 的 DOM，但其 JavaScript 在 isolated world；不能直接访问页面脚本变量。[Chrome content scripts](https://developer.chrome.com/docs/extensions/develop/concepts/content-scripts)

若播放器在 iframe，Document PiP API 只暴露给 top-level traversable，不能直接从 iframe document 请求；要改造为 top-level 页面创建 PiP 并嵌入 iframe，或站点自身提供相应结构。普通视频 PiP 则针对实际 `<video>` 所在的 document，仍受其 Permissions Policy、站点设置的 `disablePictureInPicture` 与媒体 ready state 限制。[WICG iframe boundary](https://wicg.github.io/document-picture-in-picture/#iframes) · [W3C video PiP conditions](https://w3c.github.io/picture-in-picture/#request-picture-in-picture)

扩展还需取得对应页面 / iframe URL 的 host permissions 才能注入。多加或变更 host patterns 会触发用户权限警告；这不是绕过页面跨域安全边界的授权。[Chrome host permissions](https://developer.chrome.com/docs/extensions/develop/concepts/declare-permissions) · [Chrome injection requirements](https://developer.chrome.com/docs/extensions/develop/concepts/content-scripts)

## `chrome.tabCapture` 不是替代品

`chrome.tabCapture` 捕获的是**当前已激活的标签页**的合成 video/audio `MediaStream`；开始捕获必须在用户调用扩展后发生，且需要 `tabCapture` 权限。拿到流后，原标签的音频默认不再播放，扩展必须自行接回 AudioContext 才能恢复声音。[Chrome tabCapture](https://developer.chrome.com/docs/extensions/reference/api/tabCapture)

Chrome 116 起，service worker 创建的 stream ID 可以由同源 extension offscreen document 消费；这让扩展内处理媒体流成为可能，但文档没有提供把该 `MediaStream` 零拷贝交给 QuizPane 原生 `NSPanel` 的 API。若把流再展示在 HTML / Document PiP 里，重新显示仍要接受 PiP 的用户手势规则。[Chrome stream-ID restrictions](https://developer.chrome.com/docs/extensions/reference/api/tabCapture#method-getMediaStreamId) · [Chrome offscreen documents](https://developer.chrome.com/docs/extensions/reference/api/offscreen)

**硬结论：** tabCapture 不能解决“桌面端可无感老板键恢复”的控制权问题，反而引入激活标签、音频回放与媒体流生命周期约束。

## 对产品决策的含义

若“老板键隐藏、恢复时不要求鼠标点击、且恢复后仍由 QuizPane 精确置顶”不可放宽，必须保留由 QuizPane 所属进程管理的原生窗口；再单独解决它的稳定视频源。这不是 PiP API 能取代的职责。

若可以把老板键降级为“关闭 PiP；恢复时由用户点击一次课程页面 / 扩展按钮重新开启”，Document PiP 是最有价值的独立验证路线：没有 SCK 镜像，也不需要屏幕录制授权；但必须逐站验证页面 DOM、iframe 架构、Permissions Policy 与 Chrome / Edge 版本。
