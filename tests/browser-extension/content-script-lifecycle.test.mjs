import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

const contentScriptSource = readFile(
  new URL("../../integrations/browser-extension/src/content-script.js", import.meta.url),
  "utf8"
);

function createDomNode(tagName = "DIV", bounds = {}) {
  const rect = {
    left: 0,
    top: 0,
    right: 0,
    bottom: 0,
    width: 0,
    height: 0,
    ...bounds
  };
  return {
    tagName,
    dataset: {},
    style: {},
    isConnected: true,
    offsetWidth: 80,
    append() {},
    addEventListener() {},
    getAttribute() { return null; },
    getBoundingClientRect() {
      return rect;
    },
    setBounds(next) {
      Object.assign(rect, next);
    },
    querySelector() { return null; },
    remove() {
      this.isConnected = false;
      this.removed = true;
    },
    removeAttribute(name) {
      if (name === "data-quizpane-video-focus-target") {
        delete this.dataset.quizpaneVideoFocusTarget;
        return;
      }
      if (name === "data-quizpane-video-focus-video") {
        delete this.dataset.quizpaneVideoFocusVideo;
        return;
      }
      if (name === "data-quizpane-video-focus") {
        delete this.dataset.quizpaneVideoFocus;
        return;
      }
      delete this[name];
    },
    setAttribute(name, value) {
      this[name] = value;
    }
  };
}

function createContentScriptHarness({ videos = [], frames = [] } = {}) {
  const runtimeListeners = new Set();
  const documentListeners = [];
  const windowListeners = [];
  const observers = [];
  const clearedIntervals = [];
  const scheduledTimeouts = [];
  const body = createDomNode("BODY");
  body.append = node => {
    node.isConnected = true;
    body.lastAppended = node;
  };
  const documentElement = createDomNode("HTML");
  const head = createDomNode("HEAD");
  head.append = node => {
    node.isConnected = true;
    head.lastAppended = node;
  };
  const document = {
    body,
    documentElement,
    head,
    title: "测试课程",
    addEventListener: (name, handler, options) => {
      documentListeners.push({ name, handler, options, removed: false });
    },
    removeEventListener: (name, handler, options) => {
      const entry = documentListeners.find(item =>
        item.name === name && item.handler === handler && item.options === options);
      if (entry) entry.removed = true;
    },
    createElement: tagName => createDomNode(String(tagName).toUpperCase()),
    querySelectorAll: selector => {
      if (selector === "video") return videos;
      if (selector === "iframe") return frames;
      return [];
    }
  };
  class FakeMutationObserver {
    constructor(callback) {
      this.callback = callback;
      this.disconnected = false;
      observers.push(this);
    }
    observe() {}
    disconnect() {
      this.disconnected = true;
    }
  }
  const context = {
    chrome: {
      runtime: {
        onMessage: {
          addListener: listener => runtimeListeners.add(listener),
          removeListener: listener => runtimeListeners.delete(listener)
        },
        sendMessage: async () => ({})
      }
    },
    clearInterval: id => clearedIntervals.push(id),
    clearTimeout: id => {
      const timeout = scheduledTimeouts.find(item => item.id === id);
      if (timeout) timeout.cleared = true;
    },
    console,
    document,
    innerHeight: 720,
    innerWidth: 1280,
    MutationObserver: FakeMutationObserver,
    removeEventListener: (name, handler, options) => {
      const entry = windowListeners.find(item =>
        item.name === name && item.handler === handler
          && Boolean(typeof item.options === "object"
            ? item.options.capture
            : item.options) === Boolean(options));
      if (entry) entry.removed = true;
    },
    addEventListener: (name, handler, options) => {
      windowListeners.push({ name, handler, options, removed: false });
    },
    setInterval: () => 41,
    setTimeout: callback => {
      const id = scheduledTimeouts.length + 1;
      scheduledTimeouts.push({ id, callback, cleared: false });
      return id;
    }
  };
  vm.createContext(context);
  return {
    body,
    clearedIntervals,
    context,
    documentListeners,
    observers,
    runtimeListeners,
    scheduledTimeoutCount() {
      return scheduledTimeouts.length;
    },
    async flushTimeouts() {
      for (const timeout of scheduledTimeouts.splice(0)) {
        if (!timeout.cleared) await timeout.callback();
      }
    },
    windowListeners
  };
}

