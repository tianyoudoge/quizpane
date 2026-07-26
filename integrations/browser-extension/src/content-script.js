(() => {
  if (globalThis.__quizpaneCourseController) return;
  globalThis.__quizpaneCourseController = true;

  let bossWasPlaying = false;
  let focusState = null;
  let lastReportedSnapshot = "";
  let pipButton = null;
  let windowBindingOriginalTitle = null;

  function visibleArea(video) {
    const rect = video.getBoundingClientRect();
    const width = Math.max(0, Math.min(rect.right, innerWidth) - Math.max(rect.left, 0));
    const height = Math.max(0, Math.min(rect.bottom, innerHeight) - Math.max(rect.top, 0));
    return width * height;
  }

  function findMainVideo() {
    return [...document.querySelectorAll("video")]
      .filter(video => video.readyState > 0)
      .sort((left, right) => {
        const leftPlaying = !left.paused && !left.ended ? 1 : 0;
        const rightPlaying = !right.paused && !right.ended ? 1 : 0;
        if (leftPlaying !== rightPlaying) return rightPlaying - leftPlaying;
        const areaDifference = visibleArea(right) - visibleArea(left);
        return areaDifference || (right.duration || 0) - (left.duration || 0);
      })[0] || null;
  }

  function findMainFrame() {
    return [...document.querySelectorAll("iframe")]
      .filter(frame => visibleArea(frame) > 10_000)
      .sort((left, right) => visibleArea(right) - visibleArea(left))[0] || null;
  }

  function hasPlaybackControls(container) {
    if (!container || container === document.body) return false;
    // 常见播放器的进度轨/控制条都会使用以下语义或类名。优先选中包含它们的
    // 最近一层容器，避免聚焦样式只留下 <video> 而把自定义进度条藏掉。
    return Boolean(container.querySelector([
      'input[type="range"]', '[role="slider"]', '[class*="progress" i]',
      '[class*="timeline" i]', '[class*="control-bar" i]',
      '[class*="controls" i]', '[data-testid*="control" i]'
    ].join(", ")));
  }

  // 选择播放器自身或其最小的可见容器，以尽量保留课程站点自定义的进度条和
  // 控制按钮；不移动 DOM 节点，避免 React/Vue 播放器因重挂载而重置进度。
  function findPlayerRoot(video) {
    if (!video) return findMainFrame();
    if (video.controls) return video;
    const videoRect = video.getBoundingClientRect();
    const videoArea = Math.max(1, videoRect.width * videoRect.height);
    let root = video;
    for (let parent = video.parentElement; parent && parent !== document.body;
         parent = parent.parentElement) {
      const rect = parent.getBoundingClientRect();
      const area = rect.width * rect.height;
      if (rect.width < videoRect.width * 0.9 || rect.height < videoRect.height * 0.9 ||
          area > videoArea * 5 || area > innerWidth * innerHeight * 0.96) break;
      root = parent;
      if (hasPlaybackControls(parent)) return parent;
    }
    return root;
  }

  function focusTarget() {
    return findPlayerRoot(findMainVideo());
  }

  function pictureInPictureAvailable(video) {
    return Boolean(video && document.pictureInPictureEnabled &&
      !video.disablePictureInPicture && video.readyState > 0);
  }

  function ensurePipButton() {
    if (pipButton?.isConnected) return pipButton;
    pipButton = document.createElement("button");
    pipButton.type = "button";
    pipButton.dataset.quizpanePipButton = "true";
    pipButton.setAttribute("aria-label", "置顶播放");
    pipButton.title = "置顶播放（浏览器画中画）";
    Object.assign(pipButton.style, {
      position: "fixed", zIndex: "2147483647", display: "none", border: "0",
      borderRadius: "7px", padding: "7px 10px", color: "#fff", background: "rgba(19, 23, 30, .88)",
      boxShadow: "0 2px 10px rgba(0, 0, 0, .35)", font: "600 13px system-ui, sans-serif",
      cursor: "pointer", lineHeight: "18px"
    });
    pipButton.addEventListener("click", async event => {
      // 这里必须是页面中用户真正点击按钮的同步处理函数，不能由桌面端或
      // service worker 转发；否则 Chrome/Edge 会拒绝进入原生 PiP。
      event.preventDefault();
      event.stopPropagation();
      const video = findMainVideo();
      if (!pictureInPictureAvailable(video)) return;
      pipButton.disabled = true;
      try {
        if (document.pictureInPictureElement === video) await document.exitPictureInPicture();
        else await video.requestPictureInPicture();
      } catch {
        pipButton.title = "当前课程站点不允许置顶播放";
      } finally {
        pipButton.disabled = false;
        updatePipButton();
        reportStatus();
      }
    }, true);
    document.body.append(pipButton);
    return pipButton;
  }

  function updatePipButton() {
    const button = ensurePipButton();
    const video = findMainVideo();
    if (!pictureInPictureAvailable(video)) {
      button.style.display = "none";
      return;
    }
    const rect = video.getBoundingClientRect();
    if (rect.width < 180 || rect.height < 100 || rect.bottom < 0 || rect.top > innerHeight) {
      button.style.display = "none";
      return;
    }
    button.textContent = document.pictureInPictureElement === video ? "退出置顶" : "置顶播放";
    // display:none 时 offsetWidth 为 0，先显示再定位才能让按钮完整落在视频右上角。
    button.style.display = "block";
    button.style.top = `${Math.max(8, rect.top + 10)}px`;
    button.style.left = `${Math.max(8, rect.right - button.offsetWidth - 10)}px`;
  }

  function snapshot() {
    const video = findMainVideo();
    // 只发送播放器的时间数字，不读取视频地址、页面正文或任何账户信息。-1 表示
    // 课程站点尚未提供该值（例如直播或 metadata 还未加载）。
    const currentTime = Number.isFinite(video?.currentTime) ? Math.floor(video.currentTime) : -1;
    const duration = Number.isFinite(video?.duration) ? Math.floor(video.duration) : -1;
    return {
      bound: true,
      courseTitle: windowBindingOriginalTitle ?? document.title,
      videoDetected: Boolean(video),
      videoState: video && !video.paused && !video.ended ? "playing" : "paused",
      videoCurrentTimeSeconds: currentTime,
      videoDurationSeconds: duration,
      pictureInPicture: document.pictureInPictureElement === video,
      focusMode: Boolean(focusState)
    };
  }

  function reportStatus() {
    const payload = snapshot();
    const fingerprint = JSON.stringify(payload);
    // 暂停后状态不会变化，不再反复唤醒 service worker；播放期间时间戳每秒变化，
    // 因此仍会按需更新桌面端进度。
    if (fingerprint === lastReportedSnapshot) return;
    lastReportedSnapshot = fingerprint;
    chrome.runtime.sendMessage({ type: "content-status", payload }).catch(() => {});
  }

  async function pauseForBoss() {
    const video = findMainVideo();
    bossWasPlaying = Boolean(video && !video.paused && !video.ended);
    if (video) video.pause();
    reportStatus();
    return { success: true, ...snapshot(), wasPlaying: bossWasPlaying };
  }

  async function restoreFromBoss() {
    const video = findMainVideo();
    if (!video || !bossWasPlaying) {
      bossWasPlaying = false;
      reportStatus();
      return { success: true, resumed: false, reason: video ? "was-paused" : "video-not-found" };
    }
    try {
      await video.play();
      return { success: true, resumed: true, ...snapshot() };
    } catch (error) {
      return { success: false, resumed: false, error: "autoplay-blocked", detail: String(error) };
    } finally {
      bossWasPlaying = false;
      reportStatus();
    }
  }

  async function togglePlayback() {
    const video = findMainVideo();
    if (!video) return { success: false, error: "video-not-found" };
    try {
      if (video.paused || video.ended) await video.play();
      else video.pause();
      reportStatus();
      return { success: true, ...snapshot() };
    } catch (error) {
      return { success: false, error: "playback-failed", detail: String(error) };
    }
  }

  async function ensurePlaying() {
    const video = findMainVideo();
    if (!video) return { success: false, error: "video-not-found" };
    try {
      if (video.ended && Number.isFinite(video.duration) && video.duration > 0)
        video.currentTime = 0;
      await video.play();
      // play() resolve 后再跨一个事件循环确认播放器没有被站点脚本立刻暂停。
      await new Promise(resolve => setTimeout(resolve, 120));
      const playing = !video.paused && !video.ended;
      reportStatus();
      return {
        success: playing,
        resumed: playing,
        error: playing ? undefined : "video-remained-paused",
        ...snapshot()
      };
    } catch (error) {
      reportStatus();
      return { success: false, resumed: false, error: "playback-failed", detail: String(error), ...snapshot() };
    }
  }

  async function controlVideo(action, position) {
    const video = findMainVideo();
    if (!video) return { success: false, error: "video-not-found" };
    if (action === "seek") {
      const ratio = Number(position);
      if (!Number.isFinite(ratio) || !Number.isFinite(video.duration) || video.duration <= 0)
        return { success: false, error: "video-duration-unavailable" };
      video.currentTime = Math.max(0, Math.min(1, ratio)) * video.duration;
      reportStatus();
      return { success: true, ...snapshot() };
    }
    if (action === "toggle") return togglePlayback();
    return { success: false, error: "unsupported-video-control" };
  }

  function enterFocusMode() {
    if (focusState?.target?.isConnected) return { success: true, ...snapshot() };
    const target = focusTarget();
    if (!target) return { success: false, error: "player-not-found" };
    const targetStyle = target.getAttribute("style");
    const htmlStyle = document.documentElement.getAttribute("style");
    const bodyStyle = document.body.getAttribute("style");
    const style = document.createElement("style");
    style.id = "quizpane-video-focus-style";
    style.textContent = `
      html[data-quizpane-video-focus], body[data-quizpane-video-focus] {
        background: #000 !important; overflow: hidden !important;
      }
      body[data-quizpane-video-focus] > * {
        visibility: hidden !important; pointer-events: none !important;
      }
      [data-quizpane-video-focus-target] {
        position: fixed !important; inset: 0 !important; z-index: 2147483647 !important;
        width: 100vw !important; height: 100vh !important; max-width: none !important;
        max-height: none !important; margin: 0 !important; background: #000 !important;
        visibility: visible !important; pointer-events: auto !important;
      }
      [data-quizpane-video-focus-target],
      [data-quizpane-video-focus-target] * { visibility: visible !important; }
      video[data-quizpane-video-focus-target] { object-fit: contain !important; }
      body[data-quizpane-video-focus] > [data-quizpane-pip-button] {
        visibility: visible !important; pointer-events: auto !important;
      }
    `;
    document.head.append(style);
    document.documentElement.dataset.quizpaneVideoFocus = "true";
    document.body.dataset.quizpaneVideoFocus = "true";
    target.dataset.quizpaneVideoFocusTarget = "true";
    focusState = { target, targetStyle, htmlStyle, bodyStyle, style };
    reportStatus();
    return { success: true, target: target.tagName.toLowerCase(), ...snapshot() };
  }

  function restoreStyle(element, previousStyle) {
    if (previousStyle === null) element.removeAttribute("style");
    else element.setAttribute("style", previousStyle);
  }

  function exitFocusMode() {
    if (!focusState) return { success: true, ...snapshot() };
    const { target, targetStyle, htmlStyle, bodyStyle, style } = focusState;
    style.remove();
    document.documentElement.removeAttribute("data-quizpane-video-focus");
    document.body.removeAttribute("data-quizpane-video-focus");
    if (target.isConnected) {
      target.removeAttribute("data-quizpane-video-focus-target");
      restoreStyle(target, targetStyle);
    }
    restoreStyle(document.documentElement, htmlStyle);
    restoreStyle(document.body, bodyStyle);
    focusState = null;
    reportStatus();
    return { success: true, ...snapshot() };
  }

  function setWindowBindingTitle(bindingToken) {
    if (!/^__QUIZPANE_WINDOW_[0-9a-f-]{16,}__$/i.test(String(bindingToken || ""))) {
      return { success: false, error: "invalid-window-binding-token" };
    }
    if (windowBindingOriginalTitle === null) windowBindingOriginalTitle = document.title;
    document.title = bindingToken;
    reportStatus();
    return { success: true };
  }

  function restoreWindowBindingTitle() {
    if (windowBindingOriginalTitle !== null) {
      document.title = windowBindingOriginalTitle;
      windowBindingOriginalTitle = null;
      reportStatus();
    }
    return { success: true };
  }

  chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    let operation;
    switch (message?.type) {
      case "command.boss_hide": operation = pauseForBoss(); break;
      case "command.boss_restore": operation = restoreFromBoss(); break;
      case "command.toggle_playback": operation = togglePlayback(); break;
      case "command.ensure_playing": operation = ensurePlaying(); break;
      case "command.video_control":
        operation = controlVideo(message.action, message.position); break;
      case "command.enter_focus_mode": operation = Promise.resolve(enterFocusMode()); break;
      case "command.exit_focus_mode": operation = Promise.resolve(exitFocusMode()); break;
      case "command.set_window_binding_title":
        operation = Promise.resolve(setWindowBindingTitle(message.bindingToken)); break;
      case "command.restore_window_binding_title":
        operation = Promise.resolve(restoreWindowBindingTitle()); break;
      case "command.query_status": operation = Promise.resolve({ success: true, ...snapshot() }); break;
      default: return false;
    }
    operation.then(sendResponse).catch(error => sendResponse({ success: false, error: String(error) }));
    return true;
  });

  const observer = new MutationObserver(() => {
    if (focusState && !focusState.target.isConnected) exitFocusMode();
    clearTimeout(observer.timer);
    observer.timer = setTimeout(() => {
      updatePipButton();
      reportStatus();
    }, 300);
  });
  observer.observe(document.documentElement, { childList: true, subtree: true });
  for (const eventName of ["play", "pause", "ended", "loadedmetadata", "durationchange", "emptied",
                           "enterpictureinpicture", "leavepictureinpicture"]) {
    document.addEventListener(eventName, () => {
      updatePipButton();
      reportStatus();
    }, true);
  }
  addEventListener("scroll", updatePipButton, { passive: true, capture: true });
  addEventListener("resize", updatePipButton, { passive: true });
  // timeupdate 在不同课程站点的频率并不一致；播放期间固定每秒同步一次可让
  // 桌面端进度稳定。暂停时不轮询、不发消息。
  setInterval(() => {
    const video = findMainVideo();
    if (video && !video.paused && !video.ended) reportStatus();
  }, 1_000);
  updatePipButton();
  reportStatus();
})();
