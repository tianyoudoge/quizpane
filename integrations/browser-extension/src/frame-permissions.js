function httpOriginPattern(url) {
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return null;
    return `${parsed.origin}/*`;
  } catch {
    return null;
  }
}

export async function requestCourseFramePermissions(chromeApi, tab) {
  const frames = await chromeApi.webNavigation.getAllFrames({ tabId: tab.id }) || [];
  const origins = [...new Set([
    httpOriginPattern(tab.url),
    ...frames.map(frame => httpOriginPattern(frame.url))
  ].filter(Boolean))];
  const requestedOrigins = [];
  for (const origin of origins) {
    if (!await chromeApi.permissions.contains({ origins: [origin] })) {
      requestedOrigins.push(origin);
    }
  }
  const granted = requestedOrigins.length === 0 ||
    await chromeApi.permissions.request({ origins: requestedOrigins });
  return { granted, origins, requestedOrigins };
}
