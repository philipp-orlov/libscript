// Objects made and dropped: trees with parent links, lists, closures, records with strings.
function Node(key, parent) { this.key = key; this.left = null; this.right = null; this.parent = parent; this.payload = { text: 'leaf ' + key, list: [key, key + 1, key + 2] }; }
function insert(root, key) { if (!root) return new Node(key, null); var p = root; for (;;) { if (key < p.key) { if (!p.left) { p.left = new Node(key, p); break; } p = p.left; } else { if (!p.right) { p.right = new Node(key, p); break; } p = p.right; } } return root; }
function depthOf(n) { var d = 0; while (n.parent) { n = n.parent; d++; } return d; }
function find(root, key) { var p = root; while (p && p.key != key) p = key < p.key ? p.left : p.right; return p; }
function counter(start) { var n = start; return { inc: function () { return ++n; }, add: function (k) { n += k; return n; }, get: function () { return n; } }; }
var total = 0;
for (var round = 0; round < 40; round++) {
  var root = null, x = 12345 + round;
  for (var i = 0; i < 3000; i++) { x = (x * 1103515245 + 12345) & 0x7fffffff; root = insert(root, x % 100003); }
  for (var i = 0; i < 3000; i += 7) { var n = find(root, (i * 7919) % 100003); total += n ? depthOf(n) + n.payload.list[1] : 1; }
  var head = null; for (var i = 0; i < 5000; i++) head = { v: i, next: head, tag: i % 2 ? 'odd' : 'even' };
  var len = 0; for (var p = head; p; p = p.next) len += p.tag.length;
  var cs = []; for (var i = 0; i < 500; i++) cs.push(counter(i)); for (var i = 0; i < 500; i++) total += cs[i].inc() + cs[(i * 7) % 500].add(2) + cs[i].get();
  var recs = []; for (var i = 0; i < 2000; i++) recs.push({ id: i, name: 'n' + i, pos: { x: i * 0.5, y: -i }, vel: [1, 2], alive: i % 3 != 0 });
  var moved = recs.filter(function (r) { return r.alive; }).map(function (r) { return { id: r.id, pos: { x: r.pos.x + r.vel[0], y: r.pos.y + r.vel[1] } }; });
  total += len + moved.length + moved[10].pos.x;
  var bound = find.bind(null, root), partial = Math.max.bind(Math, 5); total += (bound(root.key) === root ? 1 : 0) + partial(3) + (function () { return arguments.length + arguments[1]; })(1, 2, 3);
}
console.log(total);
