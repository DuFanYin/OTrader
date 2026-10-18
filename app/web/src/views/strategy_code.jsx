// A strategy's C++ pasted on the page and built (the put_strategy tool: strategy/research/<name>.cpp, built alone),
// then, on the person's word, installed for the paper account (install_strategy), whose engine loads it the next
// time it lists the classes. Backtests and experiments load it as soon as it builds. The public demo shows it too,
// read only: it builds no code a visitor sends.
import { signal } from "@preact/signals";
import { post } from "../lib/api.js";
import { loadCatalog } from "../lib/catalog.js";
import { demo } from "../lib/engine.js";
import { Badge } from "../ui/blocks.jsx";
import { Group, Line } from "../ui/controls.jsx";
import { Button, Dialog, Summary } from "../ui/dialog.jsx";

const TEMPLATE = `// A strategy: its class, its parameters (shown on the page, varied by experiments) and its callbacks. What it is
// written against: the Tools tab, "How to write a strategy".
#include "strategy.hpp"
#include "strategy_registry.hpp"

namespace strategy {

class MyStrategy final : public Strategy {
  public:
    struct Setting {
        Param<int> quantity{"quantity", 1, 1, 10};
    };
    using Strategy::Strategy;

    void on_day() override {}    // each trading day: choose its chain (select_chain)
    void on_timer() override {}  // every bar: read quotes and Greeks, send orders

  private:
    const Setting setting_ = read_setting<Setting>();
};

REGISTER_STRATEGY(MyStrategy);

} // namespace strategy
`;

const open = signal(false);
const name = signal("my_strategy");
const source = signal(TEMPLATE);
// idle | building | built { classes } | failed { error } | installing | installed { installed }
const state = signal({ kind: "idle" });

const NAME = /^[a-z][a-z0-9_]{0,63}$/;

async function build() {
  state.value = { kind: "building" };
  try {
    const d = await post("api/backtest/strategies/source", { name: name.value, source: source.value });
    state.value = { kind: "built", classes: d.result_summary?.classes ?? [] };
    loadCatalog();  // the new classes in the lists
  } catch (err) {
    state.value = { kind: "failed", error: err.message };
  }
}

async function install() {
  state.value = { ...state.value, kind: "installing" };
  try {
    const d = await post("api/backtest/strategies/install",
      { name: name.value, account: "paper", approved: "the owner, from the Home page" });
    state.value = { ...state.value, kind: "installed", installed: d.result_summary?.installed };
    loadCatalog();
  } catch (err) {
    state.value = { kind: "failed", error: err.message };
  }
}

function Outcome() {
  const s = state.value;
  if (s.kind === "building") return <p class="text-sm text-muted">Building strategy/research/{name.value}.cpp… (about half a minute)</p>;
  if (s.kind === "failed")
    return <pre class="max-h-56 overflow-auto rounded-sm bg-surface-2 p-2 font-mono text-xs leading-snug whitespace-pre-wrap text-down">{s.error}</pre>;
  if (!["built", "installing", "installed"].includes(s.kind)) return null;
  return (
    <div class="flex flex-col gap-1.5 text-sm">
      <p><Badge kind="good">built</Badge>{" "}
        {s.classes.length ? <>registered {s.classes.map((c) => c.name).join(", ")}: ready for Backtest and Experiments.</>
          : <>but it registered no class: REGISTER_STRATEGY(ClassName) at the end of the file.</>}</p>
      {s.classes.map((c) => (
        <p class="text-xs text-muted">{c.name}: {c.parameters?.length ? c.parameters.map((p) => p.name).join(", ") : "no parameters"}</p>
      ))}
      {s.kind === "installed"
        ? <p><Badge kind="good">installed</Badge> {s.installed}: pick it under New strategy to trade it on the paper account.</p>
        : <p class="text-xs text-muted">To trade it here, install it for the paper account: its engine loads it then.</p>}
    </div>
  );
}

function Editor() {
  const s = state.value, bad = !NAME.test(name.value);
  const working = s.kind === "building" || s.kind === "installing";
  return (
    <Dialog open={open.value} onClose={() => (open.value = false)} title="Your own strategy" width="880px"
      footer={<>
        {["built", "installing"].includes(s.kind) && s.classes.length > 0 &&
          <Button onClick={install} disabled={demo.value}>{s.kind === "installing" ? "Installing…" : "Install for paper"}</Button>}
        <Button primary onClick={build} disabled={demo.value || bad || working}
          title={demo.value ? "The public demo builds no code a visitor sends" : undefined}>
          {s.kind === "building" ? "Building…" : "Build"}</Button>
      </>}>
      <div class="flex flex-col gap-3">
        <label class="flex items-center gap-3 text-sm">
          <span class="text-muted">File</span>
          <span class="font-mono text-xs text-muted">strategy/research/</span>
          <input class={`h-7 w-56 rounded-sm border bg-surface px-2 font-mono text-xs ${bad ? "border-down" : "border-line"}`}
            value={name.value} spellcheck={false} disabled={working}
            onInput={(e) => (name.value = e.currentTarget.value)} />
          <span class="font-mono text-xs text-muted">.cpp</span>
          {bad && <span class="text-xs text-down">lower-case letters, digits and _</span>}
        </label>
        <textarea rows={22} spellcheck={false} disabled={working}
          class="w-full resize-y rounded-sm border border-line bg-surface p-2 font-mono text-xs leading-snug"
          value={source.value} onInput={(e) => (source.value = e.currentTarget.value)} />
        {demo.value && <p class="text-xs text-muted">The public demo builds no code a visitor sends: the editor is here to
          look at.</p>}
        <Outcome />
      </div>
    </Dialog>
  );
}

/** The sidebar's group: what it does, and the editor it opens. */
export function WriteStrategy() {
  return (
    <Group title="Your own strategy">
      <Line>Paste a strategy's C++ and build it: it is ready for backtests and experiments at once, and trades here once
        installed for the paper account.</Line>
      <Summary action="Open" onOpen={() => (open.value = true)}>
        <span class="font-mono text-xs">strategy/research/{name.value}.cpp</span>
      </Summary>
      <Editor />
    </Group>
  );
}
