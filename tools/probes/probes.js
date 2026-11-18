// Probes that compare engines on work that cannot be thrown away: every probe returns a checksum of what it computed, the
// harness prints it, and `run.py` checks that every engine printed the same one. Inputs come from a generator the engines
// cannot see through, and results are kept (in rings, maps, arrays) or folded into the checksum.
//
// Groups: "temp" probes make objects that never leave the loop body (an engine with escape analysis removes them); every
// other probe keeps what it makes or uses it for something an optimizer cannot fold away.
"use strict";

let seed = 12345;
function rnd() { seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff; return seed; }
function mix(h, v) { return (Math.imul(h, 31) + v) | 0; }
const words = [];
for (let i = 0; i < 512; i++) { let w = ""; const len = 3 + (rnd() % 8); for (let j = 0; j < len; j++) w += String.fromCharCode(97 + (rnd() % 26)); words.push(w); }
const text = (() => { let t = []; for (let i = 0; i < 2000; i++) t.push(words[rnd() % 512]); return t.join(" "); })();

const probes = [];
function probe(group, name, n, fn) { probes.push({ group, name, n, fn }); }

// ---- allocation -------------------------------------------------------------------------------------------------------
probe("alloc", "object literal, kept in a ring", 5000000, n => {
  const ring = new Array(256).fill(null); let h = 0;
  for (let i = 0; i < n; i++) { const o = { a: i, b: i ^ 7, c: h & 255, d: 1 }; ring[i & 255] = o; h = mix(h, o.a + o.c); }
  for (let i = 0; i < 256; i++) h = mix(h, ring[i].b);
  return h;
});
probe("alloc", "array literal of 6, kept in a ring", 4000000, n => {
  const ring = new Array(256).fill(null); let h = 0;
  for (let i = 0; i < n; i++) { const a = [i, i + 1, h & 15, 3, 4, 5]; ring[i & 255] = a; h = mix(h, a[2] + a.length); }
  for (let i = 0; i < 256; i++) h = mix(h, ring[i][0]);
  return h;
});
probe("alloc", "closure, kept in a ring", 2500000, n => {
  const ring = new Array(256).fill(null); let h = 0;
  for (let i = 0; i < n; i++) { const k = i & 31; ring[i & 255] = () => k + h; h = mix(h, ring[(i * 7) & 255] ? ring[(i * 7) & 255]() : 0); }
  return h;
});
probe("alloc", "constructor with 3 fields, kept", 1200000, n => {
  function P(x, y, z) { this.x = x; this.y = y; this.z = z; }
  const ring = new Array(256).fill(null); let h = 0;
  for (let i = 0; i < n; i++) { const p = new P(i, h & 63, i & 3); ring[i & 255] = p; h = mix(h, p.x + p.y); }
  return h;
});
probe("temp", "vec3 math with temporary objects", 1200000, n => {
  const add = (a, b) => ({ x: a.x + b.x, y: a.y + b.y, z: a.z + b.z });
  const mul = (a, s) => ({ x: a.x * s, y: a.y * s, z: a.z * s });
  const dot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
  let v = { x: 1, y: 2, z: 3 }, acc = 0;
  for (let i = 0; i < n; i++) { const w = add(mul(v, 0.5), { x: i & 7, y: 1, z: 2 }); acc += dot(w, w) % 97; v = { x: w.y % 13, y: w.z % 17, z: (w.x + 1) % 19 }; }
  return Math.round(acc);
});
probe("temp", "pair returned and destructured", 3000000, n => {
  const divmod = (a, b) => [Math.floor(a / b), a % b];
  let h = 0;
  for (let i = 1; i <= n; i++) { const [q, r] = divmod(i * 7 + (h & 255), 13); h = mix(h, q + r); }
  return h;
});

