const BRIDGE_URL = "ws://127.0.0.1:49752/quizpane-browser/v1";
const HEARTBEAT_MS = 20_000;
const RETRY_MS = 2_000;

export function protocolMessage(type, payload = {}, requestId = crypto.randomUUID()) {
  return { protocolVersion: 1, type, requestId, timestamp: Date.now(), payload };
}

export function createBridgeClient({
  WebSocketImpl = WebSocket,
  browserName,
  onConnected,
  onExternalWindowAttached,
  onCommand,
  setTimeoutImpl = setTimeout,
  setIntervalImpl = setInterval,
  clearIntervalImpl = clearInterval
}) {
  let socket = null;
  let heartbeat = null;
  let retryTimer = null;

  function send(type, payload = {}, requestId) {
    if (socket?.readyState !== WebSocketImpl.OPEN) return false;
    socket.send(JSON.stringify(protocolMessage(type, payload, requestId)));
    return true;
  }

  function scheduleReconnect() {
    if (retryTimer) return;
    retryTimer = setTimeoutImpl(() => {
      retryTimer = null;
      connect();
    }, RETRY_MS);
  }

  function connect() {
    if (socket?.readyState === WebSocketImpl.OPEN
        || socket?.readyState === WebSocketImpl.CONNECTING) return;
    try {
      socket = new WebSocketImpl(BRIDGE_URL);
    } catch {
      scheduleReconnect();
      return;
    }
    socket.onopen = () => {
      send("hello", {
        client: "quizpane-browser-extension",
        extensionVersion: "0.1.0",
        browser: browserName()
      });
      clearIntervalImpl(heartbeat);
      heartbeat = setIntervalImpl(() => send("ping"), HEARTBEAT_MS);
    };
    socket.onmessage = async event => {
      let incoming;
      try {
        incoming = JSON.parse(event.data);
      } catch {
        return;
      }
      if (incoming.protocolVersion !== 1) return;
      if (incoming.type === "hello_ack") {
        await onConnected();
        return;
      }
      if (incoming.type === "externalWindow.attached") {
        await onExternalWindowAttached(incoming.payload || {});
        return;
      }
      if (!incoming.type?.startsWith("command.")) return;
      const result = await onCommand(incoming.type, incoming.payload || {});
      send("result", result, incoming.requestId);
    };
    socket.onclose = () => {
      clearIntervalImpl(heartbeat);
      heartbeat = null;
      socket = null;
      scheduleReconnect();
    };
    socket.onerror = () => socket?.close();
  }

  return {
    connect,
    send,
    isConnected: () => socket?.readyState === WebSocketImpl.OPEN
  };
}
