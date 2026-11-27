// Generators, iterators, destructuring, spread and rest.
function* range(a, b, step) { for (var i = a; i < b; i += step || 1) yield i; }
function* take(it, n) { var k = 0; for (var v of it) { if (k++ >= n) return; yield v; } }
function* mapG(it, f) { for (var v of it) yield f(v); }
function* tree(depth) { if (depth == 0) { yield 1; return; } yield* tree(depth - 1); yield depth; yield* tree(depth - 1); }
class Bag { constructor() { this.items = []; } add(x) { this.items.push(x); return this; } *[Symbol.iterator]() { for (var i = 0; i < this.items.length; i++) yield this.items[i]; } entries() { var i = 0, self = this; return { next: function () { return i < self.items.length ? { value: [i, self.items[i++]], done: false } : { value: undefined, done: true }; }, [Symbol.iterator]: function () { return this; } }; } }
function sumAll(...xs) { var s = 0; for (var x of xs) s += x; return s; }
var total = 0;
for (var round = 0; round < 60; round++) {
  for (var v of take(mapG(range(0, 100000, 3), function (x) { return x * 2; }), 400)) total += v & 7;
  for (var v of tree(8)) total += v;
  var bag = new Bag(); for (var i = 0; i < 200; i++) bag.add({ k: 'k' + i, v: [i, i + 1], w: { z: i } });
  for (var { k, v: [a, b], w: { z } } of bag) total += k.length + a + b + z;
  for (var [i, e] of bag.entries()) total += i + e.v[0];
  var arr = [...range(0, 50)], [first, second, ...rest] = arr;
  total += sumAll(...arr) + first + second + rest.length + sumAll(...rest.slice(0, 5), ...[1, 2, 3]);
  var g = range(0, 5), r; while (!(r = g.next()).done) total += r.value;
  var o = { a: 1, b: 2, c: 3, d: 4 }, { a, ...others } = o, merged = { ...others, e: 5, ...{ f: 6 } };
  total += a + Object.keys(merged).length + Math.max(...arr.slice(0, 10));
  var it = [10, 20, 30][Symbol.iterator](); total += it.next().value + Array.from({ length: 5 }, function (_, i) { return i * i; })[4] + Array.from(new Set(arr)).length;
  var early = 0; for (var v of range(0, 1000)) { if (v > 50) break; early += v; } total += early;
}
console.log(total);
