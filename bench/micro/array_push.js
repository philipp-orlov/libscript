// Appending to an array.
(function() {
  var N = 5000000, t0 = Date.now(), a = [];
  for (var i = 0; i < N; i++) a.push(i);
  var t1 = Date.now();
  if (a.length < 0) print(a);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
