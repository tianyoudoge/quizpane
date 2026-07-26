const BRIDGE_URL = "ws://127.0.0.1:49752/quizpane-browser/v1";
const MAC_SOURCE_PARK_LEFT = -10_000;
const MAC_SOURCE_PARK_TOP = 96;
const HEARTBEAT_MS = 20_000;
const RETRY_MS = 2_000;
const RELEASE_API_URL = "https://api.github.com/repos/tianyoudoge/quizpane/releases/latest";
const EXTENSION_ASSET = "QuizPane-course-companion.zip";
const UPDATE_ALARM = "quizpane-extension-update-check";
const UPDATE_CHECK_PERIOD_MINUTES = 360;
const UPDATE_CHECK_CACHE_MS = 60 * 60 * 1000;

let socket = null;
let heartbeat = null;
let retryTimer = null;
let boundTabId = null;
let courseState = {
  bound: false,
  videoDetected: false,
  videoState: "paused",
  courseTitle: "",
  videoCurrentTimeSeconds: -1,
  videoDurationSeconds: -1
};
let courseWindowId = null;
let originWindowId = null;
let originTabIndex = null;
let courseWindowMinimized = false;
let externalWindowBinding = null;
let externalWindowStatus = null;

const restored = chrome.storage.session.get([
  "boundTabId", "courseState", "courseWindowId", "originWindowId", "originTabIndex", "courseWindowMinimized",
  "externalWindowBinding", "externalWindowStatus"
]).then(saved => {
  boundTabId = Number.isInteger(saved.boundTabId) ? saved.boundTabId : null;
  if (saved.courseState) courseState = saved.courseState;
  courseWindowId = Number.isInteger(saved.courseWindowId) ? saved.courseWindowId : null;
  originWindowId = Number.isInteger(saved.originWindowId) ? saved.originWindowId : null;
  originTabIndex = Number.isInteger(saved.originTabIndex) ? saved.originTabIndex : null;
  courseWindowMinimized = Boolean(saved.courseWindowMinimized);
  externalWindowBinding = saved.externalWindowBinding || null;
  externalWindowStatus = saved.externalWindowStatus || null;
});

function browserName() {
  return navigator.userAgent.includes("Edg/") ? "Edge" : "Chrome";
}

function numericVersion(value) {
  const match = String(value || "").match(/^v?(\d+(?:\.\d+){0,3})$/);
  return match ? match[1].split(".").map(Number) : null;
}

function compareVersions(left, right) {
  const a = numericVersion(left);
  const b = numericVersion(right);
  if (!a || !b) return 0;
  const length = Math.max(a.length, b.length);
  for (let index = 0; index < length; index += 1) {
    const delta = (a[index] || 0) - (b[index] || 0);
    if (delta) return delta > 0 ? 1 : -1;
  }
  return 0;
}

async function storedUpdateInfo() {
  return (await chrome.storage.local.get("updateInfo")).updateInfo || null;
}

async function checkForUpdate({ force = false } = {}) {
  const cached = await storedUpdateInfo();
  if (!force && cached?.checkedAt && Date.now() - cached.checkedAt < UPDATE_CHECK_CACHE_MS) return cached;
  try {
    const response = await fetch(RELEASE_API_URL, {
      cache: "no-store",
      headers: { Accept: "application/vnd.github+json" }
    });
    if (!response.ok) throw new Error(`release-api-${response.status}`);
    const release = await response.json();
    const latestVersion = numericVersion(release.tag_name) ? release.tag_name.replace(/^v/, "") : null;
    const hasExtension = Array.isArray(release.assets) && release.assets.some(asset => asset.name === EXTENSION_ASSET);
    const currentVersion = chrome.runtime.getManifest().version;
    const updateInfo = {
      available: Boolean(latestVersion && hasExtension && compareVersions(latestVersion, currentVersion) > 0),
      latestVersion,
      currentVersion,
      releaseUrl: typeof release.html_url === "string" ? release.html_url : null,
      checkedAt: Date.now()
    };
    await chrome.storage.local.set({ updateInfo });
    return updateInfo;
  } catch {
    return cached || {
      available: false,
      currentVersion: chrome.runtime.getManifest().version,
      checkedAt: null
    };
  }
}

function scheduleUpdateChecks() {
  chrome.alarms.create(UPDATE_ALARM, { periodInMinutes: UPDATE_CHECK_PERIOD_MINUTES });
}

function message(type, payload = {}, requestId = crypto.randomUUID()) {
  return { protocolVersion: 1, type, requestId, timestamp: Date.now(), payload };
}

