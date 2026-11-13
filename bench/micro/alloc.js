// An object with two properties, made and dropped.
(function() {
  var N = 3000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) { var p = { x: i, y: 2 }; s += p.y; }
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
