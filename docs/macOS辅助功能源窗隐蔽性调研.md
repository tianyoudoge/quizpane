# macOS 辅助功能控制源窗的隐蔽性调研

> 2026-07-29；只考察公开 Apple / Chromium 接口。这里的“源窗”是 Chrome/Edge 的普通 popup，“镜像”是 QuizPane 自己的 `NSPanel` + ScreenCaptureKit 渲染。

## 结论

**可以谨慎尝试的方案只有一个：** 通过辅助功能 API 把源窗移动到**仍在某块屏幕内**的正常位置，并用 QuizPane 自己的、不透明且置顶的 panel 完全盖住它。它既不最小化，也不隐藏源应用；因而是唯一有机会达到“用户看不见、源窗仍正常合成”的做法。

但这是一个待验证的运行时能力，不应写成对 Chrome/Edge 的保证：先检查目标 `AXWindow` 的 `AXPosition` 是否可写，再写入、回读，并同时监测 ScreenCaptureKit 帧流。Apple 明确要求调用方用 `AXUIElementIsAttributeSettable` 判定某个**具体元素**的属性能否修改；失败可能是属性不支持、目标无响应或 API 未实现。[Apple: AXUIElementIsAttributeSettable](https://developer.apple.com/documentation/applicationservices/1459972-axuielementisattributesettable?changes=_2_6)

不应采用“挪到屏幕外”“最小化”“隐藏 Chrome 应用”“缩到近零尺寸”来换取隐蔽性。它们不是“正常但不可见”的状态，并且现有项目真机日志已显示：源窗移出屏幕后，ScreenCaptureKit 停止送帧。这是本项目的实测证据，不是 Apple 文档承诺。

## 辅助功能实际能做什么

| 操作 | 公开 API / 文档事实 | 对本需求的判断 |
| --- | --- | --- |
| 移动窗口 | `AXPosition` 是全局屏幕坐标；可由用户直接拖动的窗口“应当”提供可写 position。`AXSize` 对可缩放窗口也同理。两者是否真可写需逐个元素检查。Apple SDK 的 `AXAttributeConstants.h`，以及 [AXUIElementIsAttributeSettable](https://developer.apple.com/documentation/applicationservices/1459972-axuielementisattributesettable?changes=_2_6)。 | **可做 POC。** 只改变位置，保持原窗口尺寸与 normal 状态。 |
| 调整大小 | 同上；若 Chrome 的 AX window 将 `AXSize` 标为可写，AX 可请求调整大小。 | **不推荐作为隐藏手段。** 缩小会改变 Chrome 的渲染表面，可能降画质或触发播放器布局变化；“不休眠”也不能由文档推出。 |
| 最小化 | `AXMinimized` 是可写窗口属性。AppKit 明确说明最小化窗口会从 screen list 移除并在 Dock 以替身出现。[Apple: `miniaturized`](https://developer.apple.com/documentation/appkit/nswindow/isminiaturized?language=objc) | **不要用。** 它不是保持 normal；项目实测 SCK 会卡在最后一帧。 |
| 隐藏应用 | `AXHidden` 仅是应用级属性常量；Apple 的公开文档没有给出“隐藏某一扇窗但仍 normal 且持续供 ScreenCaptureKit 捕获”的合同。 | **不要依赖。** 它会影响整个浏览器应用，而非仅课程 popup，且捕获行为没有保证。 |
| 改到屏幕外 | `AXPosition` 的坐标本身可以描述屏外位置；但其可写性仍取决于 Chrome/Edge。 | **不要做。** 项目真机实测的 `CGWindowIsOnscreen=false` 会使当前 SCK 流不再出帧。 |
| 改窗口层级 / 降到所有应用背后 | AX 有 `AXRaise`，即向前抬升；没有公开的 AX “send to back”、跨应用持续 bottom-most 或 alpha/opacity 属性。Apple 的公开 `NSWindow.level` 是窗口所属应用的属性，不授权外部进程持久修改。 | **做不到。** 不能靠 AX 把 Chrome popup 永久压在所有窗口最底下。 |

## 为什么“盖住但不最小化”值得验证

ScreenCaptureKit 有 `SCContentFilter(desktopIndependentWindow:)`，官方定义是只捕获指定窗口；这是捕获**窗口内容**而不是显示器像素的入口。[Apple: `SCContentFilter`](https://developer.apple.com/documentation/screencapturekit/sccontentfilter?changes=_8&language=objc) [Apple: `init(desktopIndependentWindow:)`](https://developer.apple.com/documentation/screencapturekit/sccontentfilter/init%28desktopindependentwindow%3A%29?changes=latest_minor)

Apple 没有在该 API 合同中保证“被别的窗口完全遮挡、移屏、隐藏或最小化时仍持续交付帧”。因此不能把“desktop independent”解读为“任意不可见状态仍能直播”。

项目已有同机诊断的实测结论可用作 POC 的先验：只要源窗的 `CGWindowIsOnscreen=true`，即使被其它窗口覆盖，当前 SCK 后端持续有帧；移出屏幕、最小化、0×0 则立即无帧。详见 [网课视频置顶小窗方案](网课视频置顶小窗方案.md#32-macos-当前-screencapturekit-镜像路线的死结)。这支持测试“保持完整尺寸、放在屏内、由自家 panel 覆盖”，但不代替不同 macOS、Chrome、Edge 版本的实测。

## 推荐的最小验证与验收门槛

1. 仅在用户主动启用“隐藏源窗（实验）”时调用 `AXIsProcessTrustedWithOptions`；它会在未获授权时提示用户，并返回调用方是否为受信任的辅助功能客户端。[Apple: `AXIsProcessTrustedWithOptions`](https://developer.apple.com/documentation/applicationservices/1459186-axisprocesstrustedwithoptions)
2. 通过已知 Chrome/Edge PID 枚举 `AXWindows`，以现有绑定标题令牌精确匹配课程 popup；不要按前台窗口猜测。
3. 对匹配到的 window 分别调用 `AXUIElementIsAttributeSettable(..., kAXPositionAttribute, ...)` 和读取当前 `AXSize`。任一失败即无副作用降级，并记录 AXError。
4. 保持现有尺寸、`AXMinimized=false`，把源窗放到一个完整位于任一显示器可见 frame 内的矩形；随后让自家的 opaque `NSPanel` 覆盖**整个浏览器窗口外框**，不能只盖视频内容，否则标题栏仍可见。
5. 连续 10 秒验证三个独立信号：AX 回读的位置、`CGWindowIsOnscreen=true`、SCK 新帧 PTS/帧数持续递增；播放、拖动进度、老板键 hide/restore 后各复测一次。任何一项失败都恢复源窗原坐标并关闭实验开关。

## 已知边界

- Chrome/Edge 是否把其原生 `AXWindow` 的 `AXPosition` 标为可写，并非 Apple 可以替它保证；Chromium 的 macOS 辅助功能实现说明浏览器会向辅助技术暴露原生可访问对象，但不构成“该 popup 可移动”的承诺。[Chromium accessibility overview](https://chromium.googlesource.com/chromium/src/%2B/main/docs/accessibility/overview.md)
- AX 控制不等于跨进程置顶。调用 `AXRaise` 反而会把窗口带到前面，可能抢用户注意力；它不适合作为隐藏机制。
- 访问 AX 需要单独的“辅助功能”TCC 授权，和现有的“屏幕录制”授权不同；功能必须可关闭，授权失败不得影响普通镜像路径。
- 本调研没有发现公开 API 可让第三方把另一个应用的单一窗口设为 0 alpha、永久置底，或“不可见但 normal”。若产品要求的是绝对不可见且无额外覆盖 panel，公开 API 下没有可靠实现。
