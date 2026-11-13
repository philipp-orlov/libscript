// A counted loop: the cost of one iteration (compare, add, jump).
(function() {
  var N = 20000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) s += i;
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
