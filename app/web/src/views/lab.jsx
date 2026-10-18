// Lab: the hands-on calculators (the screener, pricing, multi-leg, risk, the numerics, validation) in one view, a row
// of tabs at the top of its inputs choosing which. The Lab stands for the one chosen (inner), so its inputs, results,
// sample and button are that view's.
import { settings, setSetting } from "../lib/store.js";
import { showSample } from "../lib/samples.js";
import { Segmented } from "../ui/controls.jsx";

/** The calculators, in order; set by views/index.js (they are research views made there). */
export const calculators = [];

const chosen = () => calculators.find((v) => v.id === settings.value.labView) ?? calculators[0];

function choose(id) {
  const view = calculators.find((v) => v.id === id);
  showSample(view);
  setSetting("labView", id);
  view.enter().catch(() => {});
}

function Params() {
  const view = chosen();
  return (
    <>
      <div class="border-b border-line p-3">
        <Segmented size="xs" value={view.id} onChange={choose}
          options={calculators.map((v) => [v.id, v.label, v.about])} />
      </div>
      <view.Params />
    </>
  );
}

export const lab = {
  id: "lab",
  label: "Lab",
  about: "The calculators an agent's tools are made of, to try by hand: the screener, pricing, multi-leg, risk, the "
    + "numerics and validation.",
  /** The calculator chosen: the view the Lab stands for. */
  inner: chosen,
  enter: () => chosen().enter(),
  Params,
  get Results() { return chosen().Results; },
  hasResults: () => chosen().hasResults(),
  status: () => chosen().status(),
  get action() { return chosen().action; },
};
