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

/** A JSON request. A failure throws, including the backend's 200 answers with {status: "error"}. */
async function request(method, path, body) {
  const res = await fetch(url(path), body === undefined ? { method } : {
    method, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
  });
  if (!res.ok) {
    const why = await detail(res);
    // The public demo refuses what a visitor may not do, and holds computations to limits: say so plainly.
    if (res.status === 429 || res.headers.get("x-demo-refused")) window.alert(why || "Not available in the demo.");
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
