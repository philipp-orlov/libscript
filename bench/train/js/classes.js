// Classes, inheritance, accessors, private fields, many shapes at one site, proxies, JSON, exceptions.
class Shape { #id; static count = 0; constructor(id) { this.#id = id; Shape.count++; } get id() { return this.#id; } area() { return 0; } describe() { return this.constructor.name + '#' + this.#id + ':' + this.area().toFixed(1); } static is(o) { return #id in o; } }
class Rect extends Shape { constructor(id, w, h) { super(id); this.w = w; this.h = h; } area() { return this.w * this.h; } }
class Square extends Rect { constructor(id, s) { super(id, s, s); this.kind = 'sq'; } get side() { return this.w; } set side(v) { this.w = this.h = v; } }
class Circle extends Shape { constructor(id, r) { super(id); this.r = r; } area() { return 3.14159 * this.r * this.r; } }
class Tri extends Shape { constructor(id, b, h) { super(id); this.b = b; this.h = h; this.extra = null; } area() { return this.b * this.h / 2; } }
function visit(node, v) { var f = v['visit' + node.type]; return f ? f.call(v, node) : 0; }
var visitor = { visitNum: function (n) { return n.value; }, visitAdd: function (n) { return visit(n.left, this) + visit(n.right, this); }, visitMul: function (n) { return visit(n.left, this) * visit(n.right, this); }, visitNeg: function (n) { return -visit(n.arg, this); } };
function build(d) { if (d == 0) return { type: 'Num', value: d + 1.5 }; var k = d % 3; return k == 0 ? { type: 'Add', left: build(d - 1), right: build(d - 1) } : k == 1 ? { type: 'Mul', left: build(d - 1), right: { type: 'Num', value: 1.01 } } : { type: 'Neg', arg: build(d - 1) }; }
var total = 0, shapes = [];
for (var i = 0; i < 2000; i++) shapes.push(i % 4 == 0 ? new Rect(i, 2, i % 9) : i % 4 == 1 ? new Square(i, i % 5) : i % 4 == 2 ? new Circle(i, i % 3) : new Tri(i, 3, i % 7));
for (var round = 0; round < 30; round++) {
  for (var i = 0; i < shapes.length; i++) { var s = shapes[i]; total += s.area() + s.id + (s instanceof Rect ? 1 : 0) + (Shape.is(s) ? 1 : 0); if (s instanceof Square) { s.side = s.side + 1; s.side = s.side - 1; } }
  total += shapes[round].describe().length + visit(build(9), visitor);
  var keys = ['w', 'h', 'r', 'b', 'kind', 'missing'], hit = 0;
  for (var i = 0; i < shapes.length; i += 2) for (var k = 0; k < keys.length; k++) { var val = shapes[i][keys[k]]; if (val !== undefined) hit++; if (keys[k] in shapes[i]) hit++; }
  total += hit;
  var log = 0, target = { a: 1, b: { c: 2 } }, p = new Proxy(target, { get: function (t, k, r) { log++; return Reflect.get(t, k, r); }, has: function (t, k) { log++; return k in t; }, set: function (t, k, v) { t[k] = v; return true; } });
  for (var i = 0; i < 300; i++) { total += p.a + p.b.c + ('a' in p ? 1 : 0); p.z = i; }
  total += log;
  var data = { list: shapes.slice(0, 200).map(function (s) { return { id: s.id, area: s.area(), tags: ['t' + s.id, null, true], nested: { deep: [1.5, 'two', { three: 3 }] } }; }), meta: { round: round, name: 'r"\n' + round } };
  var text = JSON.stringify(data), back = JSON.parse(text);
  total += text.length + back.list.length + back.list[7].nested.deep[2].three + JSON.stringify(back, null, 2).length % 1000 + Object.assign({}, back.meta, { x: 1 }).x;
  for (var i = 0; i < 400; i++) { try { if (i % 5 == 0) throw new RangeError('r' + i); if (i % 7 == 0) null.x; total += i & 1; } catch (e) { total += e.message.length > 3 ? 2 : 1; } finally { total++; } }
  var frozen = Object.freeze({ a: 1 }), desc = Object.getOwnPropertyDescriptor(Square.prototype, 'side'); total += Object.isFrozen(frozen) + typeof desc.get + Object.getPrototypeOf(Square) === Rect ? 1 : 0;
}
console.log(Math.round(total) + Shape.count);
