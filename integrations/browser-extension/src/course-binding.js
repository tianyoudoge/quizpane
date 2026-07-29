export function createCourseBindingController(context) {
  const { chromeApi, state, restored } = context;

  async function disposeController(tabId = state.boundTabId) {
    if (!Number.isInteger(tabId)) return false;
    try {
      await chromeApi.tabs.sendMessage(tabId, {
        type: "command.dispose_controller"
      });
      return true;
    } catch {
      // 标签页关闭或导航时，浏览器会自行销毁 content script 上下文。
      return false;
    }
  }

  async function forwardCommand(type, payload = {}) {
    await restored;
    if (!state.boundTabId) return { success: false, error: "no-bound-course" };
    try {
      const result = await chromeApi.tabs.sendMessage(state.boundTabId, { type, ...payload });
      if (result?.videoDetected !== undefined) updateCourseState(result);
      return result || { success: false, error: "empty-content-response" };
    } catch {
      clearBinding();
      return { success: false, error: "bound-tab-unavailable" };
    }
  }

  function updateCourseState(next) {
    const previous = state.courseState;
    state.courseState = { ...previous, ...next, bound: Boolean(state.boundTabId) };
    const persistentFields = ["bound", "videoDetected", "videoState", "courseTitle", "focusMode"];
    if (persistentFields.some(field => previous[field] !== state.courseState[field])) {
      context.persistState();
    }
    context.publishStatus();
  }

  function clearBinding() {
    context.tabCaptureKeeper?.stop();
    context.externalWindow.detach();
    state.boundTabId = null;
    state.courseState = context.emptyCourseState();
    state.courseWindowId = null;
    state.originWindowId = null;
    state.originTabIndex = null;
    state.courseWindowMinimized = false;
    context.persistState();
    context.publishStatus();
  }

  async function enterCourseWindow() {
    await restored;
    if (!state.boundTabId) return { success: false, error: "no-bound-course" };
    if (state.courseWindowId) return showCourseWindow();
    try {
      // 这个调用必须贴近用户点击扩展的事件链：tabCapture 只允许由用户操作启动。
      // 它只是实验性保活，不影响创建 popup 或现有 SCK 镜像的成功与否。
      const captureStart = context.tabCaptureKeeper?.start(state.boundTabId);
      const tab = await chromeApi.tabs.get(state.boundTabId);
      state.originWindowId = tab.windowId;
      state.originTabIndex = tab.index;
      const popup = await chromeApi.windows.create({
        tabId: state.boundTabId,
        type: "popup",
        width: 640,
        height: 420,
        focused: false
      });
      state.courseWindowId = popup.id ?? null;
      state.courseWindowMinimized = false;
      await context.persistState();
      const focus = await forwardCommand("command.enter_focus_mode");
      const playback = await forwardCommand("command.ensure_playing");
      await captureStart;
      await context.externalWindow.attach(popup);
      context.publishStatus();
      return {
        success: Boolean(state.courseWindowId),
        courseWindowId: state.courseWindowId,
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
    if (!state.courseWindowId) return enterCourseWindow();
    try {
      if (!context.externalWindow.usesMacCapturedReplica()) {
        await chromeApi.windows.update(state.courseWindowId, {
          state: "normal",
          focused: true
        });
      }
      state.courseWindowMinimized = false;
      await context.persistState();
      await forwardCommand("command.enter_focus_mode");
      await forwardCommand("command.ensure_playing");
      await context.externalWindow.attach();
      context.publishStatus();
      return { success: true };
    } catch {
      state.courseWindowId = null;
      state.courseWindowMinimized = false;
      await context.persistState();
      return enterCourseWindow();
    }
  }

  async function hideCourseWindow() {
    if (!state.courseWindowId) return { success: false, error: "course-window-not-open" };
    try {
      if (!context.externalWindow.usesMacCapturedReplica()) {
        await chromeApi.windows.update(state.courseWindowId, { state: "minimized" });
      }
      state.courseWindowMinimized = true;
      await context.persistState();
      context.publishStatus();
      return { success: true };
    } catch (error) {
      return { success: false, error: "cannot-hide-course-window", detail: String(error) };
    }
  }

  async function bossHide() {
    const playback = await forwardCommand("command.boss_hide");
    if (!state.courseWindowId) return playback;
    try {
      if (!context.externalWindow.usesMacCapturedReplica()) {
        await chromeApi.windows.update(state.courseWindowId, { state: "minimized" });
      }
      state.courseWindowMinimized = true;
      await context.persistState();
      context.publishStatus();
      return { ...playback, windowMinimized: true };
    } catch {
      state.courseWindowId = null;
      state.courseWindowMinimized = false;
      await context.persistState();
      context.publishStatus();
      return { ...playback, windowMinimized: false };
    }
  }

  async function bossRestore() {
    if (state.courseWindowId) {
      try {
        if (!context.externalWindow.usesMacCapturedReplica()) {
          await chromeApi.windows.update(state.courseWindowId, {
            state: "normal",
            focused: false
          });
        }
        state.courseWindowMinimized = false;
        await context.persistState();
        context.publishStatus();
      } catch {
        state.courseWindowId = null;
        state.courseWindowMinimized = false;
        await context.persistState();
      }
    }
    return forwardCommand("command.boss_restore");
  }

  async function finalizeBossRestore() {
    if (!state.courseWindowId) {
      return { success: false, error: "course-window-not-open", windowMinimized: false };
    }
    state.courseWindowMinimized = false;
    await context.persistState();
    context.publishStatus();
    return { success: true, windowMinimized: false, windowKeptNormal: true };
  }

  async function returnTab() {
    await restored;
    if (!state.boundTabId) return { success: false, error: "no-bound-course" };
    await forwardCommand("command.exit_focus_mode");
    await context.tabCaptureKeeper?.stop();
    context.externalWindow.detach();
    try {
      if (state.originWindowId !== null) {
        await chromeApi.tabs.move(state.boundTabId, {
          windowId: state.originWindowId,
          index: state.originTabIndex ?? -1
        });
      } else {
        await chromeApi.windows.create({
          tabId: state.boundTabId,
          type: "normal",
          focused: true
        });
      }
    } catch {
      try {
        await chromeApi.windows.create({
          tabId: state.boundTabId,
          type: "normal",
          focused: true
        });
      } catch (error) {
        return { success: false, error: "cannot-return-tab", detail: String(error) };
      }
    }
    state.courseWindowId = null;
    state.originWindowId = null;
    state.originTabIndex = null;
    state.courseWindowMinimized = false;
    await context.persistState();
    context.publishStatus();
    return { success: true };
  }

  function installController(tabId) {
    return chromeApi.scripting.executeScript({
      target: { tabId, allFrames: true },
      files: ["src/content-script.js"]
    });
  }

  async function bindCurrentTab(tabId, openWindow = false) {
    if (!Number.isInteger(tabId)) return { success: false, error: "invalid-tab" };
    try {
      await installController(tabId);
      const result = await chromeApi.tabs.sendMessage(tabId, {
        type: "command.query_status"
      });
      const previousTabId = state.boundTabId;
      if (Number.isInteger(previousTabId) && previousTabId !== tabId) {
        if (state.courseWindowId) {
          const returned = await returnTab();
          if (!returned.success) {
            await disposeController(tabId);
            return returned;
          }
        } else {
          context.externalWindow.detach();
        }
        await disposeController(previousTabId);
      }
      state.boundTabId = tabId;
      updateCourseState(result || {});
      if (openWindow) {
        const windowResult = await enterCourseWindow();
        if (!windowResult.success) return windowResult;
      }
      return { success: true, ...state.courseState };
    } catch (error) {
      return { success: false, error: "cannot-inject", detail: String(error) };
    }
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

  async function handleNavigation(tabId, change) {
    if (tabId !== state.boundTabId) return;
    if (change.status === "loading") {
      state.courseState = {
        ...state.courseState,
        videoDetected: false,
        videoState: "paused",
        videoCurrentTimeSeconds: -1,
        videoDurationSeconds: -1
      };
      context.publishStatus();
    }
    if (change.status === "complete" && state.courseWindowId) {
      await installController(tabId);
      await forwardCommand("command.enter_focus_mode");
    }
  }

  return {
    bindCurrentTab,
    bossHide,
    bossRestore,
    clearBinding,
    disposeController,
    enterCourseWindow,
    finalizeBossRestore,
    forwardCommand,
    handleDesktopCommand,
    handleNavigation,
    hideCourseWindow,
    installController,
    returnTab,
    showCourseWindow,
    updateCourseState
  };
}