function send(type, payload = {}, requestId) {
  if (socket?.readyState !== WebSocket.OPEN) return false;
  socket.send(JSON.stringify(message(type, payload, requestId)));
  return true;
}

function publishStatus() {
  send("event.status_snapshot", {
    ...courseState,
    tabId: boundTabId,
    browser: browserName(),
    courseWindowMode: courseWindowId ? "popup" : "tab",
    courseWindowVisible: Boolean(courseWindowId && !courseWindowMinimized),
    externalWindowStatus
  });
}

function scheduleReconnect() {
  if (retryTimer) return;
  retryTimer = setTimeout(() => {
    retryTimer = null;
    connect();
  }, RETRY_MS);
}

function connect() {
  if (socket?.readyState === WebSocket.OPEN || socket?.readyState === WebSocket.CONNECTING) return;
  try {
    socket = new WebSocket(BRIDGE_URL);
  } catch {
    scheduleReconnect();
    return;
  }
  socket.onopen = () => {
    send("hello", { client: "quizpane-browser-extension", extensionVersion: "0.1.0", browser: browserName() });
    clearInterval(heartbeat);
    heartbeat = setInterval(() => send("ping"), HEARTBEAT_MS);
  };
  socket.onmessage = async event => {
    let incoming;
    try { incoming = JSON.parse(event.data); } catch { return; }
    if (incoming.protocolVersion !== 1) return;
    if (incoming.type === "hello_ack") {
      publishStatus();
      if (courseWindowId) {
        // 新 WebSocket 代表桌面端上一条捕获流已经随断线被释放。浏览器会话里
        // 保存的 binding 只是旧标记，必须清掉并重新 attach。
        const staleBinding = externalWindowBinding;
        const staleWasMacReplica = staleBinding?.backend === "macos-captured-replica";
        externalWindowBinding = null;
        externalWindowStatus = null;
        if (staleBinding) await restoreBindingTitle(staleBinding);
        await persistState();
        try {
          const popup = staleWasMacReplica
            ? await chrome.windows.get(courseWindowId)
            : await chrome.windows.update(courseWindowId, {
                state: "normal", focused: false
              });
          courseWindowMinimized = false;
          await persistState();
          await forwardCommand("command.enter_focus_mode");
          await forwardCommand("command.ensure_playing");
          attachExternalWindow(popup).catch(() => {});
        } catch {
          courseWindowId = null;
          courseWindowMinimized = false;
          await persistState();
        }
      }
      return;
    }
    if (incoming.type === "externalWindow.attached") {
      await handleExternalWindowAttached(incoming.payload || {});
      return;
    }
    if (!incoming.type?.startsWith("command.")) return;
    const result = await handleDesktopCommand(incoming.type, incoming.payload || {});
    send("result", result, incoming.requestId);
  };
  socket.onclose = () => {
    clearInterval(heartbeat);
    heartbeat = null;
    socket = null;
    scheduleReconnect();
  };
  socket.onerror = () => socket?.close();
}

async function handleDesktopCommand(type, payload = {}) {
  switch (type) {
    case "command.boss_hide": return bossHide();
    case "command.boss_restore": return bossRestore();
    case "command.finalize_boss_restore": return finalizeBossRestore();
    case "command.video_control": return forwardCommand(type, payload);
    case "command.show_course_window": return showCourseWindow();
    case "command.hide_course_window": return hideCourseWindow();
    case "command.return_tab": return returnTab();
    default: return forwardCommand(type);
  }
}

async function forwardCommand(type, payload = {}) {
  await restored;
  if (!boundTabId) return { success: false, error: "no-bound-course" };
  try {
    const result = await chrome.tabs.sendMessage(boundTabId, { type, ...payload });
    if (result?.videoDetected !== undefined) updateCourseState(result);
    return result || { success: false, error: "empty-content-response" };
  } catch {
    clearBinding();
    return { success: false, error: "bound-tab-unavailable" };
  }
}

function updateCourseState(next) {
  const previous = courseState;
  courseState = { ...previous, ...next, bound: Boolean(boundTabId) };
  // 当前时间每秒变化，只用于已连接桌面端的即时显示；不把它写入浏览器会话
  // 存储。其余课程状态改变才持久化，以便 service worker 重启后恢复绑定信息。
  const persistentFields = ["bound", "videoDetected", "videoState", "courseTitle", "focusMode"];
  if (persistentFields.some(field => previous[field] !== courseState[field])) persistState();
  publishStatus();
}

