const MAC_SOURCE_PARK_EXPOSE_W = 70;
const MAC_SOURCE_PARK_EXPOSE_H = 28;
export const ATTACH_TIMEOUT_MS = 30_000;

export function createExternalWindowController(context) {
  const { chromeApi, state, restored, randomUUID, setTimeoutImpl } = context;

  async function restoreBindingTitle(binding = state.externalWindowBinding) {
    if (!binding?.bindingToken || !state.boundTabId) return;
    try {
      await chromeApi.tabs.sendMessage(state.boundTabId, {
        type: "command.restore_window_binding_title"
      });
    } catch {
      // 页面关闭、跳转或权限撤销时标题自然随窗口销毁。
    }
  }

  function detach() {
    const binding = state.externalWindowBinding;
    state.externalWindowBinding = null;
    state.externalWindowStatus = null;
    if (binding?.sessionId) {
      context.send("externalWindow.detach", { sessionId: binding.sessionId });
    }
    restoreBindingTitle(binding);
  }

  function usesMacCapturedReplica() {
    return state.externalWindowBinding?.backend === "macos-captured-replica";
  }

  async function attach(popup = null) {
    await restored;
    if (!state.boundTabId || !state.courseWindowId || state.externalWindowBinding) return false;
    const sessionId = randomUUID();
    const bindingToken = `__QUIZPANE_WINDOW_${randomUUID()}__`;
    let titleResult;
    try {
      titleResult = await chromeApi.tabs.sendMessage(state.boundTabId, {
        type: "command.set_window_binding_title",
        bindingToken
      });
    } catch {
      return false;
    }
    if (!titleResult?.success) return false;
    let browserWindow = popup;
    try {
      if (!browserWindow?.id) {
        browserWindow = await chromeApi.windows.get(state.courseWindowId);
      }
    } catch {
      await restoreBindingTitle({ bindingToken });
      return false;
    }
    state.externalWindowBinding = { sessionId, bindingToken, pending: true };
    state.externalWindowStatus = { pending: true };
    await context.persistState();
    const sent = context.send("externalWindow.attach", {
      sessionId,
      bindingToken,
      chromeWindowId: state.courseWindowId,
      tabId: state.boundTabId,
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
      state.externalWindowBinding = null;
      state.externalWindowStatus = { error: "未连接本机 QuizPane，无法创建置顶视频小窗" };
      await context.persistState();
      await restoreBindingTitle({ bindingToken });
      return false;
    }
    setTimeoutImpl(async () => {
      if (state.externalWindowBinding?.sessionId === sessionId
          && state.externalWindowBinding.pending) {
        const timedOutBinding = state.externalWindowBinding;
        state.externalWindowBinding = null;
        state.externalWindowStatus = { error: "创建置顶视频小窗超时，请重试" };
        await context.persistState();
        await restoreBindingTitle(timedOutBinding);
        context.publishStatus();
      }
    }, ATTACH_TIMEOUT_MS);
    return true;
  }

  async function parkMacSourceToCorner(windowId) {
    const displays = await chromeApi.system.display.getInfo();
    const primary = displays.find(display => display.isPrimary) || displays[0];
    if (!primary) throw new Error("no-display");
    const bounds = primary.bounds;
    const left = bounds.left + bounds.width - MAC_SOURCE_PARK_EXPOSE_W;
    const top = bounds.top + bounds.height - MAC_SOURCE_PARK_EXPOSE_H;
    const parked = await chromeApi.windows.update(windowId, {
      state: "normal",
      left: Math.round(left),
      top: Math.round(top),
      focused: false
    });
    return {
      windowId,
      left: parked.left,
      top: parked.top,
      state: parked.state,
      screen: { w: bounds.width, h: bounds.height }
    };
  }

  async function handleAttached(payload) {
    if (!state.externalWindowBinding
        || payload.sessionId !== state.externalWindowBinding.sessionId) return;
    const binding = state.externalWindowBinding;
    state.externalWindowBinding = payload.success
      ? { ...binding, pending: false, backend: payload.backend || "unknown" }
      : null;
    state.externalWindowStatus = payload.success
      ? { backend: payload.backend || "unknown" }
      : { error: payload.error || "无法创建置顶视频小窗" };
    if (payload.success && payload.backend === "macos-captured-replica"
        && state.courseWindowId) {
      try {
        await parkMacSourceToCorner(state.courseWindowId);
      } catch (error) {
        console.warn("[QuizPane] cannot park course source", String(error));
      }
      state.courseWindowMinimized = false;
    }
    await context.persistState();
    await restoreBindingTitle(binding);
  }

  return {
    attach,
    detach,
    handleAttached,
    restoreBindingTitle,
    usesMacCapturedReplica
  };
}
