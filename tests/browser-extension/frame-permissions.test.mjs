import assert from "node:assert/strict";
import test from "node:test";

import {
  requestCourseFramePermissions
} from "../../integrations/browser-extension/src/frame-permissions.js";

test("binding requests the actual cross-origin frame permissions discovered in the tab", async () => {
  const requests = [];
  const chromeApi = {
    permissions: {
      contains: async ({ origins }) => origins[0] === "https://school.example/*",
      request: async permission => {
        requests.push(permission);
        return true;
      }
    },
    webNavigation: {
      getAllFrames: async ({ tabId }) => {
        assert.equal(tabId, 17);
        return [
          { frameId: 0, parentFrameId: -1, url: "https://school.example/course/42" },
          { frameId: 3, parentFrameId: 0, url: "https://player.vendor.test/playback/room" },
          { frameId: 4, parentFrameId: 0, url: "about:blank" }
        ];
      }
    }
  };

  const result = await requestCourseFramePermissions(chromeApi, {
    id: 17,
    url: "https://school.example/course/42"
  });

  assert.deepEqual(result, {
    granted: true,
    origins: ["https://school.example/*", "https://player.vendor.test/*"],
    requestedOrigins: ["https://player.vendor.test/*"]
  });
  assert.deepEqual(requests, [{ origins: ["https://player.vendor.test/*"] }]);
});
