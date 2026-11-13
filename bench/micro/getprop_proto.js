// A read of a property that lives on the prototype.
(function() {
  function A() { this.own = 1; }
  A.prototype.inherited = 2;
  var a = new A();
  var N = 8000000, s = 0, t0 = Date.now();
  for (var i = 0; i < N; i++) s += a.inherited;
  var t1 = Date.now();
  if (s < 0) print(s);
  print(((t1 - t0) * 1e6 / N).toFixed(2));
})();
