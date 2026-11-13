// Throwing and catching.
(function() {
  function t(i) { if (i >= 0) throw i; }
  var N = 300000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) { try { t(i); } catch (e) { s += e; } }
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
