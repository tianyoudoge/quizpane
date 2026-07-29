// 该控制器只在 macOS 的实验镜像路径中使用。它不保存、上传或重编码视频；目的
// 是让扩展的 offscreen document 持有 Chrome tabCapture 流，并上报其活跃状态与
// 帧率，验证 Chrome 在源窗被覆盖/隐藏时是否仍持续合成视频帧。
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
    if (!isMacOS() || !Number.isInteger(tabId)) return { success: false, skipped: true };
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
