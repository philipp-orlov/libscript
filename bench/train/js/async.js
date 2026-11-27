// Promises, async functions, async generators.
function delay(v) { return new Promise(function (res) { res(v); }); }
async function step(x) { var a = await delay(x); var b = await delay(a + 1); if (b % 97 == 0) throw new Error('boom'); return a + b; }
async function chain(n) { var s = 0; for (var i = 0; i < n; i++) { try { s += await step(i); } catch (e) { s -= 1; } } return s; }
async function* ticks(n) { for (var i = 0; i < n; i++) { await null; yield i * 2; } }
async function consume(n) { var s = 0; for await (var v of ticks(n)) s += v; return s; }
class Queue { constructor() { this.waiters = []; this.items = []; } push(x) { if (this.waiters.length) this.waiters.shift()(x); else this.items.push(x); } pop() { var self = this; return this.items.length ? Promise.resolve(this.items.shift()) : new Promise(function (res) { self.waiters.push(res); }); } }
async function producer(q, n) { for (var i = 0; i < n; i++) { q.push(i); if (i % 8 == 0) await null; } }
async function consumer(q, n) { var s = 0; for (var i = 0; i < n; i++) s += await q.pop(); return s; }
var total = 0;
async function main() {
  for (var round = 0; round < 30; round++) {
    total += await chain(300);
    total += await consume(300);
    var all = await Promise.all([delay(1), step(2), chain(20), Promise.resolve(4).then(function (v) { return v * 2; }), Promise.race([delay(7), delay(8)])]);
    total += all.reduce(function (a, b) { return a + b; }, 0);
    var settled = await Promise.allSettled([Promise.reject(new Error('x')), delay(3)]);
    total += settled.length + (settled[0].status == 'rejected' ? 1 : 0);
    var q = new Queue(), c = consumer(q, 400); await producer(q, 400); total += await c;
    var p = Promise.resolve(0); for (var i = 0; i < 400; i++) p = p.then(function (v) { return v + 1; }).catch(function () { return 0; }).finally(function () {}); total += await p;
    try { await Promise.reject(new TypeError('t')); } catch (e) { total += e instanceof TypeError ? 1 : 0; }
  }
  console.log(total);
}
main();
