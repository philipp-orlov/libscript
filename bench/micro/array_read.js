// Reading elements of an array of numbers.
(function() {
  var a = []; for (var k = 0; k < 1000; k++) a.push(k);
  var N = 10000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) s += a[i % 1000];
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