function persistState() {
  return chrome.storage.session.set({
    boundTabId, courseState, courseWindowId, originWindowId, originTabIndex, courseWindowMinimized,
    externalWindowBinding, externalWindowStatus
  });
}

async function restoreBindingTitle(binding = externalWindowBinding) {
  if (!binding?.bindingToken || !boundTabId) return;
  try {
    await chrome.tabs.sendMessage(boundTabId, { type: "command.restore_window_binding_title" });
  } catch {
    // 页面关闭、跳转或权限撤销时标题自然随窗口销毁，不影响后续绑定。
  }
}

function detachExternalWindow() {
  const binding = externalWindowBinding;
  externalWindowBinding = null;
  externalWindowStatus = null;
  if (binding?.sessionId) send("externalWindow.detach", { sessionId: binding.sessionId });
  restoreBindingTitle(binding);
}

function usesMacCapturedReplica() {
  return externalWindowBinding?.backend === "macos-captured-replica";
}

async function attachExternalWindow(popup = null) {
  await restored;
  if (!boundTabId || !courseWindowId || externalWindowBinding) return false;
  const sessionId = crypto.randomUUID();
  const bindingToken = `__QUIZPANE_WINDOW_${crypto.randomUUID()}__`;
  let titleResult;
  try {
    titleResult = await chrome.tabs.sendMessage(boundTabId, {
      type: "command.set_window_binding_title", bindingToken
    });
  } catch {
    return false;
  }
  if (!titleResult?.success) return false;
  let browserWindow = popup;
  try {
    if (!browserWindow?.id) browserWindow = await chrome.windows.get(courseWindowId);
  } catch {
    await restoreBindingTitle({ bindingToken });
    return false;
  }
  externalWindowBinding = { sessionId, bindingToken, pending: true };
  externalWindowStatus = { pending: true };
  await persistState();
  const sent = send("externalWindow.attach", {
    sessionId,
    bindingToken,
    chromeWindowId: courseWindowId,
    tabId: boundTabId,
    bounds: {
      left: Number(browserWindow.left) || 0,
      top: Number(browserWindow.top) || 0,
      width: Math.max(1, Number(browserWindow.width) || 640),
      height: Math.max(1, Number(browserWindow.height) || 420)
    },
    preferredFps: 30,
    contentMode: "video"
  });
  if (!sent) {
    externalWindowBinding = null;
    externalWindowStatus = { error: "未连接本机 QuizPane，无法创建置顶视频小窗" };
    await persistState();
    await restoreBindingTitle({ bindingToken });
    return false;
  }
  setTimeout(() => {
    if (externalWindowBinding?.sessionId === sessionId && externalWindowBinding.pending) {
      restoreBindingTitle(externalWindowBinding);
    }
  }, 30_000);
  return true;
}

async function handleExternalWindowAttached(payload) {
  if (!externalWindowBinding || payload.sessionId !== externalWindowBinding.sessionId) return;
  const binding = externalWindowBinding;
  externalWindowBinding = payload.success
    ? { ...binding, pending: false, backend: payload.backend || "unknown" }
    : null;
  externalWindowStatus = payload.success
    ? { backend: payload.backend || "unknown" }
    : { error: payload.error || "无法创建置顶视频小窗" };
  // macOS 的成功回执代表第一帧已经呈现在镜像中。源窗必须继续保持 normal：
  // 最小化会让 Chromium 停止持续提交窗口画面，ScreenCaptureKit 随即停在
  // 最后一帧，镜像鼠标控制也会失去可操作的目标。首帧确认后再把源窗停到
  // 屏幕外：若创建时就放到屏幕外，ScreenCaptureKit 还未锁定目标窗口，
  // Chromium 可能不会提交首帧，最终镜像也无法创建。
  if (payload.success && payload.backend === "macos-captured-replica" && courseWindowId) {
    try {
      const parked = await chrome.windows.update(courseWindowId, {
        state: "normal",
        left: MAC_SOURCE_PARK_LEFT,
        top: MAC_SOURCE_PARK_TOP,
        focused: false
      });
      console.info("[QuizPane] course source parked after first frame", {
        windowId: courseWindowId,
        left: parked.left,
        top: parked.top,
        state: parked.state
      });
    } catch (error) {
      console.warn("[QuizPane] cannot park course source", String(error));
    }
    courseWindowMinimized = false;
  }
  await persistState();
  await restoreBindingTitle(binding);
}

