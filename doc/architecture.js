/* The architecture diagram's boxes made live: a box clicked (or chosen with Enter) keeps what it
   touches lit and says what it does. Without this script the page is the same diagram, still. */
(() => {
  "use strict";
  const svg = document.getElementById("arch");
  if (!svg) return;
  const root = document.documentElement;
  const GROUP = { control: "--control", core: "--core", infra: "--infra", lab: "--lab" };
  const groupOf = (el) => Object.keys(GROUP).find((k) => el.classList.contains(k));

  // Each arrow is its own edge: lit or dark with the labels that name it.
  for (const path of svg.querySelectorAll("path[data-kind]")) {
    path.classList.add("edge");
    path.dataset.edge = path.id;
  }
  root.classList.add("arch-live");

  // ---- A box clicked: it, its arrows and the boxes at their other ends stay; a card in the
  // diagram's empty corner says what it does.
  const nodes = [...svg.querySelectorAll("g.node")];
  const touching = new Map();
  const near = (id) => {
    if (!touching.has(id)) touching.set(id, { edges: new Set(), nodes: new Set([id]) });
    return touching.get(id);
  };
  for (const path of svg.querySelectorAll("path[data-kind]")) {
    const { from, to } = path.dataset;
    for (const [a, b] of [[from, to], [to, from]]) {
      near(a).edges.add(path.id);
      near(a).nodes.add(b);
    }
  }
  const tip = document.getElementById("arch-tip");
  const frame = svg.parentElement;
  let lit = null;

  const show = (node) => {
    const group = groupOf(node);
    tip.style.setProperty("--accent", `var(${group ? GROUP[group] : "--line"})`);
    const title = document.createElement("b");
    title.append(document.createElement("i"), node.getAttribute("aria-label"));
    const text = document.createElement("span");
    text.textContent = node.querySelector("desc")?.textContent ?? "";
    tip.replaceChildren(title, text);
    tip.hidden = false;
    const outer = frame.getBoundingClientRect();
    const scale = outer.width / 1100; // the viewBox is 1100 wide
    if (frame.parentElement.clientWidth >= frame.offsetWidth - 1) {
      tip.style.maxWidth = `${380 * scale}px`; // the empty corner, between the actors
      tip.style.left = `${410 * scale}px`;
      tip.style.top = `${20 * scale}px`;
      return;
    }
    tip.style.maxWidth = ""; // a narrow screen: by the box, below it or above
    const box = node.getBoundingClientRect();
    const left = Math.max(8, Math.min(box.left - outer.left + box.width / 2 - tip.offsetWidth / 2,
                                      outer.width - tip.offsetWidth - 8));
    let top = box.bottom - outer.top + 10;
    if (top + tip.offsetHeight > outer.height - 4) top = box.top - outer.top - tip.offsetHeight - 10;
    tip.style.left = `${left}px`;
    tip.style.top = `${top}px`;
  };
  const light = (node) => {
    lit = node;
    const around = near(node.id.slice(2));
    svg.classList.add("focus");
    for (const n of nodes) n.classList.toggle("lit", around.nodes.has(n.id.slice(2)));
    for (const el of svg.querySelectorAll("[data-edge]")) {
      el.classList.toggle("lit", around.edges.has(el.dataset.edge));
    }
    show(node);
  };
  const unlight = () => {
    lit = null;
    svg.classList.remove("focus");
    for (const el of svg.querySelectorAll(".lit")) el.classList.remove("lit");
    tip.hidden = true;
  };
  const toggle = (node) => (lit === node ? unlight() : light(node));

  for (const node of nodes) {
    node.addEventListener("click", (event) => {
      event.stopPropagation();
      toggle(node);
    });
    node.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        toggle(node);
      }
    });
  }
  svg.addEventListener("click", unlight);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && lit) unlight();
  });
})();
