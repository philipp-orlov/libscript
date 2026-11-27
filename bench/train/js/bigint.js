// BigInt: products, quotients and remainders of 256-bit numbers, powers, text.
var P = 2n ** 255n - 19n, L = 2n ** 252n + 27742317777372353535851937790883648493n;
function mod(a, m) { var r = a % m; return r >= 0n ? r : r + m; }
function powMod(b, e, m) { var r = 1n; b = mod(b, m); while (e > 0n) { if (e & 1n) r = r * b % m; b = b * b % m; e >>= 1n; } return r; }
function inv(a, m) { var [x, y, u, v, b] = [0n, 1n, 1n, 0n, m]; a = mod(a, m); while (a !== 0n) { var q = b / a, r = b % a, mm = x - u * q, n = y - v * q; b = a; a = r; x = u; y = v; u = mm; v = n; } return mod(x, m); }
var acc = 0n, a = 0x1234567890abcdef1234567890abcdefn, b = 987654321987654321987654321n;
for (var round = 0; round < 60; round++) {
  for (var i = 0; i < 200; i++) { a = mod(a * b + BigInt(i), P); b = mod(b * b - a, L); acc += (a ^ b) & 0xffffn; }
  acc += powMod(a, 65537n, P) % 1000n + inv(b, P) % 1000n + (a << 13n) % 977n + (b >> 7n) % 977n + BigInt.asUintN(64, a * b) % 977n + (a > b ? 1n : 0n) + (-a / 12345n) % 7n;
  acc += BigInt(a.toString(16).length + b.toString().length) + BigInt('0x' + a.toString(16)) % 3n + BigInt(Number(a % 1000000n)) + (a * a * a) % 1000003n;
}
console.log(acc.toString());
