export const ATTACH_TIMEOUT_MS = 30_000;

export function createExternalWindowController(context) {
  const { chromeApi, state, restored, randomUUID, setTimeoutImpl, send } = context;

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
    if (payload.success && payload.backend === "macos-captured-replica") {
      // Chrome 的 windows.update 会拒绝把普通 popup 挪到只露标题栏的位置。
      // 诊断包由桌面端在首帧后以 Accessibility API 做可回滚验证；扩展不再
      // 二次写 bounds，避免把本机的真实结果覆盖掉。
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
