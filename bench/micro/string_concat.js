// Building a string a piece at a time.
(function() {
  var N = 2000000, t0 = Date.now(), s = '';
  for (var i = 0; i < N; i++) s += 'x';
  var t1 = Date.now();
  if (s.length < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
