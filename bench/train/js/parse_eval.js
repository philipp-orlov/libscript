// Front end: source text that is new each round (names change), compiled by eval and new Function, partly run.
function makeSource(round) {
  var s = [], p = 'm' + round + '_';
  for (var i = 0; i < 60; i++) {
    var n = p + i;
    s.push('function ' + n + '(a, b) { var t = a * ' + (i + 1) + ' + b; if (t > 1000) { t = t % 1000; } else if (t < 0) t = -t; for (var k = 0; k < 3; k++) t += k * b; return t; }');
    s.push('var ' + n + 'o = { id: ' + i + ', name: "' + n + '", tags: ["x", "y", ' + i + '], nested: { a: [1, 2, 3], b: null, c: /a+b*[c-f]?/g }, get twice() { return this.id * 2; }, run: function (q) { return ' + n + '(q, this.id); } };');
    s.push('class ' + n + 'C extends Object { constructor(v) { super(); this.v = v; this.w = `v=${v}`; } get half() { return this.v / 2; } static make(v) { return new ' + n + 'C(v); } add(o) { return ' + n + 'C.make(this.v + o.v); } }');
    s.push('var ' + n + 'f = (x, ...rest) => { try { if (x === undefined) throw new Error("no x"); return rest.length ? x + rest[0] : x; } catch (e) { return -1; } finally { x = 0; } };');
    s.push('var ' + n + 'g = function* (lim) { let i = 0; while (i < lim) { yield i++; } }; var ' + n + 'd = ({ a, b: [c, d = 4] = [], ...others }) => a + c + d;');
    s.push('switch (' + i + ' % 4) { case 0: ' + n + 'o.k = 1; break; case 1: ' + n + 'o.k = "s"; break; default: ' + n + 'o.k = [' + i + ']; }');
  }
  s.push('var total = 0; for (var j = 0; j < 60; j += 7) { total += eval("' + p + '" + j + "(j, 3)") + eval("' + p + '" + j + "o").run(j) + eval("' + p + '" + j + "C").make(j).add({ v: 1 }).half + eval("' + p + '" + j + "f")(j, 1) + eval("' + p + '" + j + "d")({ a: 1, b: [2] }); } total;');
  return s.join('\n');
}
var sum = 0;
for (var round = 0; round < 40; round++) {
  sum += (0, eval)(makeSource(round));
  var f = new Function('a', 'b', 'var r = 0; for (var i = 0; i < a; i++) { r += (i ^ b) & 15; } return r + ' + round + ';');
  sum += f(50, round);
}
console.log(sum);
