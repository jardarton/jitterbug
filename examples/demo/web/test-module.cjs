// The page's own module under Node:
//   node test-module.cjs demo.js index.html expected.txt
// 1. Every module function index.html calls (m._dm_web_*) exists, and so
//    does the HEAPU8 view it reads the frames through.
// 2. Each run of expected.txt, run by the page's own code (its runTicks,
//    parseScript and hash, taken from index.html: the part of its script
//    before it touches the page), through the module's functions as the
//    page calls them, gives the hash demo-headless gives natively.
"use strict";
const fs = require("fs");
const path = require("path");

const [modulePath, pagePath, expectedPath] = process.argv.slice(2);
if (!expectedPath) {
  console.error("usage: node test-module.cjs demo.js index.html expected.txt");
  process.exit(2);
}
const page = fs.readFileSync(pagePath, "utf8");
let failures = 0;
const fail = msg => { console.log("FAIL " + msg); failures++; };

const begin = page.indexOf("function fnv1a64"), end = page.indexOf("const canvas");
if (begin < 0 || end < begin) {
  console.log("FAIL index.html: no pure part (function fnv1a64 ... const canvas)");
  process.exit(1);
}
const { runTicks, parseScript } =
  new Function(page.slice(begin, end) + "\nreturn { runTicks, parseScript };")();

// name frames step scale precise perspective dither widescreen float
// clutcache fog script hash
const runs = fs.readFileSync(expectedPath, "utf8").split("\n")
  .filter(l => l.trim() !== "" && !l.startsWith("#"))
  .map(l => {
    const c = l.trim().split(/\s+/);
    if (c.length !== 13) throw new Error(expectedPath + ": a run needs 13 columns: " + l);
    return { name: c[0], frames: +c[1], step: +c[2], sw: c.slice(3, 11).map(Number),
      script: c[11], hash: c[12].toLowerCase() };
  });
if (runs.length === 0)
  throw new Error(expectedPath + ": no runs");

require(path.resolve(modulePath))().then(m => {
  const called = [...new Set([...page.matchAll(/\bm\.(_dm_web_\w+)/g)].map(x => x[1]))];
  if (called.length === 0) fail("index.html calls no m._dm_web_* function");
  for (const name of called) {
    if (typeof m[name] !== "function") fail("the module has no function " + name);
    else console.log("module function " + name + ": present");
  }
  if (!(m.HEAPU8 instanceof Uint8Array)) fail("the module has no HEAPU8");
  if (failures) process.exit(1);

  for (const run of runs) {
    const r = m._dm_web_init(...run.sw);
    if (r !== 0) { fail(run.name + ": the scene did not start, jb_result " + r); continue; }
    let got;
    try {
      got = runTicks(m, run.frames, run.step, parseScript(run.script));
    } catch (e) {
      fail(run.name + ": " + e.message);
      continue;
    }
    const ok = got === run.hash;
    console.log(run.name.padEnd(8) + " " + String(run.frames).padStart(4) + " ticks, step " +
      run.step + ", switches " + run.sw.join(" ") + ", script " + run.script + ": " + got +
      (ok ? " ok" : " FAIL, want " + run.hash));
    if (!ok) failures++;
  }
  process.exit(failures ? 1 : 0);
}, e => { console.log("FAIL the module did not load: " + e); process.exit(1); });
