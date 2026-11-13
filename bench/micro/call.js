// A call of a small function and its return.
(function() {
  function f(a, b) { return a + b; }
  var N = 5000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) s = f(s, i);
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
