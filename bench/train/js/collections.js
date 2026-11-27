// Map, Set, WeakMap, arrays with callbacks, sorting, object keys.
function Item(id) { this.id = id; this.name = 'item' + id; this.score = (id * 7919) % 1013; }
var items = [];
for (var i = 0; i < 3000; i++) items.push(new Item(i));
var total = 0;
for (var round = 0; round < 20; round++) {
  var byName = new Map(), byObj = new WeakMap(), seen = new Set(), counts = {};
  for (var i = 0; i < items.length; i++) { var it = items[i]; byName.set(it.name, it); byObj.set(it, i); seen.add(it.score); counts[it.score % 50] = (counts[it.score % 50] || 0) + 1; }
  for (var [k, v] of byName) total += k.length + v.id;
  for (var s of seen) total += s & 3;
  for (var v of byName.values()) total += byObj.get(v) & 1;
  for (var k of byName.keys()) if (byName.has(k + 'x')) total++;
  byName.forEach(function (v, k) { total += v.score > 1000 ? 1 : 0; });
  for (var k in counts) total += counts[k];
  total += Object.keys(counts).length + Object.values(counts).reduce(function (a, b) { return a + b; }, 0) + Object.entries(counts)[3][1];
  var sorted = items.slice().sort(function (a, b) { return a.score - b.score || a.id - b.id; });
  var top = sorted.filter(function (x) { return x.score % 3 == 0; }).map(function (x) { return x.id; }).slice(0, 100);
  total += top.indexOf(sorted[0].id) + top.includes(5) + [...seen].length + Array.from(byName.keys()).length + top.find(function (x) { return x > 2000; }) + top.some(function (x) { return x < 0; }) + top.every(Number.isInteger);
  for (var i = 0; i < 400; i++) { byName.delete('item' + i * 3); seen.delete(i); }
  total += byName.size + seen.size + [3, 1, 2].concat(top.slice(0, 3)).reverse().join('').length + new Set(top).has(top[5]) + Math.max.apply(null, top) + Math.min(...top.slice(0, 20));
  var grid = []; for (var r = 0; r < 30; r++) { var row = []; for (var c = 0; c < 30; c++) row.push(r * c % 7 + 0.5); grid.push(row); }
  var acc = 0; for (var r = 0; r < 30; r++) for (var c = 0; c < 30; c++) { var sacc = 0; for (var k = 0; k < 30; k++) sacc += grid[r][k] * grid[k][c]; acc += sacc; }
  total += acc | 0;
  var q = []; for (var i = 0; i < 2000; i++) { q.push(i); if (i % 3 == 0) q.shift(); if (i % 7 == 0) q.pop(); if (i % 11 == 0) q.unshift(i); if (i % 97 == 0) q.splice(1, 2, i, i); }
  total += q.length;
}
console.log(total);
