// Recursion: fib(30), nanoseconds per call.
(function() {
  function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
  var t0 = Date.now(), r = fib(30), t1 = Date.now();
  if (r < 0) print(r);
  print(((t1 - t0) * 1e6 / 2692537).toFixed(2));
})();
