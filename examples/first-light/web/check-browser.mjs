// The real page in a real browser (not part of the checks: it needs
// Chromium, which the Nix sandbox does not have). From the repository:
//
//   nix build .#first-light-web
//   nix shell nixpkgs#chromium nixpkgs#nodejs -c \
//     node examples/first-light/web/check-browser.mjs result examples/first-light/expected.txt
//
// It serves the directory on 127.0.0.1, starts headless Chromium, and over
// the DevTools protocol:
//   1. loads index.html in check mode (?frames=N&scale=...) for every run of
//      expected.txt and compares the hash the page puts in its title;
//   2. loads it normally and checks that frames advance at about 60 a
//      second, that keys 3, V and T switch scale, precise vertices and
//      perspective (the canvas becomes 960 x 720), that W makes it 16:9
//      (1281 x 720) and back, that in a 600 x 800 window too small for the
//      3x frame it is shown at its 1x size in either shape (427 or 320 CSS
//      pixels wide), and that Space pauses.
// The exit status is 1 on any failure.
import { spawn } from "node:child_process";
import { readFileSync, mkdtempSync, rmSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join, extname } from "node:path";

const [dir, expectedPath, chromium = "chromium"] = process.argv.slice(2);
if (!expectedPath) {
  console.error("usage: node check-browser.mjs WEB_DIR expected.txt [chromium]");
  process.exit(2);
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
let failures = 0;
const report = (ok, msg) => { console.log((ok ? "ok   " : "FAIL ") + msg); if (!ok) failures++; };

// A static server for the page, its module and its wasm.
const types = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm" };
const server = createServer((req, res) => {
  const name = new URL(req.url, "http://x").pathname.slice(1) || "index.html";
  try {
    const body = readFileSync(join(dir, name));
    res.writeHead(200, { "content-type": types[extname(name)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404).end();
  }
});
await new Promise(r => server.listen(0, "127.0.0.1", r));
const base = `http://127.0.0.1:${server.address().port}/index.html`;

const profile = mkdtempSync(join(tmpdir(), "first-light-chromium-"));
const browser = spawn(chromium, ["--headless=new", "--no-sandbox", "--disable-gpu",
  `--user-data-dir=${profile}`, "--remote-debugging-port=0", "about:blank"],
  { stdio: ["ignore", "ignore", "pipe"] });
const wsUrl = await new Promise((resolve, reject) => {
  let err = "";
  browser.stderr.on("data", d => {
    err += d;
    const m = /DevTools listening on (ws:\/\/\S+)/.exec(err);
    if (m) resolve(m[1]);
  });
  browser.on("exit", () => reject(new Error("chromium exited:\n" + err)));
});

// One page target, driven over its WebSocket.
const http = wsUrl.replace(/^ws/, "http").replace(/\/devtools\/.*/, "");
const target = await (await fetch(`${http}/json/new?about:blank`, { method: "PUT" })).json();
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener("open", r));
let id = 0;
const pending = new Map();
ws.addEventListener("message", e => {
  const m = JSON.parse(e.data);
  if (pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
});
const send = (method, params = {}) =>
  new Promise(r => { pending.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
const evaluate = async expr =>
  (await send("Runtime.evaluate", { expression: expr, returnByValue: true })).result.result.value;
const press = async key => {
  for (const type of ["keyDown", "keyUp"])
    await send("Input.dispatchKeyEvent", { type, key, text: type === "keyDown" ? key : undefined });
};
const state = async () => JSON.parse(await evaluate(`JSON.stringify({
  status: document.getElementById("status").textContent,
  w: document.getElementById("screen").width, h: document.getElementById("screen").height,
  pressed: [...document.querySelectorAll("button[aria-pressed=true]")].map(b => b.dataset.scale
    ? "scale " + b.dataset.scale : b.dataset.switch || b.id)})`));
const frameOf = s => Number((/frame (\d+)/.exec(s.status) || [])[1]);

try {
  // 1. Check mode, every run.
  const runs = readFileSync(expectedPath, "utf8").split("\n")
    .filter(l => l.trim() && !l.startsWith("#")).map(l => l.trim().split(/\s+/));
  for (const [name, frames, scale, precise, perspective, dither, wide, flt, cache, hash] of runs) {
    await send("Page.navigate", { url: `${base}?frames=${frames}&scale=${scale}&precise=${precise}` +
      `&perspective=${perspective}&dither=${dither}&widescreen=${wide}&float=${flt}` +
      `&clutcache=${cache}` });
    let title = "";
    for (let i = 0; i < 600 && !/^first-light [0-9a-f]{16}$/.test(title); i++) {
      await sleep(100);
      title = await evaluate("document.title");
    }
    report(title === "first-light " + hash, `${name}: the page's hash ${title.slice(12) || "(none)"}` +
      `, expected.txt ${hash}`);
  }

  // 2. The interactive page.
  await send("Page.navigate", { url: base });
  await sleep(2000);
  const a = await state();
  await sleep(1000);
  const b = await state();
  const rate = frameOf(b) - frameOf(a);
  report(rate >= 30 && rate <= 70, `frames advance: ${frameOf(a)} then ${frameOf(b)} a second later`);
  await press("3"); await press("v"); await press("t");
  await sleep(300);
  const c = await state();
  report(c.w === 960 && c.h === 720 && ["scale 3", "precise", "perspective"].every(p => c.pressed.includes(p)),
    `keys 3 V T: canvas ${c.w} x ${c.h}, on: ${c.pressed.join(", ")}`);
  await press("w");
  await sleep(300);
  const w1 = await state();
  await press("w");
  await sleep(300);
  const w2 = await state();
  report(w1.w === 1281 && w1.h === 720 && w2.w === 960 && w1.pressed.includes("widescreen"),
    `key W: canvas ${w1.w} x ${w1.h}, then ${w2.w} x ${w2.h}`);
  // A window too small for the 3x frame: the 1x size, in either shape.
  await send("Emulation.setDeviceMetricsOverride",
    { width: 600, height: 800, deviceScaleFactor: 1, mobile: false });
  await press("w");
  await sleep(300);
  const cssWide = await evaluate(`document.getElementById("screen").style.width`);
  await press("w");
  await sleep(300);
  const cssNarrow = await evaluate(`document.getElementById("screen").style.width`);
  await send("Emulation.clearDeviceMetricsOverride");
  await sleep(300);
  report(cssWide === "427px" && cssNarrow === "320px",
    `a 600 x 800 window: 3x shown ${cssWide} wide in 16:9, ${cssNarrow} in 4:3`);
  await press(" ");
  await sleep(200);
  const d = await state();
  await sleep(800);
  const e = await state();
  report(frameOf(d) === frameOf(e) && /paused/.test(e.status), `Space pauses: ${d.status} / ${e.status}`);
} finally {
  ws.close();
  browser.kill();
  server.close();
  await sleep(200);
  rmSync(profile, { recursive: true, force: true });
}
process.exit(failures ? 1 : 0);
