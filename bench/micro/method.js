// A method call through a property of an object.
(function() {
  var o = { n: 1, get: function() { return this.n; } };
  var N = 5000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) s += o.get();
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
