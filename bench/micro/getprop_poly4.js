// The same read of objects of four shapes.
(function() {
  var os = [{ a: 1, b: 2 }, { b: 2, a: 1 }, { c: 0, b: 2 }, { d: 0, e: 0, b: 2 }];
  var N = 8000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) s += os[i & 3].b;
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