test("disposing a content controller releases resources and permits reinjection", async () => {
  const source = await contentScriptSource;
  assert.doesNotMatch(source, /requestPictureInPicture|quizpanePipButton|置顶播放/);
  const harness = createContentScriptHarness();
  vm.runInContext(source, harness.context);
  assert.equal(harness.runtimeListeners.size, 1);
  assert.equal(harness.observers.length, 1);
  assert.equal(harness.context.__quizpaneCourseController, true);

  const listener = [...harness.runtimeListeners][0];
  const response = await new Promise(resolve => {
    listener({ type: "command.dispose_controller" }, {}, resolve);
  });
  assert.equal(response.success, true);
  assert.equal(response.disposed, true);
  assert.equal(harness.runtimeListeners.size, 0);
  assert.equal(harness.observers[0].disconnected, true);
  assert.deepEqual(harness.clearedIntervals, [41]);
  assert.ok(harness.documentListeners.every(entry => entry.removed));
  assert.ok(harness.windowListeners.every(entry => entry.removed));
  assert.equal(harness.body.lastAppended, undefined);
  assert.equal("__quizpaneCourseController" in harness.context, false);

  vm.runInContext(source, harness.context);
  assert.equal(harness.runtimeListeners.size, 1);
  assert.equal(harness.observers.length, 2);
});

test("focus mode rejects a broad controls container that also contains non-video content", async () => {
  const source = await contentScriptSource;
  const video = createDomNode("VIDEO", {
    right: 600, bottom: 220, width: 600, height: 220
  });
  Object.assign(video, {
    readyState: 4,
    paused: false,
    ended: false,
    duration: 600,
    currentTime: 10,
    controls: false
  });
  const playerAndChat = createDomNode("DIV", {
    right: 600, bottom: 360, width: 600, height: 360
  });
  playerAndChat.querySelector = () => ({ kind: "custom-controls" });
  video.parentElement = playerAndChat;

  const harness = createContentScriptHarness({ videos: [video] });
  playerAndChat.parentElement = harness.body;
  vm.runInContext(source, harness.context);
  const listener = [...harness.runtimeListeners][0];
  const response = await new Promise(resolve => {
    listener({ type: "command.enter_focus_mode" }, {}, resolve);
  });

  assert.equal(response.success, true);
  assert.equal(response.target, "video");
  assert.equal(video.dataset.quizpaneVideoFocusTarget, "true");
  assert.equal(playerAndChat.dataset.quizpaneVideoFocusTarget, undefined);
});

test("focus mode activates when the video appears after the initial request", async () => {
  const source = await contentScriptSource;
  const videos = [];
  const harness = createContentScriptHarness({ videos });
  vm.runInContext(source, harness.context);
  const listener = [...harness.runtimeListeners][0];
  const enter = message => new Promise(resolve => listener(message, {}, resolve));

  const early = await enter({ type: "command.enter_focus_mode" });
  assert.equal(early.success, false);
  assert.equal(early.error, "player-not-found");

  const video = createDomNode("VIDEO", {
    right: 620, bottom: 340, width: 620, height: 340
  });
  Object.assign(video, {
    readyState: 4,
    paused: false,
    ended: false,
    duration: 600,
    currentTime: 10,
    controls: true,
    parentElement: harness.body
  });
  videos.push(video);

  const loadedMetadata = harness.documentListeners.find(entry =>
    entry.name === "loadedmetadata");
  loadedMetadata.handler();

  assert.equal(video.dataset.quizpaneVideoFocusTarget, "true");
});

test("focus mode keeps a selected compact controls shell after its focused layout expands", async () => {
  const source = await contentScriptSource;
  const video = createDomNode("VIDEO", {
    right: 600, bottom: 300, width: 600, height: 300
  });
  Object.assign(video, {
    readyState: 4, paused: false, ended: false, duration: 600,
    currentTime: 10, controls: false
  });
  const shell = createDomNode("DIV", {
    right: 600, bottom: 340, width: 600, height: 340
  });
  shell.querySelector = () => ({ kind: "custom-controls" });
  video.parentElement = shell;
  const harness = createContentScriptHarness({ videos: [video] });
  shell.parentElement = harness.body;
  vm.runInContext(source, harness.context);
  const listener = [...harness.runtimeListeners][0];
  const enter = () => new Promise(resolve =>
    listener({ type: "command.enter_focus_mode" }, {}, resolve));

  assert.equal((await enter()).target, "div");
  video.setBounds({ right: 1280, bottom: 720, width: 1280, height: 720 });
  shell.setBounds({ right: 1280, bottom: 720, width: 1280, height: 720 });
  assert.equal((await enter()).target, "div");
  assert.equal(shell.dataset.quizpaneVideoFocusTarget, "true");
});

