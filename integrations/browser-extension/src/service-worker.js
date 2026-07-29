import { createBridgeClient } from "./bridge-client.js";
import { createCourseBindingController } from "./course-binding.js";
import { createExternalWindowController } from "./external-window.js";
import { createTabCaptureKeeper } from "./tab-capture-keeper.js";
import { createUpdateChecker, UPDATE_ALARM } from "./update-checker.js";

function emptyCourseState() {
  return {
    bound: false,
    videoDetected: false,
    videoState: "paused",
    courseTitle: "",
    videoCurrentTimeSeconds: -1,
    videoDurationSeconds: -1
  };
}

const state = {
  boundTabId: null,
  courseState: emptyCourseState(),
  courseWindowId: null,
  originWindowId: null,
  originTabIndex: null,
  courseWindowMinimized: false,
  externalWindowBinding: null,
  externalWindowStatus: null
};

const restored = chrome.storage.session.get([
  "boundTabId",
  "courseState",
  "courseWindowId",
  "originWindowId",
  "originTabIndex",
  "courseWindowMinimized",
  "externalWindowBinding",
  "externalWindowStatus"
]).then(saved => {
  state.boundTabId = Number.isInteger(saved.boundTabId) ? saved.boundTabId : null;
  if (saved.courseState) state.courseState = saved.courseState;
  state.courseWindowId = Number.isInteger(saved.courseWindowId) ? saved.courseWindowId : null;
  state.originWindowId = Number.isInteger(saved.originWindowId) ? saved.originWindowId : null;
  state.originTabIndex = Number.isInteger(saved.originTabIndex) ? saved.originTabIndex : null;
  state.courseWindowMinimized = Boolean(saved.courseWindowMinimized);
  state.externalWindowBinding = saved.externalWindowBinding || null;
  state.externalWindowStatus = saved.externalWindowStatus || null;
});

function browserName() {
  return navigator.userAgent.includes("Edg/") ? "Edge" : "Chrome";
}

function persistState() {
  return chrome.storage.session.set(state);
}

const context = {
  chromeApi: chrome,
  state,
  restored,
  emptyCourseState,
  persistState,
  publishStatus: () => publishStatus(),
  send: (...args) => bridge.send(...args),
  randomUUID: () => crypto.randomUUID(),
  setTimeoutImpl: setTimeout,
  externalWindow: null
};

const course = createCourseBindingController(context);
const externalWindow = createExternalWindowController(context);
context.externalWindow = externalWindow;
const tabCaptureKeeper = createTabCaptureKeeper({
  chromeApi: chrome,
  send: (...args) => bridge.send(...args),
  isMacOS: () => navigator.userAgent.includes("Macintosh")
});
context.tabCaptureKeeper = tabCaptureKeeper;

const bridge = createBridgeClient({
  browserName,
  onConnected: async () => {
    publishStatus();
    if (!state.courseWindowId) return;
    const staleBinding = state.externalWindowBinding;
    const staleWasMacReplica = staleBinding?.backend === "macos-captured-replica";
    state.externalWindowBinding = null;
    state.externalWindowStatus = null;
    if (staleBinding) await externalWindow.restoreBindingTitle(staleBinding);
    await persistState();
    try {
      const popup = staleWasMacReplica
        ? await chrome.windows.get(state.courseWindowId)
        : await chrome.windows.update(state.courseWindowId, {
            state: "normal",
            focused: false
          });
      state.courseWindowMinimized = false;
      await persistState();
      await course.forwardCommand("command.enter_focus_mode");
      await course.forwardCommand("command.ensure_playing");
      externalWindow.attach(popup).catch(() => {});
    } catch {
      state.courseWindowId = null;
      state.courseWindowMinimized = false;
      await persistState();
    }
  },
  onExternalWindowAttached: payload => externalWindow.handleAttached(payload),
  onCommand: (type, payload) => course.handleDesktopCommand(type, payload)
});

const updateChecker = createUpdateChecker({ chromeApi: chrome });

function publishStatus() {
  bridge.send("event.status_snapshot", {
    ...state.courseState,
    tabId: state.boundTabId,
    browser: browserName(),
    courseWindowMode: state.courseWindowId ? "popup" : "tab",
    courseWindowVisible: Boolean(state.courseWindowId && !state.courseWindowMinimized),
    externalWindowStatus: state.externalWindowStatus
  });
}

function start() {
  bridge.connect();
  updateChecker.scheduleUpdateChecks();
  updateChecker.checkForUpdate();
}

chrome.runtime.onInstalled.addListener(start);
chrome.runtime.onStartup.addListener(start);
chrome.alarms.onAlarm.addListener(alarm => {
  if (alarm.name === UPDATE_ALARM) updateChecker.checkForUpdate();
});
chrome.tabs.onRemoved.addListener(tabId => {
  if (tabId !== state.boundTabId) return;
  tabCaptureKeeper.stop();
  course.clearBinding();
});
chrome.windows.onRemoved.addListener(windowId => {
  if (windowId !== state.courseWindowId) return;
  externalWindow.detach();
  state.courseWindowId = null;
  state.courseWindowMinimized = false;
  persistState();
  publishStatus();
});
chrome.tabs.onUpdated.addListener((tabId, change) => {
  course.handleNavigation(tabId, change).catch(() => {});
});

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  (async () => {
    await restored;
    if (request?.type === "content-status" && sender.tab?.id === state.boundTabId) {
      course.updateCourseState(request.payload);
      return { ok: true };
    }
    if (request?.type === "content-source-pointer" && sender.tab?.id === state.boundTabId &&
        state.externalWindowBinding?.sessionId) {
      bridge.send("externalWindow.source_input", {
        sessionId: state.externalWindowBinding.sessionId,
        ...request.payload
      });
      return { ok: true };
    }
    if (request?.type === "tab-capture-keeper.event") {
      bridge.send("externalWindow.tab_capture", request.payload || {});
      return { ok: true };
    }
    if (request?.type === "bind-current-tab") {
      return course.bindCurrentTab(request.tabId, request.openWindow);
    }
    if (request?.type === "get-popup-state") {
      return {
        connected: bridge.isConnected(),
        boundTabId: state.boundTabId,
        courseWindowId: state.courseWindowId,
        courseWindowVisible: Boolean(
          state.courseWindowId && !state.courseWindowMinimized
        ),
        externalWindowStatus: state.externalWindowStatus,
        updateInfo: await updateChecker.storedUpdateInfo(),
        ...state.courseState
      };
    }
    if (request?.type === "check-for-update") {
      return updateChecker.checkForUpdate({ force: Boolean(request.force) });
    }
    if (request?.type === "toggle-playback") {
      return course.forwardCommand("command.toggle_playback");
    }
    if (request?.type === "show-course-window") return course.showCourseWindow();
    if (request?.type === "return-tab") return course.returnTab();
    if (request?.type === "unbind") {
      if (state.courseWindowId) await course.returnTab();
      else await course.forwardCommand("command.exit_focus_mode");
      await course.disposeController();
      course.clearBinding();
      return { success: true };
    }
    return { success: false, error: "unsupported-request" };
  })().then(sendResponse).catch(error => {
    sendResponse({ success: false, error: String(error) });
  });
  return true;
});

bridge.connect();