function clearBinding() {
  detachExternalWindow();
  boundTabId = null;
  courseState = {
    bound: false,
    videoDetected: false,
    videoState: "paused",
    courseTitle: "",
    videoCurrentTimeSeconds: -1,
    videoDurationSeconds: -1
  };
  courseWindowId = null;
  originWindowId = null;
  originTabIndex = null;
  courseWindowMinimized = false;
  persistState();
  publishStatus();
}

async function enterCourseWindow() {
  await restored;
  if (!boundTabId) return { success: false, error: "no-bound-course" };
  if (courseWindowId) return showCourseWindow();
  try {
    const tab = await chrome.tabs.get(boundTabId);
    originWindowId = tab.windowId;
    originTabIndex = tab.index;
    const popupOptions = {
      tabId: boundTabId,
      type: "popup",
      width: 640,
      height: 420,
      // 课程页只作为 QuizPane 镜像的后台来源。保持 normal 才能持续合成；
      // 首帧呈现后才会移出桌面，避免捕获初始化阶段丢帧。
      focused: false
    };
    const popup = await chrome.windows.create(popupOptions);
    courseWindowId = popup.id ?? null;
    courseWindowMinimized = false;
    await persistState();
    const focus = await forwardCommand("command.enter_focus_mode");
    const playback = await forwardCommand("command.ensure_playing");
    await attachExternalWindow(popup);
    publishStatus();
    return {
      success: Boolean(courseWindowId),
      courseWindowId,
      focusMode: Boolean(focus?.success),
      focusError: focus?.success ? undefined : focus?.error,
      playbackStarted: Boolean(playback?.success),
      playbackError: playback?.success ? undefined : playback?.error
    };
  } catch (error) {
    return { success: false, error: "cannot-create-course-window", detail: String(error) };
  }
}

async function showCourseWindow() {
  if (!courseWindowId) return enterCourseWindow();
  try {
    // macOS 镜像模式下，源窗一直保持 normal，只是由桌面端隐藏 Edge 应用；
    // 再调用 windows.update 会把大号源窗重新带回桌面。
    if (!usesMacCapturedReplica()) {
      await chrome.windows.update(courseWindowId, { state: "normal", focused: true });
    }
    courseWindowMinimized = false;
    await persistState();
    await forwardCommand("command.enter_focus_mode");
    await forwardCommand("command.ensure_playing");
    await attachExternalWindow();
    publishStatus();
    return { success: true };
  } catch {
    courseWindowId = null;
    courseWindowMinimized = false;
    await persistState();
    return enterCourseWindow();
  }
}

async function hideCourseWindow() {
  if (!courseWindowId) return { success: false, error: "course-window-not-open" };
  try {
    if (!usesMacCapturedReplica()) {
      await chrome.windows.update(courseWindowId, { state: "minimized" });
    }
    courseWindowMinimized = true;
    await persistState();
    publishStatus();
    return { success: true };
  } catch (error) {
    return { success: false, error: "cannot-hide-course-window", detail: String(error) };
  }
}

async function bossHide() {
  const playback = await forwardCommand("command.boss_hide");
  if (!courseWindowId) return playback;
  try {
    if (!usesMacCapturedReplica()) {
      await chrome.windows.update(courseWindowId, { state: "minimized" });
    }
    courseWindowMinimized = true;
    await persistState();
    publishStatus();
    return { ...playback, windowMinimized: true };
  } catch {
    courseWindowId = null;
    courseWindowMinimized = false;
    await persistState();
    publishStatus();
    return { ...playback, windowMinimized: false };
  }
}

async function bossRestore() {
  // 先把源窗恢复为 normal 并恢复播放。macOS 桌面端会等待新帧真正显示
  // 到镜像后，再发来 finalize_boss_restore；源窗随后只失去应用焦点，
  // 不再重新最小化。
  if (courseWindowId) {
    try {
      if (!usesMacCapturedReplica()) {
        await chrome.windows.update(courseWindowId, { state: "normal", focused: false });
      }
      courseWindowMinimized = false;
      await persistState();
      publishStatus();
    } catch {
      courseWindowId = null;
      courseWindowMinimized = false;
      await persistState();
    }
  }
  return forwardCommand("command.boss_restore");
}

async function finalizeBossRestore() {
  if (courseWindowId) {
    // bossRestore 已经把源窗恢复为 normal；随后镜像原生层激活了 QuizPane。
    // 此处不再触碰 Edge 窗口，确保应用激活权切换是最后一个层级动作。
    courseWindowMinimized = false;
    await persistState();
    publishStatus();
    return { success: true, windowMinimized: false, windowKeptNormal: true };
  }
  return { success: false, error: "course-window-not-open", windowMinimized: false };
}