test("focus mode keeps the selected course video when another video starts playing", async () => {
  const source = await contentScriptSource;
  const course = createDomNode("VIDEO", {
    right: 600, bottom: 320, width: 600, height: 320
  });
  Object.assign(course, {
    readyState: 4, paused: false, ended: false, duration: 600,
    currentTime: 10, controls: true
  });
  const advert = createDomNode("VIDEO", {
    right: 200, bottom: 100, width: 200, height: 100
  });
  Object.assign(advert, {
    readyState: 4, paused: true, ended: false, duration: 30,
    currentTime: 0, controls: true
  });
  const harness = createContentScriptHarness({ videos: [course, advert] });
  course.parentElement = harness.body;
  advert.parentElement = harness.body;
  vm.runInContext(source, harness.context);
  const listener = [...harness.runtimeListeners][0];
  const enter = () => new Promise(resolve =>
    listener({ type: "command.enter_focus_mode" }, {}, resolve));

  assert.equal((await enter()).target, "video");
  course.paused = true;
  advert.paused = false;
  await enter();
  assert.equal(course.dataset.quizpaneVideoFocusTarget, "true");
  assert.equal(advert.dataset.quizpaneVideoFocusTarget, undefined);
});

test("a hidden placeholder video does not block a visible iframe fallback", async () => {
  const source = await contentScriptSource;
  const placeholder = createDomNode("VIDEO");
  Object.assign(placeholder, {
    readyState: 4, paused: true, ended: false, duration: 30,
    currentTime: 0, controls: true
  });
  const frame = createDomNode("IFRAME", {
    right: 620, bottom: 400, width: 620, height: 400
  });
  const harness = createContentScriptHarness({ videos: [placeholder], frames: [frame] });
  placeholder.parentElement = harness.body;
  frame.parentElement = harness.body;
  vm.runInContext(source, harness.context);
  const listener = [...harness.runtimeListeners][0];
  const response = await new Promise(resolve =>
    listener({ type: "command.enter_focus_mode" }, {}, resolve));

  assert.equal(response.target, "iframe");
  assert.equal(placeholder.dataset.quizpaneVideoFocusTarget, undefined);
});

test("a compact shell marks only the selected main video for fullscreen sizing", async () => {
  const source = await contentScriptSource;
  const main = createDomNode("VIDEO", {
    right: 600, bottom: 300, width: 600, height: 300
  });
  Object.assign(main, {
    readyState: 4, paused: false, ended: false, duration: 600,
    currentTime: 10, controls: false
  });
  const preview = createDomNode("VIDEO", {
    right: 120, bottom: 80, width: 120, height: 80
  });
  Object.assign(preview, {
    readyState: 4, paused: true, ended: false, duration: 30,
    currentTime: 0, controls: false
  });
  const shell = createDomNode("DIV", {
    right: 600, bottom: 340, width: 600, height: 340
  });
  shell.querySelector = () => ({ kind: "custom-controls" });
  main.parentElement = shell;
  preview.parentElement = shell;
  const harness = createContentScriptHarness({ videos: [main, preview] });
  shell.parentElement = harness.body;
  vm.runInContext(source, harness.context);
  const listener = [...harness.runtimeListeners][0];
  await new Promise(resolve =>
    listener({ type: "command.enter_focus_mode" }, {}, resolve));

  assert.equal(main.dataset.quizpaneVideoFocusVideo, "true");
  assert.equal(preview.dataset.quizpaneVideoFocusVideo, undefined);
  assert.match(harness.context.document.head.lastAppended.textContent,
    /video\[data-quizpane-video-focus-video\]/);
  assert.doesNotMatch(harness.context.document.head.lastAppended.textContent,
    /:not\(video\) video/);
});

test("repeated DOM mutations share one pending focus reconciliation", async () => {
  const source = await contentScriptSource;
  const video = createDomNode("VIDEO", {
    right: 600, bottom: 320, width: 600, height: 320
  });
  Object.assign(video, {
    readyState: 4, paused: false, ended: false, duration: 600,
    currentTime: 10, controls: true
  });
  const harness = createContentScriptHarness({ videos: [video] });
  video.parentElement = harness.body;
  vm.runInContext(source, harness.context);
  const listener = [...harness.runtimeListeners][0];
  await new Promise(resolve =>
    listener({ type: "command.enter_focus_mode" }, {}, resolve));

  harness.observers[0].callback();
  harness.observers[0].callback();

  // One focus timer is shared. The existing status debounce replaces only its own timer.
  assert.equal(harness.scheduledTimeoutCount(), 3);
  await harness.flushTimeouts();
  assert.equal(video.dataset.quizpaneVideoFocusTarget, "true");
});
