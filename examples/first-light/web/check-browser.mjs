// The real page in a real browser, in headless Chromium: the flake's
// checks.browser-first-light (nix build -L .#checks.x86_64-linux.browser-first-light).
// By hand, from the repository:
//
//   nix build .#first-light-web
//   nix shell nixpkgs#chromium nixpkgs#nodejs -c \
//     node examples/first-light/web/check-browser.mjs result examples/first-light/expected.txt
//
// It serves the directory on 127.0.0.1, starts headless Chromium, and over
// the DevTools protocol:
//   1. loads index.html in check mode (?frames=N&scale=...) for every run of
//      expected.txt and compares the hash the page puts in its title;
//   2. loads it normally and checks that frames advance (and, with
//      JB_CHECK_FPS=1 in the environment, at about 60 a second: not in the
//      flake check, where a busy machine may be slower), that keys 3, V
//      and T switch scale, precise vertices and perspective (the canvas
//      becomes 960 x 720), that W makes it 16:9 (1281 x 720) and back, that
//      in a 600 x 800 window too small for the 3x frame it is shown at its
//      1x size in either shape (427 or 320 CSS pixels wide), and that Space
//      pauses.
// The exit status is 1 on any failure, Chromium or its DevTools connection
// going away included; every step has a deadline, and so has the whole run
// (15 minutes), so that it fails rather than hangs.
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

// Deadlines, so that the check fails rather than hangs: the whole run,
// Chromium's start and its DevTools connection, and each DevTools request.
const TOTAL_MS = 15 * 60 * 1000, START_MS = 60 * 1000, REQUEST_MS = 60 * 1000;
const withDeadline = (promise, ms, what) => {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${what}: nothing in ${ms / 1000} s`)), ms);
  })]).finally(() => clearTimeout(timer));
};

let server, browser, ws, profile;
let browserExit = null; // how Chromium stopped, once it has
const pending = new Map();
const failPending = why => {
  for (const { reject } of pending.values()) reject(new Error(why));
  pending.clear();
};
let cleaned = false;
const cleanup = async () => {
  if (cleaned) return;
  cleaned = true;
  failPending("the check is stopping");
  ws?.close();
  browser?.kill("SIGKILL");
  server?.close();
  server?.closeAllConnections();
  await sleep(200);
  if (profile) rmSync(profile, { recursive: true, force: true });
};
const overall = setTimeout(async () => {
  report(false, `the check did not finish in ${TOTAL_MS / 60000} minutes`);
  await cleanup();
  process.exit(1);
}, TOTAL_MS);

// DevTools requests over the page's WebSocket; each settles, with its
// answer, at its deadline, or when Chromium or the connection goes.
let id = 0;
const send = (method, params = {}) => {
  if (browserExit) return Promise.reject(new Error(browserExit));
  const n = ++id;
  return withDeadline(new Promise((resolve, reject) => {
    pending.set(n, { resolve, reject });
    ws.send(JSON.stringify({ id: n, method, params }));
  }), REQUEST_MS, method).finally(() => pending.delete(n));
};
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
  // A static server for the page, its module and its wasm.
  const types = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm" };
  server = createServer((req, res) => {
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

  profile = mkdtempSync(join(tmpdir(), "first-light-chromium-"));
  browser = spawn(chromium, ["--headless=new", "--no-sandbox", "--disable-gpu",
    `--user-data-dir=${profile}`, "--remote-debugging-port=0", "about:blank"],
    { stdio: ["ignore", "ignore", "pipe"] });
  let err = "";
  const wsUrl = await withDeadline(new Promise((resolve, reject) => {
    browser.stderr.on("data", d => {
      err += d;
      const m = /DevTools listening on (ws:\/\/\S+)/.exec(err);
      if (m) resolve(m[1]);
    });
    browser.on("error", e => reject(new Error(`could not start ${chromium}: ${e.message}`)));
    browser.on("exit", (code, signal) => {
      browserExit = `chromium exited early (${signal || "status " + code})`;
      failPending(browserExit);
      reject(new Error(`${browserExit}:\n${err}`));
    });
  }), START_MS, "chromium's DevTools address");

  // One page target, driven over its WebSocket.
  const http = wsUrl.replace(/^ws/, "http").replace(/\/devtools\/.*/, "");
  const target = await (await fetch(`${http}/json/new?about:blank`,
    { method: "PUT", signal: AbortSignal.timeout(START_MS) })).json();
  ws = new WebSocket(target.webSocketDebuggerUrl);
  ws.addEventListener("message", e => {
    const m = JSON.parse(e.data);
    const p = pending.get(m.id);
    if (p) { pending.delete(m.id); p.resolve(m); }
  });
  await withDeadline(new Promise((resolve, reject) => {
    ws.addEventListener("open", resolve);
    ws.addEventListener("error", () => reject(new Error("could not connect to chromium's DevTools")));
  }), START_MS, "the DevTools connection");
  ws.addEventListener("close", () => failPending(browserExit || "chromium closed the DevTools connection"));

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
  // Once the page shows a frame, its frames advance (a slow or busy machine
  // gets few animation callbacks, each catching up at most four frames).
  const until = Date.now() + 20000;
  let a;
  do { await sleep(100); a = await state(); } while (Number.isNaN(frameOf(a)) && Date.now() < until);
  let b = a;
  while (!(frameOf(b) >= frameOf(a) + 10) && Date.now() < until) { await sleep(100); b = await state(); }
  report(frameOf(b) >= frameOf(a) + 10, `frames advance: ${frameOf(a)} then ${frameOf(b)}`);
  if (process.env.JB_CHECK_FPS) {
    await sleep(1000);
    const a = await state();
    await sleep(1000);
    const b = await state();
    const rate = frameOf(b) - frameOf(a);
    report(rate >= 30 && rate <= 70, `about 60 frames a second: ${frameOf(a)} then ${frameOf(b)} a second later`);
  }
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
} catch (e) {
  report(false, e.message);
} finally {
  await cleanup();
  clearTimeout(overall);
}
process.exit(failures ? 1 : 0);