// ---- calls and properties ------------------------------------------------------------------------------------------------
probe("calls", "polymorphic method call (4 classes)", 2000000, n => {
  class A { constructor(v) { this.v = v; } m(x) { return this.v + x; } }
  class B { constructor(v) { this.v = v; } m(x) { return this.v - x; } }
  class C { constructor(v) { this.w = 1; this.v = v; } m(x) { return this.v ^ x; } }
  class D { constructor(v) { this.v = v; this.w = 2; } m(x) { return (this.v * 3 + x) | 0; } }
  const objs = []; for (let i = 0; i < 64; i++) objs.push([new A(i), new B(i), new C(i), new D(i)][i & 3]);
  let h = 0; for (let i = 0; i < n; i++) h = mix(h, objs[i & 63].m(i));
  return h;
});
probe("calls", "inherited and absent properties", 12000000, n => {
  function Base() { this.own = 1; } Base.prototype.inherited = 5;
  function Mid() { Base.call(this); } Mid.prototype = Object.create(Base.prototype);
  function Leaf() { Mid.call(this); this.x = 2; } Leaf.prototype = Object.create(Mid.prototype);
  const objs = []; for (let i = 0; i < 16; i++) objs.push(new Leaf());
  let h = 0; for (let i = 0; i < n; i++) { const o = objs[i & 15]; h = mix(h, o.inherited + (o.nothere === undefined ? 1 : 2) + o.own); }
  return h;
});
probe("calls", "rest, spread, arguments, bind", 18000000, n => {
  const rest = (...xs) => xs.length + xs[0];
  const args = function () { return arguments.length + arguments[0]; };
  const three = (a, b, c) => a + b + c;
  const bound = three.bind(null, 1);
  const arr = [1, 2, 3]; let h = 0;
  for (let i = 0; i < n; i++) h = mix(h, rest(i, 2, 3) + args(i, 1) + three(...arr) + bound(i, 2));
  return h;
});
probe("calls", "closures over mutable state", 9000000, n => {
  function counter() { let c = 0; return { inc: () => ++c, get: () => c }; }
  const cs = []; for (let i = 0; i < 8; i++) cs.push(counter());
  for (let i = 0; i < n; i++) cs[i & 7].inc();
  let h = 0; for (const c of cs) h = mix(h, c.get());
  return h;
});
probe("calls", "recursion (fib, tak)", 1, n => {
  const fib = k => k < 2 ? k : fib(k - 1) + fib(k - 2);
  const tak = (x, y, z) => y >= x ? z : tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y));
  return fib(31) + tak(22, 14, 6);
});
probe("calls", "try/catch with throws", 400000, n => {
  let h = 0;
  for (let i = 0; i < n; i++) { try { if ((i & 7) === 0) throw new Error("e" + i); h = mix(h, i); } catch (e) { h = mix(h, e.message.length); } finally { h ^= 1; } }
  return h;
});

