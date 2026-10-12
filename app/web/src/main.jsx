import { render } from "preact";
import { App } from "./app.jsx";
import "./styles.css";

const root = document.getElementById("app");
root.replaceChildren();  // the page's static intro (for crawlers and the first paint) gives way to the app
render(<App />, root);
