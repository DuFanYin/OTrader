// The server's API, relative to the page (it runs under any path prefix): api/... for the trading routes, api/lab/...
// for the lab's tools, api/backtest/... and api/runs/... beside them.
const url = (path) => new URL(path, document.baseURI).href;

async function detail(res) {
  try {
    const d = await res.json();
    return typeof d?.detail === "string" ? d.detail : JSON.stringify(d?.detail ?? d);
  } catch {
    return "";
  }
}

/** What a view says while the engine it reads is not answering (the server's 503): a state, not a fault. */
export const ENGINE_DOWN = "The engine is not answering: it is starting, restarting or stopped. This fills in once it is back.";

const READ_SECONDS = 30;  // a read the server has not answered by then has failed; an action (a run) takes its time

/** A JSON request. A failure throws, including the backend's 200 answers with {status: "error"}. */
async function request(method, path, body) {
  const signal = method === "GET" ? AbortSignal.timeout(READ_SECONDS * 1000) : undefined;
  let res;
  try {
    res = await fetch(url(path), body === undefined ? { method, signal } : {
      method, signal, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
    });
  } catch (err) {
    if (err?.name === "TimeoutError") throw new Error(`the server did not answer in ${READ_SECONDS} s (${method} ${path.split("?")[0]})`);
    throw err;
  }
  if (!res.ok) {
    const why = await detail(res);
    // The public demo refuses what a visitor may not do, and holds computations to limits: say so plainly.
    if (res.status === 429 || res.headers.get("x-demo-refused")) window.alert(why || "Not available in the demo.");
    if (res.status === 503) throw Object.assign(new Error(ENGINE_DOWN), { detail: why });
    throw new Error(`${res.status}${why ? `: ${why}` : ""} (${method} ${path.split("?")[0]})`);
  }
  const data = await res.json();
  if (data?.status === "error") throw new Error(data.error ?? data.message ?? `${method} ${path.split("?")[0]} failed`);
  return data;
}

export const get = (path) => request("GET", path);
export const post = (path, body) => request("POST", path, body);
export const del = (path) => request("DELETE", path);

/** The WebSocket URL of a backend path (wss: on an https page). */
export const socketUrl = (path) => url(path).replace(/^http/, "ws");

/** A Server-Sent Events route: each event's data, parsed, to onData; an "unchanged" event to onUnchanged; failed() once
 * the stream cannot be had (the server refused it; a dropped connection the browser opens again by itself). Returns
 * what closes it. */
export function follow(path, { onData, onUnchanged = () => {}, failed }) {
  const source = new EventSource(url(path));
  source.onmessage = (e) => onData(JSON.parse(e.data));
  source.addEventListener("unchanged", onUnchanged);
  source.onerror = () => { if (source.readyState === EventSource.CLOSED) failed(); };
  return () => source.close();
}