// ---- classes ------------------------------------------------------------------------------------------------------------
probe("classes", "class hierarchy, super, accessors, #private", 800000, n => {
  class Shape { #id; constructor(id) { this.#id = id; } get id() { return this.#id; } area() { return 0; } describe() { return this.id + this.area(); } }
  class Rect extends Shape { constructor(id, w, h) { super(id); this.w = w; this.h = h; } area() { return this.w * this.h; } }
  class Sq extends Rect { constructor(id, s) { super(id, s, s); } describe() { return super.describe() + 1; } }
  const shapes = []; for (let i = 0; i < 128; i++) shapes.push(i & 1 ? new Sq(i, i & 15) : new Rect(i, i & 7, 3));
  let h = 0; for (let i = 0; i < n; i++) h = mix(h, shapes[i & 127].describe());
  for (let i = 0; i < n / 8; i++) { const s = new Sq(i, i & 7); h = mix(h, s.describe()); }
  return h;
});

// ---- strings -----------------------------------------------------------------------------------------------------------
probe("string", "number to string, concatenation, template", 400000, n => {
  let h = 0; for (let i = 0; i < n; i++) { const s = "k" + i + ":" + (i & 63) + `|${i % 97}`; h = mix(h, s.length + s.charCodeAt(s.length >> 1)); }
  return h;
});
probe("string", "split, join, slice, indexOf, replace", 600, n => {
  let h = 0; for (let i = 0; i < n; i++) { const parts = text.split(" "); h = mix(h, parts.length); h = mix(h, parts.slice(i & 31, 100).join("-").length); h = mix(h, text.indexOf(words[i & 511])); h = mix(h, text.replace(/a/g, "A").length); }
  return h;
});
probe("string", "string keys: Map and object as hash", 400000, n => {
  const m = new Map(), o = {}; let h = 0;
  for (let i = 0; i < n; i++) { const k = words[i & 511] + (i & 63); m.set(k, i); o[k] = i; if ((i & 15) === 0) { m.delete(words[(i >> 4) & 511] + 3); delete o[words[(i >> 4) & 511] + 3]; } h = mix(h, (m.get(words[(i * 3) & 511] + 5) | 0) + (o[words[(i * 5) & 511] + 7] | 0)); }
  return mix(h, m.size + Object.keys(o).length);
});
probe("string", "charCodeAt scan and String.fromCharCode", 600, n => {
  let h = 0; for (let r = 0; r < n; r++) { let s = ""; for (let i = 0; i < text.length; i += 3) { s += String.fromCharCode(97 + ((text.charCodeAt(i) + r) % 26)); } h = mix(h, s.length + s.charCodeAt(r & 63)); }
  return h;
});

// ---- regexp, json -------------------------------------------------------------------------------------------------------
probe("regexp", "test, exec, match, replace", 300, n => {
  let h = 0; const re = /\b([a-m]+)(\w*)\b/g;
  for (let r = 0; r < n; r++) {
    h = mix(h, (text.match(/[aeiou]{2}/g) || []).length);
    let m, c = 0; re.lastIndex = 0; while ((m = re.exec(text)) !== null) c += m[1].length; h = mix(h, c);
    h = mix(h, text.replace(/(\w)(\w)/g, "$2$1").length);
    for (let i = 0; i < 64; i++) if (/^[a-f]+\d*$/.test(words[(i * 7 + r) & 511])) h++;
  }
  return h;
});
probe("json", "stringify and parse nested data", 600, n => {
  const data = { items: [], meta: { name: "probe", tags: ["a", "b", "c"], n: 42 } };
  for (let i = 0; i < 200; i++) data.items.push({ id: i, name: words[i], score: i * 1.5, ok: !(i & 1), nested: { a: [i, i + 1], b: null } });
  let h = 0; for (let r = 0; r < n; r++) { const s = JSON.stringify(data); const back = JSON.parse(s); h = mix(h, s.length + back.items.length + back.items[r % 200].id); }
  return h;
});

// ---- collections and arrays -------------------------------------------------------------------------------------------
probe("collections", "Map and Set with integer keys", 600000, n => {
  const m = new Map(), s = new Set(); let h = 0;
  for (let i = 0; i < n; i++) { const k = (i * 2654435761) & 4095; m.set(k, (m.get(k) | 0) + 1); s.add(k ^ 5); if (s.has(i & 4095)) h++; }
  for (const [k, v] of m) h = mix(h, k + v);
  for (const v of s) h = mix(h, v);
  return h;
});
probe("collections", "array map/filter/reduce/sort", 150, n => {
  const base = []; for (let i = 0; i < 10000; i++) base.push(rnd() % 100000);
  let h = 0;
  for (let r = 0; r < n; r++) {
    const a = base.map(x => x + r).filter(x => (x & 1) === 0); h = mix(h, a.reduce((p, c) => (p + c) | 0, 0));
    const b = base.slice(0, 3000).sort((x, y) => x - y); h = mix(h, b[r % 3000]);
    h = mix(h, base.indexOf(base[(r * 37) % 10000]) + (base.some(x => x === r) ? 1 : 0));
  }
  return h;
});
probe("collections", "push/pop/shift/splice queue", 1200000, n => {
  const q = []; let h = 0;
  for (let i = 0; i < n; i++) { q.push(i); q.push(i + 1); if (q.length > 64) { h = mix(h, q.shift()); h = mix(h, q.pop()); } if ((i & 255) === 0) q.splice(3, 2, i); }
  return mix(h, q.length);
});
probe("collections", "for-in, Object.keys/entries, spread, destructuring", 150000, n => {
  const proto = { shared: 1 }; let h = 0;
  for (let i = 0; i < n; i++) {
    const o = Object.create(proto); o.a = i; o.b = i & 3; o.c = 7;
    let c = 0; for (const k in o) c += k.length;
    const copy = { ...o, d: i }; const { a, b, ...others } = copy; const [x, y] = Object.entries(others)[0];
    h = mix(h, c + a + b + Object.keys(copy).length + x.length + y);
  }
  return h;
});

// ---- numeric ----------------------------------------------------------------------------------------------------------
probe("numeric", "typed arrays: matrix multiply and sieve", 18, n => {
  const N = 96, A = new Float64Array(N * N), B = new Float64Array(N * N), C = new Float64Array(N * N);
  for (let i = 0; i < N * N; i++) { A[i] = (i % 17) * 0.5; B[i] = (i % 13) - 3; }
  let h = 0;
  for (let r = 0; r < n; r++) {
    for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) { let s = 0; for (let k = 0; k < N; k++) s += A[i * N + k] * B[k * N + j]; C[i * N + j] = s; }
    h = mix(h, Math.round(C[(r * 37) % (N * N)]));
    const sieve = new Uint8Array(200000); let cnt = 0; for (let i = 2; i < 200000; i++) { if (!sieve[i]) { cnt++; for (let j = i * 2; j < 200000; j += i) sieve[j] = 1; } } h = mix(h, cnt);
  }
  return h;
});
probe("numeric", "double math and Math functions", 1500000, n => {
  let x = 0.5, acc = 0; for (let i = 0; i < n; i++) { x = Math.sin(x * 3.7 + i * 0.001) * 0.5 + 0.5; acc += Math.sqrt(x) + Math.floor(x * 10) + Math.abs(x - 0.3); }
  return Math.round(acc / 1000);  // not the digits Math.sin may differ in
});
probe("numeric", "int32 hash and bit twiddling", 20000000, n => {
  let h = 0x811c9dc5; for (let i = 0; i < n; i++) { h ^= i & 255; h = Math.imul(h, 16777619); h = (h << 5) | (h >>> 27); }
  return h | 0;
});

