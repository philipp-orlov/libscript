// Runs the probes in this engine: a warm-up, then the best of `reps` timed runs of each. One line per probe:
//   name <TAB> group <TAB> checksum <TAB> milliseconds
"use strict";
const now = typeof performance !== "undefined" && performance.now ? () => performance.now() : () => Date.now();
const out = typeof console !== "undefined" ? s => console.log(s) : print;
async function main() {
  const reps = 3;
  for (const p of globalThis.__probes) {
    let sum = await p.fn(Math.max(1, Math.floor(p.n / 10)));
    let best = Infinity;
    for (let r = 0; r < reps; r++) { const t0 = now(); sum = await p.fn(p.n); const t1 = now(); if (t1 - t0 < best) best = t1 - t0; }
    out(p.name + "\t" + p.group + "\t" + sum + "\t" + best.toFixed(1));
  }
}
main();
