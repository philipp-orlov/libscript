// A read of an own property of an object of one shape.
(function() {
  var o = { a: 1, b: 2, c: 3 };
  var N = 10000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) s += o.b;
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