// ---- iteration and async ---------------------------------------------------------------------------------------------
probe("iteration", "generators and for-of", 300000, n => {
  function* range(k) { for (let i = 0; i < k; i++) yield i; }
  function* mapG(g, f) { for (const x of g) yield f(x); }
  let h = 0; for (let r = 0; r < n / 100; r++) for (const v of mapG(range(100), x => x * 3 + r)) h = mix(h, v);
  for (const [i, v] of [10, 20, 30].entries()) h = mix(h, i + v);
  return h;
});
probe("iteration", "async/await and promise chains", 180000, async n => {
  let h = 0;
  const step = async v => { await null; return v + 1; };
  for (let i = 0; i < n; i++) { h = mix(h, await step(i)); if ((i & 63) === 0) h = mix(h, await Promise.resolve(i).then(v => v * 2)); }
  return h;
});

// ---- interpreter-style workload ---------------------------------------------------------------------------------------
probe("program", "bytecode interpreter loop (switch)", 36, n => {
  const code = new Int32Array(64); for (let i = 0; i < 64; i++) code[i] = (i * 7 + 3) % 6;
  let h = 0;
  for (let r = 0; r < n; r++) {
    let pc = 0, acc = r, reg0 = 1, reg1 = 2, steps = 0;
    while (steps < 400000) {
      switch (code[pc]) { case 0: acc = (acc + reg0) | 0; break; case 1: reg0 = (reg0 * 3 + 1) | 0; break; case 2: acc ^= reg1; break; case 3: reg1 = (reg1 + acc) | 0; break; case 4: acc = (acc << 1) | (acc >>> 31); break; default: reg0 ^= pc; }
      pc = (pc + 1) & 63; steps++;
    }
    h = mix(h, acc ^ reg0 ^ reg1);
  }
  return h;
});
probe("program", "linked list and tree (allocation, pointer chasing)", 30, n => {
  class Node { constructor(v, l, r) { this.v = v; this.l = l; this.r = r; } }
  const build = d => d === 0 ? null : new Node(d, build(d - 1), build(d - 1));
  const sum = t => t === null ? 0 : t.v + sum(t.l) + sum(t.r);
  let h = 0; for (let r = 0; r < n; r++) { const t = build(14); h = mix(h, sum(t)); }
  return h;
});

globalThis.__probes = probes;
