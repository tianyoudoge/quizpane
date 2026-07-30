import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { createCourseBindingController } from "../../integrations/browser-extension/src/course-binding.js";
import {
  ATTACH_TIMEOUT_MS,
  createExternalWindowController
} from "../../integrations/browser-extension/src/external-window.js";
import { createTabCaptureKeeper } from "../../integrations/browser-extension/src/tab-capture-keeper.js";
import {
  createUpdateChecker,
  EXTENSION_DOWNLOAD_PAGE_URL
} from "../../integrations/browser-extension/src/update-checker.js";

function createHarness(overrides = {}) {
  const calls = [];
  const state = {
    boundTabId: 7,
    courseState: {
      bound: true,
      videoDetected: true,
      videoState: "playing",
      courseTitle: "测试课程",
      videoCurrentTimeSeconds: 3,
      videoDurationSeconds: 30
    },
    courseWindowId: 12,
    originWindowId: 4,
    originTabIndex: 2,
    courseWindowMinimized: false,
    externalWindowBinding: null,
    externalWindowStatus: null
  };
  const chromeApi = {
    tabs: {
      get: async tabId => ({ id: tabId, windowId: 4, index: 2 }),
      move: async (tabId, options) => calls.push(["tabs.move", tabId, options]),
      sendMessage: async (tabId, message) => {
        calls.push(["tabs.sendMessage", tabId, message]);
        if (message.type === "command.set_window_binding_title") return { success: true };
        return { success: true, videoDetected: true };
      }
    },
    windows: {
      create: async options => ({ id: 12, ...options }),
      get: async id => ({ id, left: 10, top: 20, width: 640, height: 420 }),
      update: async (id, options) => {
        calls.push(["windows.update", id, options]);
        return { id, ...options };
      }
    },
    scripting: {
      executeScript: async options => calls.push(["executeScript", options])
    },
    system: {
      display: {
        getInfo: async () => [{
          isPrimary: true,
          bounds: { left: 0, top: 0, width: 1920, height: 1080 }
        }]
      }
    }
  };
  Object.assign(chromeApi, overrides.chromeApi);
  const timers = [];
  const context = {
    chromeApi,
    state,
    restored: Promise.resolve(),
    emptyCourseState: () => ({
      bound: false,
      videoDetected: false,
      videoState: "paused",
      courseTitle: "",
      videoCurrentTimeSeconds: -1,
      videoDurationSeconds: -1
    }),
    persistState: async () => calls.push(["persist"]),
    publishStatus: () => calls.push(["publish"]),
    send: (type, payload) => {
      calls.push(["send", type, payload]);
      return true;
    },
    randomUUID: (() => {
      let value = 0;
      return () => `uuid-${++value}`;
    })(),
    setTimeoutImpl: (callback, delay) => {
      timers.push({ callback, delay });
      return timers.length;
    },
    externalWindow: null
  };
  const course = createCourseBindingController(context);
  const externalWindow = createExternalWindowController(context);
  context.externalWindow = externalWindow;
  return { calls, chromeApi, context, course, externalWindow, state, timers };
}

test("iframe fixture is injected in all frames", async () => {
  const iframeFixture = await readFile(
    new URL("./iframe-video.html", import.meta.url), "utf8");
  const videoFixture = await readFile(
    new URL("./single-video.html", import.meta.url), "utf8");
  assert.match(iframeFixture, /src="\.\/single-video\.html"/);
  assert.match(videoFixture, /<video\b/);

  const harness = createHarness();
  await harness.course.installController(7);
  assert.deepEqual(harness.calls[0], [
    "executeScript",
    {
      target: { tabId: 7, allFrames: true },
      files: ["src/content-script.js"]
    }
  ]);
});

test("completed navigation reinjects the controller and restores focus mode", async () => {
  const harness = createHarness();
  await harness.course.handleNavigation(7, { status: "complete" });
  assert.equal(harness.calls[0][0], "executeScript");
  assert.equal(harness.calls[1][0], "tabs.sendMessage");
  assert.equal(harness.calls[1][2].type, "command.enter_focus_mode");
});

test("pending external attach restores the temporary title after timeout", async () => {
  const harness = createHarness();
  assert.equal(await harness.externalWindow.attach({
    id: 12, left: 10, top: 20, width: 640, height: 420
  }), true);
  assert.equal(harness.timers.length, 1);
  assert.equal(harness.timers[0].delay, ATTACH_TIMEOUT_MS);
  await harness.timers[0].callback();
  assert.equal(harness.state.externalWindowBinding, null);
  assert.match(harness.state.externalWindowStatus.error, /超时/);
  assert.ok(harness.calls.some(call =>
    call[0] === "tabs.sendMessage"
      && call[2].type === "command.restore_window_binding_title"));
});

