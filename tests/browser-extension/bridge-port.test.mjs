import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const extensionSource = new URL(
  "../../integrations/browser-extension/src/bridge-client.js",
  import.meta.url
);
const desktopHeader = new URL(
  "../../apps/desktop-qt/src/browser/browser_bridge.hpp",
  import.meta.url
);

test("desktop and extension share a bridge port outside the Windows dynamic range", async () => {
  const [extension, desktop] = await Promise.all([
    readFile(extensionSource, "utf8"),
    readFile(desktopHeader, "utf8")
  ]);
  const extensionPort = Number(extension.match(/127\.0\.0\.1:(\d+)/)?.[1]);
  const desktopPort = Number(desktop.match(/kBridgePort\s*=\s*(\d+)/)?.[1]);

  assert.equal(extensionPort, desktopPort);
  assert.ok(desktopPort > 1024);
  assert.ok(desktopPort < 49152);
});