async function returnTab() {
  await restored;
  if (!boundTabId) return { success: false, error: "no-bound-course" };
  await forwardCommand("command.exit_focus_mode");
  detachExternalWindow();
  try {
    if (originWindowId !== null) {
      await chrome.tabs.move(boundTabId, { windowId: originWindowId, index: originTabIndex ?? -1 });
    } else {
      await chrome.windows.create({ tabId: boundTabId, type: "normal", focused: true });
    }
  } catch {
    try {
      await chrome.windows.create({ tabId: boundTabId, type: "normal", focused: true });
    } catch (error) {
      return { success: false, error: "cannot-return-tab", detail: String(error) };
    }
  }
  courseWindowId = null;
  originWindowId = null;
  originTabIndex = null;
  courseWindowMinimized = false;
  await persistState();
  publishStatus();
  return { success: true };
}

chrome.runtime.onInstalled.addListener(() => {
  connect();
  scheduleUpdateChecks();
  checkForUpdate();
});
chrome.runtime.onStartup.addListener(() => {
  connect();
  scheduleUpdateChecks();
  checkForUpdate();
});
chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === UPDATE_ALARM) checkForUpdate();
});
chrome.tabs.onRemoved.addListener(tabId => { if (tabId === boundTabId) clearBinding(); });
chrome.windows.onRemoved.addListener(windowId => {
  if (windowId === courseWindowId) {
    detachExternalWindow();
    courseWindowId = null;
    courseWindowMinimized = false;
    persistState();
    publishStatus();
  }
});
chrome.tabs.onUpdated.addListener((tabId, change) => {
  if (tabId === boundTabId && change.status === "loading") {
    courseState = {
      ...courseState,
      videoDetected: false,
      videoState: "paused",
      videoCurrentTimeSeconds: -1,
      videoDurationSeconds: -1
    };
    publishStatus();
  }
  if (tabId === boundTabId && change.status === "complete" && courseWindowId) {
    installController(tabId)
      .then(() => forwardCommand("command.enter_focus_mode"))
      .catch(() => {});
  }
});

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  (async () => {
    await restored;
    if (request?.type === "content-status" && sender.tab?.id === boundTabId) {
      updateCourseState(request.payload);
      return { ok: true };
    }
    if (request?.type === "bind-current-tab") return bindCurrentTab(request.tabId, request.openWindow);
    if (request?.type === "get-popup-state") {
      return {
        connected: socket?.readyState === WebSocket.OPEN,
        boundTabId,
        courseWindowId,
        courseWindowVisible: Boolean(courseWindowId && !courseWindowMinimized),
        externalWindowStatus,
        updateInfo: await storedUpdateInfo(),
        ...courseState
      };
    }
    if (request?.type === "check-for-update") return checkForUpdate({ force: Boolean(request.force) });
    if (request?.type === "toggle-playback") return forwardCommand("command.toggle_playback");
    if (request?.type === "show-course-window") return showCourseWindow();
    if (request?.type === "return-tab") return returnTab();
    if (request?.type === "unbind") {
      if (courseWindowId) await returnTab();
      else await forwardCommand("command.exit_focus_mode");
      clearBinding();
      return { success: true };
    }
    return { success: false, error: "unsupported-request" };
  })().then(sendResponse).catch(error => sendResponse({ success: false, error: String(error) }));
  return true;
});

async function bindCurrentTab(tabId, openWindow = false) {
  if (!Number.isInteger(tabId)) return { success: false, error: "invalid-tab" };
  try {
    await installController(tabId);
    boundTabId = tabId;
    const result = await chrome.tabs.sendMessage(tabId, { type: "command.query_status" });
    updateCourseState(result || {});
    if (openWindow) {
      const windowResult = await enterCourseWindow();
      if (!windowResult.success) return windowResult;
    }
    return { success: true, ...courseState };
  } catch (error) {
    return { success: false, error: "cannot-inject", detail: String(error) };
  }
}

function installController(tabId) {
  // 许多网课把播放器放在同源 iframe；只注入顶层页面时，右键菜单能看到浏览器
  // 自带画中画，但扩展无法找到实际 <video>。allFrames 会覆盖当前已有权限的
  // 所有 frame；跨域 frame 仍必须由用户明确授予对应网站权限，不能绕过。
  return chrome.scripting.executeScript({
    target: { tabId, allFrames: true },
    files: ["src/content-script.js"]
  });
}

connect();
