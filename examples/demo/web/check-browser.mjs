// The real page in a real browser (not part of the checks: it needs
// Chromium, which the Nix sandbox does not have). From the repository:
//
//   nix build .#demo-web
//   nix shell nixpkgs#chromium nixpkgs#nodejs -c \
//     node examples/demo/web/check-browser.mjs result examples/demo/expected.txt
//
// It serves the directory on 127.0.0.1, starts headless Chromium, and over
// the DevTools protocol:
//   1. loads index.html in check mode (?frames=N&step=...&script=...) for
//      every run of expected.txt and compares the hash the page puts in its
//      title;
//   2. loads it normally and checks that frames advance at about 60 a
//      second; that keys 3, V and T switch scale, precise vertices and
//      perspective (the canvas becomes 960 x 720, their buttons pressed);
//      that W makes it 16:9 (1281 x 720) and back; that in a 390 x 844
//      window (a phone) the 3x frame is shown at its 1x width or less and
//      the page does not scroll sideways; that tapping the Fog button turns
//      fog off (its button no longer pressed); that Space on the focused
//      Fog button presses it (fog on again) and does not pause; that Space
//      elsewhere pauses and Right then steps one frame.
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

const profile = mkdtempSync(join(tmpdir(), "demo-chromium-"));
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
const press = async (key, code) => {
  for (const type of ["keyDown", "keyUp"])
    await send("Input.dispatchKeyEvent", { type, key, code,
      text: type === "keyDown" && key.length === 1 ? key : undefined });
};
const state = async () => JSON.parse(await evaluate(`JSON.stringify({
  status: document.getElementById("status").textContent,
  w: document.getElementById("screen").width, h: document.getElementById("screen").height,
  css: document.getElementById("screen").getBoundingClientRect().width,
  scrollW: document.documentElement.scrollWidth, innerW: window.innerWidth,
  pressed: [...document.querySelectorAll("button[aria-pressed=true]")].map(b => b.dataset.key)})`));
const frameOf = s => Number((/frame (\d+)/.exec(s.status) || [])[1]);

try {
  // 1. Check mode, every run.
  const runs = readFileSync(expectedPath, "utf8").split("\n")
    .filter(l => l.trim() && !l.startsWith("#")).map(l => l.trim().split(/\s+/));
  for (const [name, frames, step, scale, precise, perspective, dither, wide, flt, cache, fog, script,
    hash] of runs) {
    await send("Page.navigate", { url: `${base}?frames=${frames}&step=${step}&scale=${scale}` +
      `&precise=${precise}&perspective=${perspective}&dither=${dither}&widescreen=${wide}` +
      `&float=${flt}&clutcache=${cache}&fog=${fog}&script=${encodeURIComponent(script)}` });
    let title = "";
    for (let i = 0; i < 1200 && !/^demo ([0-9a-f]{16}|failed)$/.test(title); i++) {
      await sleep(100);
      title = await evaluate("document.title");
    }
    report(title === "demo " + hash, `${name}: the page's hash ${title.slice(5) || "(none)"}` +
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
  await press("3", "Digit3"); await press("v", "KeyV"); await press("t", "KeyT");
  await sleep(300);
  const c = await state();
  report(c.w === 960 && c.h === 720 && ["3", "v", "t"].every(p => c.pressed.includes(p)),
    `keys 3 V T: canvas ${c.w} x ${c.h}, pressed: ${c.pressed.join(" ")}`);
  await press("w", "KeyW");
  await sleep(300);
  const w1 = await state();
  await press("w", "KeyW");
  await sleep(300);
  const w2 = await state();
  report(w1.w === 1281 && w1.h === 720 && w2.w === 960 && w1.pressed.includes("w"),
    `key W: canvas ${w1.w} x ${w1.h}, then ${w2.w} x ${w2.h}`);
  // A phone: the frame fits the width, and nothing scrolls sideways.
  await send("Emulation.setDeviceMetricsOverride",
    { width: 390, height: 844, deviceScaleFactor: 3, mobile: true });
  await sleep(500);
  const p = await state();
  report(p.css <= 320 && p.css > 200 && p.scrollW <= p.innerW,
    `a 390 x 844 phone: 3x frame shown ${p.css} CSS pixels wide, page ${p.scrollW} wide in ${p.innerW}`);
  await evaluate(`document.querySelector("[data-key=f]").click()`);
  await sleep(200);
  const f = await state();
  report(!f.pressed.includes("f") && p.pressed.includes("f"), `the Fog button: fog off (pressed: ${f.pressed.join(" ")})`);
  await send("Emulation.clearDeviceMetricsOverride");
  // Space on a focused button presses the button (fog on again), and does
  // not pause.
  await evaluate(`document.querySelector("[data-key=f]").focus()`);
  await press(" ", "Space");
  await sleep(300);
  const sp = await state();
  report(sp.pressed.includes("f") && !/paused/.test(sp.status),
    `Space on the focused Fog button: pressed ${sp.pressed.join(" ")}; ${sp.status}`);
  await evaluate(`document.activeElement.blur()`);
  await press(" ", "Space");
  await sleep(300);
  const d = await state();
  await sleep(800);
  const e = await state();
  report(frameOf(d) === frameOf(e) && /paused/.test(e.status), `Space pauses: ${d.status} / ${e.status}`);
  await press("ArrowRight", "ArrowRight");
  await sleep(300);
  const g = await state();
  report(frameOf(g) === frameOf(e) + 1, `Right steps one frame: ${e.status} / ${g.status}`);
} finally {
  ws.close();
  browser.kill();
  server.close();
  await sleep(200);
  rmSync(profile, { recursive: true, force: true });
}
process.exit(failures ? 1 : 0);
