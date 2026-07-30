let stream = null;
let video = null;
let audioContext = null;
let frameCount = 0;
let lastFrameCount = 0;
let frameCallbackActive = false;

function send(event, payload = {}) {
  chrome.runtime.sendMessage({
    type: "tab-capture-keeper.event",
    payload: { event, ...payload }
  }).catch(() => {});
}

function stopStream() {
  if (video) {
    video.pause();
    video.srcObject = null;
    video = null;
  }
  if (stream) {
    for (const track of stream.getTracks()) track.stop();
    stream = null;
  }
  if (audioContext) {
    audioContext.close().catch(() => {});
    audioContext = null;
  }
  frameCallbackActive = false;
  frameCount = 0;
  lastFrameCount = 0;
}

function monitorFrames() {
  if (!video || !frameCallbackActive) return;
  video.requestVideoFrameCallback(() => {
    frameCount += 1;
    monitorFrames();
  });
}

async function startStream({ streamId, tabId }) {
  stopStream();
  stream = await navigator.mediaDevices.getUserMedia({
    audio: { mandatory: { chromeMediaSource: "tab", chromeMediaSourceId: streamId } },
    video: { mandatory: { chromeMediaSource: "tab", chromeMediaSourceId: streamId } }
  });
  // tabCapture 默认截走 tab 的声音；本地接回默认音频输出。不会写文件或联网。
  audioContext = new AudioContext();
  audioContext.createMediaStreamSource(stream).connect(audioContext.destination);
  video = document.createElement("video");
  video.muted = true;
  video.srcObject = stream;
  await video.play();
  frameCallbackActive = typeof video.requestVideoFrameCallback === "function";
  if (frameCallbackActive) monitorFrames();
  send("active", {
    tabId,
    videoTracks: stream.getVideoTracks().length,
    audioTracks: stream.getAudioTracks().length,
    frameCallbackActive
  });
}

setInterval(() => {
  if (!stream) return;
  const videoTrack = stream.getVideoTracks()[0];
  const audioTrack = stream.getAudioTracks()[0];
  const framesPerSecond = frameCount - lastFrameCount;
  lastFrameCount = frameCount;
  send("health", {
    videoState: videoTrack?.readyState || "missing",
    audioState: audioTrack?.readyState || "missing",
    framesPerSecond
  });
}, 1000);

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type === "tab-capture-keeper.start") {
    startStream(message.payload || {}).then(
      () => sendResponse({ success: true }),
      error => {
        send("start-failed", { error: String(error) });
        sendResponse({ success: false, error: String(error) });
      }
    );
    return true;
  }
  if (message?.type === "tab-capture-keeper.stop") {
    stopStream();
    send("stopped");
    sendResponse({ success: true });
    return false;
  }
  return false;
});
