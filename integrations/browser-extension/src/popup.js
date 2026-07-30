const connection = document.querySelector("#connection");
const course = document.querySelector("#course");
const bindButton = document.querySelector("#bind");
const toggleButton = document.querySelector("#toggle");
const showWindowButton = document.querySelector("#show-window");
const returnTabButton = document.querySelector("#return-tab");
const unbindButton = document.querySelector("#unbind");
const update = document.querySelector("#update");
const updateTitle = document.querySelector("#update-title");
const updateDetail = document.querySelector("#update-detail");
const openUpdateButton = document.querySelector("#open-update");
const checkUpdateButton = document.querySelector("#check-update");
const error = document.querySelector("#error");

function send(type, payload = {}) {
  return chrome.runtime.sendMessage({ type, ...payload });
}

async function activeTab() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  return tab;
}

function originPattern(url) {
  const parsed = new URL(url);
  return `${parsed.protocol}//${parsed.hostname}/*`;
}

function formatTime(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return "--:--";
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const remainder = Math.floor(seconds % 60);
  const clock = `${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}`;
  return hours ? `${hours}:${clock}` : clock;
}

function render(state) {
  connection.textContent = state.connected ? "已连接本机 QuizPane" : "QuizPane 未启动或尚未连接";
  const updateInfo = state.updateInfo;
  update.hidden = !updateInfo?.available;
  if (updateInfo?.available) {
    updateTitle.textContent = `发现新版 v${updateInfo.latestVersion}`;
    updateDetail.textContent = `当前为 v${updateInfo.currentVersion}。下载新版后，请在扩展管理页点击“刷新”。`;
    openUpdateButton.dataset.url = updateInfo.downloadPageUrl || "";
  }
  if (!state.bound) {
    course.textContent = "尚未绑定课程页面";
    bindButton.hidden = false;
    toggleButton.hidden = true;
    showWindowButton.hidden = true;
    returnTabButton.hidden = true;
    unbindButton.hidden = true;
    return;
  }
  const video = state.videoDetected ? (state.videoState === "playing" ? "正在播放" : "已暂停") : "未检测到视频";
  course.textContent = `${state.courseTitle || "当前课程"} · ${video} · ${formatTime(state.videoCurrentTimeSeconds)} / ${formatTime(state.videoDurationSeconds)}`;
  if (state.externalWindowStatus?.pending) {
    error.textContent = "正在创建 QuizPane 置顶视频小窗…";
  } else if (state.externalWindowStatus?.error) {
    error.textContent = "置顶视频小窗未启动：" + state.externalWindowStatus.error;
  }
  bindButton.hidden = true;
  toggleButton.hidden = false;
  showWindowButton.hidden = false;
  returnTabButton.hidden = !state.courseWindowId;
  unbindButton.hidden = false;
}

async function refresh() {
  try {
    await send("check-for-update");
    render(await send("get-popup-state"));
  }
  catch { render({ connected: false, bound: false }); }
}

openUpdateButton.addEventListener("click", async () => {
  const url = openUpdateButton.dataset.url;
  if (url) await chrome.tabs.create({ url });
});

checkUpdateButton.addEventListener("click", async () => {
  checkUpdateButton.disabled = true;
  checkUpdateButton.textContent = "正在检查…";
  try {
    await send("check-for-update", { force: true });
    await refresh();
    if (update.hidden) error.textContent = "已是最新版本。";
  } finally {
    checkUpdateButton.disabled = false;
    checkUpdateButton.textContent = "检查扩展更新";
  }
});

bindButton.addEventListener("click", async () => {
  error.textContent = "";
  const tab = await activeTab();
  if (!tab?.id || !/^https?:/.test(tab.url || "")) {
    error.textContent = "请在普通网页的课程播放页中使用。";
    return;
  }
  const origin = originPattern(tab.url);
  const granted = await chrome.permissions.request({ origins: [origin] });
  if (!granted) {
    error.textContent = "需要当前课程站点的权限才能控制视频。";
    return;
  }
  const result = await send("bind-current-tab", { tabId: tab.id, openWindow: true });
  if (!result.success) error.textContent = "绑定失败：" + (result.error || "未知错误");
  await refresh();
});

toggleButton.addEventListener("click", async () => {
  const result = await send("toggle-playback");
  if (!result.success) error.textContent = "控制失败：" + (result.error || "未知错误");
  await refresh();
});

showWindowButton.addEventListener("click", async () => {
  const result = await send("show-course-window");
  if (!result.success) error.textContent = "无法显示小窗：" + (result.error || "未知错误");
  await refresh();
});

returnTabButton.addEventListener("click", async () => {
  const result = await send("return-tab");
  if (!result.success) error.textContent = "无法放回原浏览器：" + (result.error || "未知错误");
  await refresh();
});

unbindButton.addEventListener("click", async () => { await send("unbind"); await refresh(); });
refresh();