test("macOS attach leaves source-window parking to the desktop accessibility probe", async () => {
  const harness = createHarness();
  harness.state.externalWindowBinding = {
    sessionId: "session-1",
    bindingToken: "binding-1",
    pending: true
  };

  await harness.externalWindow.handleAttached({
    sessionId: "session-1",
    success: true,
    backend: "macos-captured-replica"
  });

  assert.equal(harness.state.courseWindowMinimized, false);
  assert.ok(!harness.calls.some(call =>
    call[0] === "windows.update" && call[1] === 12
      && Object.hasOwn(call[2], "left")));
});

test("tab capture keeper obtains the stream ID before creating the offscreen consumer", async () => {
  const calls = [];
  const keeper = createTabCaptureKeeper({
    chromeApi: {
      tabCapture: {
        getMediaStreamId: async options => {
          calls.push(["stream-id", options]);
          return "stream-1";
        }
      },
      runtime: {
        getContexts: async options => {
          calls.push(["contexts", options]);
          return [];
        },
        sendMessage: async message => calls.push(["message", message])
      },
      offscreen: {
        createDocument: async options => calls.push(["offscreen", options])
      }
    },
    send: (type, payload) => calls.push(["bridge", type, payload]),
    isMacOS: () => true
  });

  assert.deepEqual(await keeper.start(7), { success: true });
  assert.deepEqual(calls.map(call => call[0]), [
    "stream-id", "contexts", "offscreen", "message", "bridge"
  ]);
  assert.deepEqual(calls[0][1], { targetTabId: 7 });
  assert.equal(calls[3][1].type, "tab-capture-keeper.start");
  assert.equal(calls[3][1].payload.streamId, "stream-1");
});

test("boss hide and restore keep the window state machine consistent", async () => {
  const harness = createHarness();
  const hidden = await harness.course.bossHide();
  assert.equal(hidden.windowMinimized, true);
  assert.equal(harness.state.courseWindowMinimized, true);
  assert.ok(harness.calls.some(call =>
    call[0] === "windows.update" && call[2].state === "minimized"));

  const restored = await harness.course.bossRestore();
  assert.equal(restored.success, true);
  assert.equal(harness.state.courseWindowMinimized, false);
  assert.ok(harness.calls.some(call =>
    call[0] === "windows.update"
      && call[2].state === "normal"
      && call[2].focused === false));
});

test("return tab moves it back to the recorded window and clears popup state", async () => {
  const harness = createHarness();
  const result = await harness.course.returnTab();
  assert.equal(result.success, true);
  assert.ok(harness.calls.some(call =>
    call[0] === "tabs.move"
      && call[1] === 7
      && call[2].windowId === 4
      && call[2].index === 2));
  assert.equal(harness.state.courseWindowId, null);
  assert.equal(harness.state.originWindowId, null);
  assert.equal(harness.state.originTabIndex, null);
});

test("binding a different tab returns and disposes the previous course page", async () => {
  const harness = createHarness();
  const result = await harness.course.bindCurrentTab(9);
  assert.equal(result.success, true);
  assert.equal(harness.state.boundTabId, 9);
  assert.ok(harness.calls.some(call =>
    call[0] === "tabs.move" && call[1] === 7 && call[2].windowId === 4));
  assert.ok(harness.calls.some(call =>
    call[0] === "tabs.sendMessage"
      && call[1] === 7
      && call[2].type === "command.dispose_controller"));
});

test("extension update notice leads to the official download page", async () => {
  const writes = [];
  const checker = createUpdateChecker({
    chromeApi: {
      storage: {
        local: {
          get: async () => ({}),
          set: async value => writes.push(value)
        }
      },
      runtime: { getManifest: () => ({ version: "0.1.1" }) },
      alarms: { create() {} }
    },
    fetchImpl: async url => {
      assert.equal(url.toString(),
        "https://xutianyou.cc/quizpane/api/releases/latest?refresh=1");
      return ({
      ok: true,
      json: async () => ({
        tag: "v0.4.0",
        assets: { "QuizPane-course-companion.zip": { size: 1 } }
      })
      });
    },
    now: () => 1
  });

  const update = await checker.checkForUpdate({ force: true });
  assert.equal(update.available, true);
  assert.equal(update.downloadPageUrl, EXTENSION_DOWNLOAD_PAGE_URL);
  assert.equal(update.downloadPageUrl,
    "https://xutianyou.cc/quizpane/course-companion.html");
  assert.equal(writes[0].updateInfo.downloadPageUrl, EXTENSION_DOWNLOAD_PAGE_URL);
});
