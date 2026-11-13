// A constructor, an instance, a method call on it.
(function() {
  function P(x, y) { this.x = x; this.y = y; }
  P.prototype.sum = function() { return this.x + this.y; };
  var N = 2000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) { var p = new P(i, 2); s += p.sum(); }
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
