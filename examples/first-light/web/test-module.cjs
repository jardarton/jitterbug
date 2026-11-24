// The page's own module under Node:
//   node test-module.cjs first-light.js index.html expected.txt
// 1. Every module function index.html calls (m._fl_web_*) exists, and so
//    does the HEAPU8 view it reads the frames through.
// 2. Each run of expected.txt, drawn through those functions the way the
//    page draws (fl_web_init once, then fl_web_set_switches, fl_web_draw
//    and the frame's bytes from HEAPU8) and hashed by the page's own hash
//    function (taken from index.html), gives the hash first-light-headless
//    gives natively.
"use strict";
const fs = require("fs");
const path = require("path");

const [modulePath, pagePath, expectedPath] = process.argv.slice(2);
if (!expectedPath) {
  console.error("usage: node test-module.cjs first-light.js index.html expected.txt");
  process.exit(2);
}
const page = fs.readFileSync(pagePath, "utf8");
let failures = 0;
const fail = msg => { console.log("FAIL " + msg); failures++; };

// The page's hash: its script up to the first line that touches the DOM.
const script = page.slice(page.indexOf("function fnv1a64"), page.indexOf("const canvas"));
const { fnv1a64, FNV_BASIS, hex } =
  new Function(script + "\nreturn { fnv1a64, FNV_BASIS, hex };")();

const runs = fs.readFileSync(expectedPath, "utf8").split("\n")
  .filter(l => l.trim() !== "" && !l.startsWith("#"))
  .map(l => {
    const [name, frames, scale, precise, perspective, dither, wide, flt, cache, hash] =
      l.trim().split(/\s+/);
    // Digits only, 1 to 1,000,000, as main_headless.c reads it.
    const count = /^[0-9]{1,9}$/.test(frames) ? +frames : 0;
    if (count < 1 || count > 1000000)
      throw new Error(expectedPath + ": run " + name + ": bad frame count " + frames);
    return { name, frames: count,
      sw: [+scale, +precise, +perspective, +dither, +wide, +flt, +cache], hash };
  });
if (runs.length === 0)
  throw new Error(expectedPath + ": no runs");

require(path.resolve(modulePath))().then(m => {
  const called = [...new Set([...page.matchAll(/\bm\.(_fl_web_\w+)/g)].map(x => x[1]))];
  if (called.length === 0) fail("index.html calls no m._fl_web_* function");
  for (const name of called) {
    if (typeof m[name] !== "function") fail("the module has no function " + name);
    else console.log("module function " + name + ": present");
  }
  if (!(m.HEAPU8 instanceof Uint8Array)) fail("the module has no HEAPU8");
  if (failures) process.exit(1);

  runs.forEach((run, i) => {
    const r = i === 0 ? m._fl_web_init(...run.sw) : m._fl_web_set_switches(...run.sw);
    if (r !== 0) { fail(run.name + ": switches refused, jb_result " + r); return; }
    const h = FNV_BASIS.slice();
    for (let f = 0; f < run.frames; f++) {
      const ptr = m._fl_web_draw(f);
      if (!ptr) { fail(run.name + ": frame " + f + " failed"); return; }
      const w = m._fl_web_width(), hgt = m._fl_web_height();
      fnv1a64(h, m.HEAPU8.subarray(ptr, ptr + w * hgt * 4));
    }
    const got = hex(h);
    const ok = got === run.hash.toLowerCase();
    console.log(run.name.padEnd(10) + " " + String(run.frames).padStart(4) + " frames, switches " +
      run.sw.join(" ") + ": " + got + (ok ? " ok" : " FAIL, want " + run.hash));
    if (!ok) failures++;
  });
  process.exit(failures ? 1 : 0);
}, e => { console.log("FAIL the module did not load: " + e); process.exit(1); });
