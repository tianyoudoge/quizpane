const RELEASE_API_URL = "https://api.github.com/repos/tianyoudoge/quizpane/releases/latest";
const EXTENSION_ASSET = "QuizPane-course-companion.zip";
export const UPDATE_ALARM = "quizpane-extension-update-check";
const UPDATE_CHECK_PERIOD_MINUTES = 360;
const UPDATE_CHECK_CACHE_MS = 60 * 60 * 1000;

export function numericVersion(value) {
  const match = String(value || "").match(/^v?(\d+(?:\.\d+){0,3})$/);
  return match ? match[1].split(".").map(Number) : null;
}

export function compareVersions(left, right) {
  const a = numericVersion(left);
  const b = numericVersion(right);
  if (!a || !b) return 0;
  const length = Math.max(a.length, b.length);
  for (let index = 0; index < length; index += 1) {
    const delta = (a[index] || 0) - (b[index] || 0);
    if (delta) return delta > 0 ? 1 : -1;
  }
  return 0;
}

export function createUpdateChecker({ chromeApi, fetchImpl = fetch, now = Date.now }) {
  async function storedUpdateInfo() {
    return (await chromeApi.storage.local.get("updateInfo")).updateInfo || null;
  }

  async function checkForUpdate({ force = false } = {}) {
    const cached = await storedUpdateInfo();
    if (!force && cached?.checkedAt && now() - cached.checkedAt < UPDATE_CHECK_CACHE_MS) {
      return cached;
    }
    try {
      const response = await fetchImpl(RELEASE_API_URL, {
        cache: "no-store",
        headers: { Accept: "application/vnd.github+json" }
      });
      if (!response.ok) throw new Error(`release-api-${response.status}`);
      const release = await response.json();
      const latestVersion = numericVersion(release.tag_name)
        ? release.tag_name.replace(/^v/, "")
        : null;
      const hasExtension = Array.isArray(release.assets)
        && release.assets.some(asset => asset.name === EXTENSION_ASSET);
      const currentVersion = chromeApi.runtime.getManifest().version;
      const updateInfo = {
        available: Boolean(
          latestVersion && hasExtension && compareVersions(latestVersion, currentVersion) > 0
        ),
        latestVersion,
        currentVersion,
        releaseUrl: typeof release.html_url === "string" ? release.html_url : null,
        checkedAt: now()
      };
      await chromeApi.storage.local.set({ updateInfo });
      return updateInfo;
    } catch {
      return cached || {
        available: false,
        currentVersion: chromeApi.runtime.getManifest().version,
        checkedAt: null
      };
    }
  }

  function scheduleUpdateChecks() {
    chromeApi.alarms.create(UPDATE_ALARM, {
      periodInMinutes: UPDATE_CHECK_PERIOD_MINUTES
    });
  }

  return { checkForUpdate, scheduleUpdateChecks, storedUpdateInfo };
}
