// Tools: every tool an agent can call (GET /api/tools). The sidebar lists them in the order of the research flow (write
// a strategy, backtest it, promote it), then the calculators; the one chosen is shown whole: what it does, each input,
// an example and the call.
import { signal } from "@preact/signals";
import { get } from "../lib/api.js";
import { params, setParam } from "../lib/store.js";
import { Badge, Card, ErrorText, Note } from "../ui/blocks.jsx";
import { Group, Line, Text } from "../ui/controls.jsx";
import { toast } from "../ui/toast.jsx";

const listing = signal(null);  // { tools: [...] } or { error }
const busy = signal(false);

// What each tool is for, by its route under /api; the research flow first.
const GROUPS = [
  ["1 · Write a strategy", /^\/api\/backtest\/strategies/],
  ["2 · Backtest", /^\/api\/(backtest(\/|$)|experiment)/],
  ["3 · Promote", /^\/api\/strategy_versions/],
  ["Pricing and Greeks", /^\/api\/lab\/(pricing|scenario|pde|convergence|benchmark|greek)/],
  ["Calibration", /^\/api\/lab\/calibration/],
  ["Risk and hedging", /^\/api\/(lab\/(stress|hedging)|risk)/],
  ["Market data", /^\/api\/lab\/(market|screener)/],
  ["Theory checks", /^\/api\/lab\/(stats|ito|simulation|measure|validation)/],
  ["Run history", /^\/api\/runs/],
];
const groupOf = (tool) => GROUPS.find(([, re]) => re.test(tool.http.path))?.[0] ?? "Other";

async function load() {
  busy.value = true;
  try {
    listing.value = { tools: (await get("api/tools")).tools };
  } catch (err) {
    listing.value = { error: err.message };
  } finally {
    busy.value = false;
  }
}

const allTools = () => listing.value?.tools ?? [];
const chosen = () => allTools().find((t) => t.name === params.value.toolsOpen) ?? allTools()[0];

function Params() {
  const tools = allTools(), groups = new Set(tools.map(groupOf)).size;
  return (
    <>
      <Group title="Driven by an agent">
        <Line><b class="font-medium text-ink">Everything this page does, an AI agent can do too.</b> Writing and building a
          strategy, backtests, experiments, promotion, pricing and risk are each a tool: an HTTP call with a described
          schema. The page and an agent call the same tools.</Line>
        <Line>{tools.length ? `${tools.length} tools in ${groups} groups` : "The tools"}, listed by GET /api/tools: choose one
          on the right for what it does, its inputs, an example and the call to copy.</Line>
        <Line>An agent researches freely; what it writes reaches a trading account only once a person approves it.</Line>
      </Group>
      <Group title="Find">
        <Text k="toolsFilter" label="" placeholder="iv, heston, overfitting…" />
      </Group>
      <Group title="How an agent researches">
        <Line><b class="font-medium text-ink">1 · Write a strategy.</b> strategy_api gives the rules and a template;
          put_strategy builds the file and returns its parameters.</Line>
        <Line><b class="font-medium text-ink">2 · Backtest it.</b> backtest runs one setting or a space of them, counts
          every try in the family and answers with the measures, the control, the deflated Sharpe and what review asks.</Line>
        <Line><b class="font-medium text-ink">3 · Promote it.</b> register_strategy_version, then
          promote_strategy_version: the gates are checked; paper and beyond need a person.</Line>
        <Line>The rest are the calculators the strategies and the lab are built on.</Line>
      </Group>
    </>
  );
}

function Catalog({ tools, open }) {
  const groups = GROUPS.map(([g]) => g).concat("Other")
    .map((g) => [g, tools.filter((t) => groupOf(t) === g)]).filter(([, rows]) => rows.length);
  return (
    <section class="col-span-full">
      <div class="grid gap-x-8 gap-y-5 md:grid-cols-2 xl:grid-cols-3">
        {groups.map(([g, rows]) => (
          <div class="min-w-0">
            <h3 class="flex items-baseline justify-between border-b border-line pb-1.5 text-2xs font-semibold tracking-[0.08em] text-accent uppercase">
              {g}<span class="font-normal tracking-normal text-muted">{rows.length}</span>
            </h3>
            <div class="flex flex-wrap gap-1 pt-2">
              {rows.map((t) => (
                <button type="button" onClick={() => setParam("toolsOpen", t.name)} title={t.title}
                  class={`rounded-sm px-1.5 font-mono text-[11px] leading-5 transition-colors ${t.name === open
                    ? "bg-accent text-accent-ink" : "bg-surface-2 text-ink-2 hover:text-accent"}`}>
                  {t.name}
                </button>
              ))}
            </div>
          </div>
        ))}
      </div>
    </section>
  );
}

const COST = { instant: ["good", "instant"], seconds: ["muted", "seconds"], heavy: ["warn", "minutes"] };

function kind(p) {
  if (p.enum) return p.enum.join(" | ");
  if (p.anyOf) return p.anyOf.map(kind).filter((k) => k !== "null").join(" or ");
  if (p.type === "array") return `list of ${p.items ? kind(p.items) : "values"}`;
  return p.type ?? "any";
}

function range(p) {
  const lo = p.minimum ?? p.exclusiveMinimum, hi = p.maximum ?? p.exclusiveMaximum;
  if (lo == null && hi == null) return "";
  return `${lo ?? "…"} to ${hi ?? "…"}`;
}

