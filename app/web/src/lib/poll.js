// Polling that never piles up: the next call starts `ms` after the last one ended, not on a clock; none while the page
// is hidden; slower, up to a minute, while failing() says the server is not answering.
export function poll(fn, ms, failing = () => false) {
  let stopped = false, timer = null, misses = 0;
  const schedule = () => {
    if (stopped) return;
    misses = failing() ? misses + 1 : 0;
    timer = setTimeout(tick, Math.min(ms * 2 ** misses, 60000));
  };
  async function tick() {
    if (document.hidden) return schedule();
    try {
      await fn();
    } catch { /* fn says what went wrong itself */ }
    schedule();
  }
  const visible = () => { if (!document.hidden && !stopped) { clearTimeout(timer); tick(); } };
  document.addEventListener("visibilitychange", visible);
  tick();
  return () => { stopped = true; clearTimeout(timer); document.removeEventListener("visibilitychange", visible); };
}
