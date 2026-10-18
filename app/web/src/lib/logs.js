// The log: the backend streams the engine's lines over a WebSocket, sending the recent ones first. The last
// 500 are kept.
import { signal } from "@preact/signals";
import { post, socketUrl } from "./api.js";

const MAX = 500;
export const lines = signal([]);  // { time, level, source, msg }
export const connected = signal(false);

function parse(raw) {
  try {
    const o = JSON.parse(raw);  // the C++ engine's lines are JSON
    if (o?.msg != null) {
      return { time: o.time ?? "", level: String(o.level_str ?? "INFO").toUpperCase(), source: o.source || (o.src === "live" ? "Live" : ""), msg: o.msg };
    }
  } catch { /* any other line is shown as it is */ }
  return { time: "", level: "INFO", source: "", msg: raw };
}

let socket = null;
let retry = 1000;
let pending = [], flush = null;

export function connectLogs() {
  if (socket) return;
  socket = new WebSocket(socketUrl("ws/logs"));
  socket.onopen = () => {
    connected.value = true;
    retry = 1000;
    lines.value = [];  // the backend sends its recent lines again
  };
  socket.onmessage = (e) => {
    pending.push(parse(String(e.data)));
    flush ??= requestAnimationFrame(() => {  // a burst of lines is one update of the list
      lines.value = [...lines.value, ...pending].slice(-MAX);
      pending = [];
      flush = null;
    });
  };
  socket.onclose = () => {
    connected.value = false;
    socket = null;
    setTimeout(connectLogs, retry);
    retry = Math.min(retry * 2, 30000);
  };
}

export async function clearLogs() {
  await post("api/logs/clear");
  lines.value = [];
}
