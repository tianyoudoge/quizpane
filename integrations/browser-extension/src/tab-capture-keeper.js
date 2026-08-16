// 该控制器只在 macOS 的实验镜像路径中使用。Windows（包括 Chrome 109/Win7）
// 操作的是真实浏览器 popup，不需要捕获视频。Chrome 116 以前不能把 service
// worker 获取的 stream ID 交给 offscreen document，因此旧浏览器必须在触碰这些
// API 前安全跳过，不能影响其余网课伴侣功能。
export function createTabCaptureKeeper({ chromeApi, send, isMacOS }) {
  let activeTabId = null;
  let starting = null;

  async function ensureOffscreenDocument() {
    const contexts = await chromeApi.runtime.getContexts({
      contextTypes: ["OFFSCREEN_DOCUMENT"]
    });
    if (contexts.length) return;
    await chromeApi.offscreen.createDocument({
      url: "src/tab-capture-keeper.html",
      reasons: ["USER_MEDIA"],
      justification: "Keep the locally captured course video stream alive for the macOS mirror experiment."
    });
  }

  async function start(tabId) {
    if (!isMacOS() || !Number.isInteger(tabId)) {
      return { success: false, skipped: true, reason: "platform-not-captured" };
    }
    if (typeof chromeApi.runtime?.getContexts !== "function"
        || typeof chromeApi.tabCapture?.getMediaStreamId !== "function"
        || typeof chromeApi.offscreen?.createDocument !== "function") {
      return { success: false, skipped: true, reason: "chrome-116-capture-unavailable" };
    }
    if (activeTabId === tabId) return { success: true, alreadyActive: true };
    if (starting) return starting;
    starting = (async () => {
      try {
        // 必须在用户点击扩展的调用链内先取得 stream ID；后续创建 offscreen
        // document 不再依赖 transient user activation。
        const streamId = await chromeApi.tabCapture.getMediaStreamId({ targetTabId: tabId });
        await ensureOffscreenDocument();
        await chromeApi.runtime.sendMessage({
          type: "tab-capture-keeper.start",
          payload: { streamId, tabId }
        });
        activeTabId = tabId;
        send("externalWindow.tab_capture", { event: "start-requested", tabId });
        return { success: true };
      } catch (error) {
        const detail = String(error);
        send("externalWindow.tab_capture", { event: "start-failed", tabId, error: detail });
        return { success: false, error: detail };
      } finally {
        starting = null;
      }
    })();
    return starting;
  }

  async function stop() {
    if (activeTabId === null && !starting) return;
    const tabId = activeTabId;
    activeTabId = null;
    starting = null;
    try {
      await chromeApi.runtime.sendMessage({ type: "tab-capture-keeper.stop" });
      send("externalWindow.tab_capture", { event: "stop-requested", tabId });
    } catch (error) {
      send("externalWindow.tab_capture", {
        event: "stop-failed", tabId, error: String(error)
      });
    }
  }

  return { start, stop };
}
