import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

function createDomNode() {
  return {
    dataset: {},
    style: {},
    isConnected: true,
    offsetWidth: 80,
    append() {},
    addEventListener() {},
    getAttribute() { return null; },
    getBoundingClientRect() {
      return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 };
    },
    querySelector() { return null; },
    remove() {
      this.isConnected = false;
      this.removed = true;
    },
    removeAttribute(name) {
      delete this[name];
    },
    setAttribute(name, value) {
      this[name] = value;
    }
  };
}

function createContentScriptHarness() {
  const runtimeListeners = new Set();
  const documentListeners = [];
  const windowListeners = [];
  const observers = [];
  const clearedIntervals = [];
  const body = createDomNode();
  body.append = node => {
    node.isConnected = true;
    body.lastAppended = node;
  };
  const documentElement = createDomNode();
  const head = createDomNode();
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
    createElement: () => createDomNode(),
    querySelectorAll: () => []
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
    clearTimeout() {},
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
    setTimeout: () => 42
  };
  vm.createContext(context);
  return {
    body,
    clearedIntervals,
    context,
    documentListeners,
    observers,
    runtimeListeners,
    windowListeners
  };
}

test("disposing a content controller releases resources and permits reinjection", async () => {
  const source = await readFile(
    new URL("../../integrations/browser-extension/src/content-script.js", import.meta.url),
    "utf8"
  );
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
  assert.equal(harness.windowListeners.length, 0);
  assert.equal(harness.body.lastAppended, undefined);
  assert.equal("__quizpaneCourseController" in harness.context, false);

  vm.runInContext(source, harness.context);
  assert.equal(harness.runtimeListeners.size, 1);
  assert.equal(harness.observers.length, 2);
});
