// Making a closure and calling it.
(function() {
  function mk(n) { return function() { return n; }; }
  var N = 3000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) s += mk(i)();
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