const callOf = (t) => {
  const example = t.input_schema?.examples?.[0] ?? {};
  let path = t.http.path;
  const body = { ...example };
  for (const [k, v] of Object.entries(example)) {
    if (path.includes(`{${k}}`)) { path = path.replace(`{${k}}`, v); delete body[k]; }
  }
  const url = `http://127.0.0.1:8080${path}`;
  if (t.http.method === "GET") {
    const q = new URLSearchParams(Object.entries(body).map(([k, v]) => [k, typeof v === "object" ? JSON.stringify(v) : v]));
    return `curl -s '${url}${q.size ? `?${q}` : ""}'`;
  }
  return `curl -s -X ${t.http.method} ${url} -H 'content-type: application/json' -d '${JSON.stringify(body)}'`;
};

function Results() {
  const l = listing.value;
  if (l?.error) return <Card title="Tools" wide><ErrorText>{l.error}</ErrorText></Card>;
  const t = chosen();
  if (!t) return <Card title="Tools" wide><Note>Loading the tools…</Note></Card>;
  const words = (params.value.toolsFilter ?? "").toLowerCase().trim();
  const shown = allTools().filter((x) => !words || `${x.name} ${x.title} ${x.description}`.toLowerCase().includes(words));
  const schema = t.input_schema ?? {};
  const required = new Set(schema.required ?? []);
  const inputs = Object.entries(schema.properties ?? {}).map(([name, p]) => ({
    name, kind: kind(p), default: required.has(name) ? "required" : JSON.stringify(p.default ?? null),
    range: range(p) && `range ${range(p)}`, about: p.description ?? "",
  }));
  const [costKind, costLabel] = COST[t.cost] ?? COST.instant;
  return (
    <>
    <Catalog tools={shown} open={t.name} />
    <article class="col-span-full overflow-hidden rounded-md border border-line bg-surface shadow-[var(--shadow)]">
      <header class="flex flex-col gap-2 px-6 pt-6 sm:px-8">
        <p class="text-2xs font-semibold tracking-[0.08em] text-accent uppercase">{groupOf(t)}</p>
        <h2 class="text-xl font-semibold tracking-tight">{t.title}</h2>
        <div class="flex flex-wrap items-center gap-2 text-sm">
          <code class="rounded-sm bg-surface-2 px-1.5 py-0.5 font-mono text-[13px]">{t.name}</code>
          <code class="rounded-sm bg-surface-2 px-1.5 py-0.5 font-mono text-[13px] text-ink-2">{t.http.method} {t.http.path}</code>
          <Badge kind={costKind}>{costLabel}</Badge>
          {t.network && <Badge>network</Badge>}
          {t.who === "person" && <Badge kind="warn">the person's</Badge>}
        </div>
      </header>

      <div class="px-6 pb-7 sm:px-8">
      <p class="mt-4 max-w-3xl text-[15px] leading-7 text-ink-2">{t.description}</p>

      <section class="mt-7 border-t border-line pt-6">
        <h3 class="mb-3 text-2xs font-semibold tracking-[0.08em] text-accent uppercase">Inputs</h3>
        {inputs.length ? (
          <div class="overflow-x-auto">
            <table class="w-full border-collapse text-sm">
              <thead class="border-b border-line-strong text-left text-xs text-muted">
                <tr><th class="py-2 pr-3 font-medium">Name</th><th class="px-3 py-2 font-medium">Type</th>
                  <th class="px-3 py-2 font-medium">Default</th><th class="py-2 pl-3 font-medium">What it is</th></tr>
              </thead>
              <tbody>
                {inputs.map((i) => (
                  <tr class="border-t border-line align-top">
                    <td class="py-2.5 pr-3 font-mono text-[13px] whitespace-nowrap">{i.name}</td>
                    <td class="px-3 py-2.5 text-xs text-muted">{i.kind}{i.range && <div class="mt-0.5">{i.range}</div>}</td>
                    <td class="px-3 py-2.5 font-mono text-xs whitespace-nowrap">
                      {i.default === "required" ? <Badge kind="warn">required</Badge> : i.default}</td>
                    <td class="min-w-72 py-2.5 pl-3 leading-6 text-ink-2">{i.about}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        ) : <p class="text-sm text-muted">None.</p>}
      </section>

      <section class="mt-7 grid gap-6 border-t border-line pt-6 xl:grid-cols-2">
        <div class="min-w-0">
          <h3 class="mb-3 text-2xs font-semibold tracking-[0.08em] text-accent uppercase">Example input</h3>
          <pre class="max-h-80 overflow-auto rounded-md bg-surface-2 p-4 font-mono text-[13px] leading-6">{JSON.stringify(schema.examples?.[0] ?? {}, null, 2)}</pre>
        </div>
        <div class="min-w-0">
          <h3 class="mb-3 text-2xs font-semibold tracking-[0.08em] text-accent uppercase">The call</h3>
          <pre class="overflow-auto rounded-md bg-surface-2 p-4 font-mono text-[13px] leading-6 break-all whitespace-pre-wrap">{callOf(t)}</pre>
          <p class="mt-3 text-sm leading-6 text-muted">Every tool answers in one envelope: <code>status</code>,
            <code> result_summary</code>, <code>result_details</code>, <code>warnings</code>, <code>diagnostics</code>.
            Each run is kept in the history (<code>GET /api/runs</code>).</p>
        </div>
      </section>
      </div>
    </article>
    </>
  );
}

async function copyCall() {
  const t = chosen();
  if (!t) return;
  await navigator.clipboard.writeText(callOf(t));
  toast(`${t.name}: the call is on the clipboard.`, "Copied");
}

export const tools = {
  id: "tools",
  label: "Tools",
  about: "Every tool an agent can call, in the order of the research flow: what each does, its inputs, an example and the call.",
  enter: async () => { if (!listing.value) await load(); },
  Params,
  Results,
  hasResults: () => listing.value != null,
  status: () => "",  // the count is in the sidebar
  action: { label: "Copy the call", busyLabel: "Loading…", busy, run: copyCall },
};
